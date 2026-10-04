/* Same-process RocksDB readrandom observer for UUSE root-cause experiments. */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <numeric>
#include <optional>
#include <pthread.h>
#include <random>
#include <sched.h>
#include <sstream>
#include <string>
#include <sys/syscall.h>
#include <thread>
#include <utility>
#include <vector>

#include "uuse/diagnostics.hpp"
#include "rocksdb/cache.h"
#include "rocksdb/db.h"
#include "rocksdb/uuse_filesystem.hpp"
#include "rocksdb/options.h"
#include "rocksdb/statistics.h"
#include "rocksdb/table.h"
#include "rocksdb/version.h"

namespace {

struct Options {
    std::string mode = "uuse";
    std::string dbPath;
    std::string configPath;
    std::uint64_t keys = 160000;
    std::uint32_t threads = 8;
    std::uint32_t keySize = 16;
    std::uint32_t valueSize = 100;
    std::uint64_t cacheSize = 32ULL * 1024ULL * 1024ULL;
    std::uint32_t blockSize = 4096;
    bool directReads = false;
    bool compression = true;
    bool dropOsCacheBeforeRead = false;
    bool skipFill = false;
    bool skipCompaction = false;
    std::uint32_t rounds = 3;
    std::string workerCpus = "2,3";
    std::string diskName;
};

struct DiskStats {
    std::uint64_t readIos = 0;
    std::uint64_t readSectors = 0;
};

struct RoundResult {
    std::uint64_t operations = 0;
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
    double seconds = 0.0;
    std::uint64_t p50 = 0;
    std::uint64_t p95 = 0;
    std::uint64_t p99 = 0;
    std::uint64_t p999 = 0;
    rocksdb::Tickers blockCacheHit = rocksdb::BLOCK_CACHE_HIT;
    std::uint64_t blockCacheHitCount = 0;
    std::uint64_t blockCacheMissCount = 0;
    std::uint64_t blockDataHitCount = 0;
    std::uint64_t blockDataMissCount = 0;
    std::uint64_t nvmeReadIos = 0;
    std::uint64_t nvmeReadBytes = 0;
    uuse::diagnostics::Snapshot uuse;
};

struct WorkerStat {
    std::uint64_t tid = 0;
    std::uint64_t cpuNs = 0;
    std::uint64_t voluntarySwitches = 0;
    std::uint64_t involuntarySwitches = 0;
};

/* Builds the same binary key representation as uuse_db_bench. */
std::string makeKey(std::uint64_t index, std::uint32_t keySize) {
    std::string key(keySize, '\0');
    const std::uint32_t bytes = std::min<std::uint32_t>(8, keySize);
    for (std::uint32_t byte = 0; byte < bytes; ++byte) {
        key[byte] = static_cast<char>((index >> ((bytes - 1 - byte) * 8)) & 0xFF);
    }
    return key;
}

/* Builds the same deterministic value representation as uuse_db_bench. */
std::string makeValue(std::uint64_t index, std::uint32_t valueSize) {
    std::string value(valueSize, 'v');
    const std::uint32_t bytes = std::min<std::uint32_t>(8, valueSize);
    for (std::uint32_t byte = 0; byte < bytes; ++byte) {
        value[byte] = static_cast<char>((index >> (byte * 8)) & 0xFF);
    }
    return value;
}

std::vector<unsigned int> parseCpus(const std::string &text) {
    std::vector<unsigned int> result;
    std::stringstream stream(text);
    std::string item;
    while (std::getline(stream, item, ',')) {
        if (!item.empty()) result.push_back(static_cast<unsigned int>(std::stoul(item)));
    }
    return result;
}

/* Keeps benchmark workers on the requested two CPUs. */
void pinWorker(unsigned int cpu) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    const int rc = pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
    std::fprintf(stderr, "[CPU MAP] rocksdb worker: tid=%llu configured_cpu=%u actual_cpu=%d "
                         "pin=%s\n",
                 static_cast<unsigned long long>(::syscall(SYS_gettid)), cpu, sched_getcpu(),
                 rc == 0 ? "ok" : "failed");
}

/* Reads scheduler switch counters for one benchmark worker. */
std::pair<std::uint64_t, std::uint64_t> threadSwitches(std::uint64_t tid) {
    std::ifstream input("/proc/self/task/" + std::to_string(tid) + "/status");
    std::string line;
    std::uint64_t voluntary = 0;
    std::uint64_t involuntary = 0;
    while (std::getline(input, line)) {
        if (line.rfind("voluntary_ctxt_switches:", 0) == 0) {
            voluntary = std::stoull(line.substr(line.find_last_of(" \t") + 1));
        } else if (line.rfind("nonvoluntary_ctxt_switches:", 0) == 0) {
            involuntary = std::stoull(line.substr(line.find_last_of(" \t") + 1));
        }
    }
    return {voluntary, involuntary};
}

/* Returns CPU time consumed by the current worker thread. */
std::uint64_t threadCpuNs() {
    struct timespec value {};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &value);
    return static_cast<std::uint64_t>(value.tv_sec) * 1000000000ULL + value.tv_nsec;
}

bool parseArguments(int argc, char **argv, Options *options) {
    for (int index = 1; index < argc; ++index) {
        const std::string value = argv[index];
        const auto split = value.find('=');
        if (split == std::string::npos) return false;
        const std::string name = value.substr(0, split);
        const std::string arg = value.substr(split + 1);
        try {
            if (name == "--mode") options->mode = arg;
            else if (name == "--db") options->dbPath = arg;
            else if (name == "--uuse-config") options->configPath = arg;
            else if (name == "--keys") options->keys = std::stoull(arg);
            else if (name == "--threads") options->threads = std::stoul(arg);
            else if (name == "--key-size") options->keySize = std::stoul(arg);
            else if (name == "--value-size") options->valueSize = std::stoul(arg);
            else if (name == "--cache-size") options->cacheSize = std::stoull(arg);
            else if (name == "--block-size") options->blockSize = std::stoul(arg);
            else if (name == "--direct-reads") options->directReads = arg == "1";
            else if (name == "--compression") options->compression = arg != "none";
            else if (name == "--drop-os-cache-before-read") options->dropOsCacheBeforeRead = arg == "1";
            else if (name == "--skip-fill") options->skipFill = arg == "1";
            else if (name == "--skip-compaction") options->skipCompaction = arg == "1";
            else if (name == "--rounds") options->rounds = std::stoul(arg);
            else if (name == "--worker-cpus") options->workerCpus = arg;
            else if (name == "--disk") options->diskName = arg;
            else return false;
        } catch (...) {
            return false;
        }
    }
    return !options->dbPath.empty() && options->threads != 0 && options->keys != 0 &&
           (options->mode == "uuse" || options->mode == "ext4-buffered" ||
                                    options->mode == "ext4-direct");
}

DiskStats readDiskStats(const std::string &diskName) {
    std::ifstream input("/proc/diskstats");
    std::string line;
    while (std::getline(input, line)) {
        std::istringstream stream(line);
        unsigned int major = 0;
        unsigned int minor = 0;
        std::string name;
        DiskStats value;
        if (!(stream >> major >> minor >> name)) continue;
        if (name != diskName) continue;
        std::uint64_t merged = 0;
        std::uint64_t readTicks = 0;
        stream >> value.readIos >> merged >> value.readSectors >> readTicks;
        return value;
    }
    return {};
}

std::uint64_t percentile(std::vector<std::uint64_t> *values, double fraction) {
    if (values->empty()) return 0;
    const std::size_t position = static_cast<std::size_t>(fraction * (values->size() - 1));
    std::nth_element(values->begin(), values->begin() + position, values->end());
    return (*values)[position];
}

void printSnapshot(const uuse::diagnostics::Snapshot &value) {
    std::cout << "  UUSE frontend_read_ops=" << value.frontendReadOps
              << " frontend_read_bytes=" << value.frontendReadBytes
              << " request_4k=" << value.frontendRequest4k
              << " request_8k=" << value.frontendRequest8k
              << " request_other=" << value.frontendRequestOther
              << " frontend_cache_hit=" << value.frontendCacheHit
              << " frontend_cache_miss=" << value.frontendCacheMiss
              << " frontend_submit_ops=" << value.frontendSubmitOps
              << " direct_worker_read_ops=" << value.directWorkerReadOps
              << " direct_worker_read_bytes=" << value.directWorkerReadBytes
              << " direct_worker_completion_ops=" << value.directWorkerCompletionOps
              << '\n';
    std::cout << "  UUSE planner_read_requests=" << value.plannerReadRequests
              << " planner_request_bytes=" << value.plannerReadRequestBytes
              << " planner_child_bytes=" << value.plannerReadChildBytes
              << " planner_span_bytes=" << value.plannerReadSpanBytes
              << " coalesced_children=" << value.plannerReadCoalescedChildren
              << " direct_children=" << value.plannerReadDirectChildren
              << " head_rmw_children=" << value.plannerReadHeadRmwChildren
              << " tail_rmw_children=" << value.plannerReadTailRmwChildren
              << " zero_children=" << value.plannerReadZeroChildren
              << " span_4k=" << value.plannerSpan4k
              << " span_8k=" << value.plannerSpan8k
              << " span_other=" << value.plannerSpanOther << '\n';
    std::cout << "  UUSE data_read_submit=" << value.dataReactorReadSubmit
              << " data_read_complete=" << value.dataReactorReadComplete
              << " queue_depth_sum=" << value.dataReactorQueueDepthSum
              << " queue_depth_max=" << value.dataReactorQueueDepthMax << '\n';
    std::cout << "  UUSE bdev_data_read_ops=" << value.bdevDataReadOps
              << " bdev_data_read_bytes=" << value.bdevDataReadBytes
              << " bdev_block_bytes=" << value.bdevDataBlockBytes
              << " completion_count=" << value.completionCount
              << " completion_wait_count=" << value.completionWaitCount
              << " completion_wait_ns=" << value.completionWaitNs << '\n';
    std::cout << "  UUSE frontend_queue_wait_ns=" << value.frontendQueueWaitNs
              << " frontend_io_ns=" << value.frontendIoNs
              << " read_bounce_children=" << value.readBounceChildren
              << " read_bounce_bytes=" << value.readBounceBytes
              << " read_direct_user_children=" << value.readDirectUserChildren
              << " read_direct_user_bytes=" << value.readDirectUserBytes << '\n'
              << "  UUSE read_copy_out_ops=" << value.readCopyOutOps
              << " read_copy_out_bytes=" << value.readCopyOutBytes
              << " read_copy_out_ns=" << value.readCopyOutNs << '\n';
}

rocksdb::Status configureDatabase(const Options &options, rocksdb::Options *dbOptions,
                                  std::unique_ptr<rocksdb::Env> *ownedEnv) {
    dbOptions->create_if_missing = true;
    dbOptions->max_background_jobs = static_cast<int>(options.threads);
    dbOptions->write_buffer_size = 64ULL * 1024ULL * 1024ULL;
    dbOptions->allow_mmap_reads = false;
    dbOptions->use_direct_reads = options.directReads || options.mode == "uuse";
    dbOptions->use_direct_io_for_flush_and_compaction = true;
    dbOptions->compression = options.compression ? rocksdb::kSnappyCompression
                                                  : rocksdb::kNoCompression;
    dbOptions->statistics = rocksdb::CreateDBStatistics();
    rocksdb::BlockBasedTableOptions tableOptions;
    tableOptions.block_size = options.blockSize;
    tableOptions.block_cache = rocksdb::NewLRUCache(options.cacheSize);
    dbOptions->table_factory = std::shared_ptr<rocksdb::TableFactory>(
        rocksdb::NewBlockBasedTableFactory(tableOptions));
    if (options.mode == "uuse") {
        *ownedEnv = uuse::rocksdb_adapter::createCompositeEnv(options.configPath);
        if (!*ownedEnv) return rocksdb::Status::IOError("cannot create UUSE Env");
        dbOptions->env = ownedEnv->get();
    }
    return rocksdb::Status::OK();
}

bool fillAndCompact(const Options &options, rocksdb::DB *db) {
    const auto cpus = parseCpus(options.workerCpus);
    const std::uint64_t perWorker = options.keys / options.threads;
    if (perWorker * options.threads != options.keys || cpus.empty()) return false;
    rocksdb::WriteOptions writeOptions;
    std::atomic<bool> failed{false};
    std::vector<std::thread> workers;
    for (std::uint32_t worker = 0; worker < options.threads; ++worker) {
        workers.emplace_back([&, worker] {
            pinWorker(cpus[worker % cpus.size()]);
            for (std::uint64_t offset = 0; offset < perWorker; ++offset) {
                const std::uint64_t index = worker * perWorker + offset;
                if (!db->Put(writeOptions, makeKey(index, options.keySize),
                             makeValue(index, options.valueSize)).ok()) {
                    failed.store(true, std::memory_order_relaxed);
                    return;
                }
            }
        });
    }
    for (auto &worker : workers) worker.join();
    if (failed.load(std::memory_order_relaxed)) return false;
    rocksdb::FlushOptions flushOptions;
    rocksdb::Status status = db->Flush(flushOptions);
    if (!status.ok()) return false;
    if (options.skipCompaction) {
        std::cout << "Skipped compaction after flush (diagnostic mode)\n";
        return true;
    }
    rocksdb::CompactRangeOptions compactOptions;
    status = db->CompactRange(compactOptions, nullptr, nullptr);
    return status.ok();
}

void printDbShape(const Options &options, rocksdb::DB *db) {
    std::string value;
    std::uint64_t estimate = 0;
    db->GetIntProperty("rocksdb.estimate-num-keys", &estimate);
    std::uint64_t memtable = 0;
    db->GetIntProperty("rocksdb.cur-size-all-mem-tables", &memtable);
    std::uint64_t immutable = 0;
    db->GetIntProperty("rocksdb.num-immutable-mem-table", &immutable);
    std::vector<rocksdb::LiveFileMetaData> files;
    db->GetLiveFilesMetaData(&files);
    std::uint64_t sstBytes = 0;
    std::size_t sstFiles = 0;
    for (const auto &file : files) {
        if (file.name.size() >= 4 && file.name.substr(file.name.size() - 4) == ".sst") {
            ++sstFiles;
            sstBytes += file.size;
        }
    }
    std::cout << "DB shape: keys_written=" << options.keys << " key_range=["
              << makeKey(0, options.keySize).size() << "-byte 0, " << options.keys - 1
              << "] estimate_num_keys=" << estimate << " memtable_bytes=" << memtable
              << " immutable_memtables=" << immutable << " sst_files=" << sstFiles
              << " sst_bytes=" << sstBytes << '\n';
}

/* Drops Linux page cache after population so the first buffered round is cold-ish. */
bool dropOsCacheBeforeRead(const Options &options) {
    if (!options.dropOsCacheBeforeRead || options.mode == "uuse") return true;
    std::system("sync");
    std::ofstream output("/proc/sys/vm/drop_caches");
    if (!output) {
        std::cerr << "cannot open /proc/sys/vm/drop_caches\n";
        return false;
    }
    output << "3\n";
    output.close();
    std::cout << "Dropped Linux page cache before readrandom\n";
    return true;
}

RoundResult runRound(const Options &options, rocksdb::DB *db, std::uint32_t round,
                     const std::shared_ptr<rocksdb::Statistics> &statistics) {
    const auto cpus = parseCpus(options.workerCpus);
    const std::uint64_t perWorker = options.keys / options.threads;
    const auto beforeDisk = readDiskStats(options.diskName);
    const std::uint64_t beforeHit = statistics->getTickerCount(rocksdb::BLOCK_CACHE_HIT);
    const std::uint64_t beforeMiss = statistics->getTickerCount(rocksdb::BLOCK_CACHE_MISS);
    const std::uint64_t beforeDataHit = statistics->getTickerCount(rocksdb::BLOCK_CACHE_DATA_HIT);
    const std::uint64_t beforeDataMiss = statistics->getTickerCount(rocksdb::BLOCK_CACHE_DATA_MISS);
    uuse::diagnostics::reset();
    std::vector<std::vector<std::uint64_t>> latencies(options.threads);
    std::vector<std::uint64_t> hits(options.threads, 0);
    std::vector<WorkerStat> workerStats(options.threads);
    std::vector<std::thread> workers;
    const auto start = std::chrono::steady_clock::now();
    for (std::uint32_t worker = 0; worker < options.threads; ++worker) {
        workers.emplace_back([&, worker] {
            pinWorker(cpus[worker % cpus.size()]);
            WorkerStat &workerStat = workerStats[worker];
            workerStat.tid = static_cast<std::uint64_t>(::syscall(SYS_gettid));
            const auto switchesBefore = threadSwitches(workerStat.tid);
            const std::uint64_t cpuBefore = threadCpuNs();
            auto &samples = latencies[worker];
            samples.reserve(perWorker);
            std::mt19937_64 rng(0x9E3779B97F4A7C15ULL * (worker + 1));
            std::string value;
            for (std::uint64_t op = 0; op < perWorker; ++op) {
                const auto begin = std::chrono::steady_clock::now();
                const std::uint64_t index = rng() % options.keys;
                const rocksdb::Status status =
                    db->Get(rocksdb::ReadOptions(), makeKey(index, options.keySize), &value);
                const auto end = std::chrono::steady_clock::now();
                samples.push_back(static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count()));
                if (status.ok()) {
                    ++hits[worker];
                } else if (!status.IsNotFound()) {
                    std::cerr << "Get failed: " << status.ToString() << '\n';
                }
            }
            workerStat.cpuNs = threadCpuNs() - cpuBefore;
            const auto switchesAfter = threadSwitches(workerStat.tid);
            workerStat.voluntarySwitches = switchesAfter.first - switchesBefore.first;
            workerStat.involuntarySwitches = switchesAfter.second - switchesBefore.second;
        });
    }
    for (auto &worker : workers) worker.join();
    const double seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - start).count();
    std::vector<std::uint64_t> samples;
    for (auto &worker : latencies) {
        samples.insert(samples.end(), worker.begin(), worker.end());
    }
    RoundResult result;
    result.operations = samples.size();
    result.hits = std::accumulate(hits.begin(), hits.end(), std::uint64_t{0});
    result.misses = result.operations - result.hits;
    result.seconds = seconds;
    result.p50 = percentile(&samples, .50);
    result.p95 = percentile(&samples, .95);
    result.p99 = percentile(&samples, .99);
    result.p999 = percentile(&samples, .999);
    result.blockCacheHitCount = statistics->getTickerCount(rocksdb::BLOCK_CACHE_HIT) - beforeHit;
    result.blockCacheMissCount = statistics->getTickerCount(rocksdb::BLOCK_CACHE_MISS) - beforeMiss;
    result.blockDataHitCount = statistics->getTickerCount(rocksdb::BLOCK_CACHE_DATA_HIT) - beforeDataHit;
    result.blockDataMissCount = statistics->getTickerCount(rocksdb::BLOCK_CACHE_DATA_MISS) - beforeDataMiss;
    const auto afterDisk = readDiskStats(options.diskName);
    result.nvmeReadIos = afterDisk.readIos - beforeDisk.readIos;
    result.nvmeReadBytes = (afterDisk.readSectors - beforeDisk.readSectors) * 512;
    result.uuse = uuse::diagnostics::snapshot();
    std::cout << "=== readrandom round " << round << " ===\n"
              << "  operations=" << result.operations << " hits=" << result.hits
              << " misses=" << result.misses << " ops_per_sec="
              << std::fixed << std::setprecision(1)
              << static_cast<double>(result.operations) / result.seconds
              << " seconds=" << result.seconds << '\n'
              << "  latency_ns p50=" << result.p50 << " p95=" << result.p95
              << " p99=" << result.p99 << " p99.9=" << result.p999 << '\n'
              << "  RocksDB block_cache_hit=" << result.blockCacheHitCount
              << " block_cache_miss=" << result.blockCacheMissCount
              << " data_hit=" << result.blockDataHitCount
              << " data_miss=" << result.blockDataMissCount << '\n'
              << "  Linux " << options.diskName << " read_ios=" << result.nvmeReadIos
              << " read_bytes=" << result.nvmeReadBytes << '\n';
    if (options.mode == "uuse") printSnapshot(result.uuse);
    for (const auto &worker : workerStats) {
        std::cout << "  worker tid=" << worker.tid << " cpu_ms="
                  << std::setprecision(3) << (worker.cpuNs / 1000000.0)
                  << " voluntary_cs=" << worker.voluntarySwitches
                  << " involuntary_cs=" << worker.involuntarySwitches << '\n';
    }
    return result;
}

} // namespace

int main(int argc, char **argv) {
    Options options;
    if (!parseArguments(argc, argv, &options)) {
        std::cerr << "usage: --mode=uuse|ext4-buffered|ext4-direct --db=PATH "
                     "[--uuse-config=PATH] [--keys=160000] [--threads=8] "
                     "[--direct-reads=0|1] [--drop-os-cache-before-read=0|1] "
                     "[--skip-fill=0|1] [--skip-compaction=0|1] [--rounds=3]\n";
        return 1;
    }
    if (options.diskName.empty()) {
        options.diskName = options.mode == "uuse" ? "nvme0n1" : "sdb1";
    }
    rocksdb::Options dbOptions;
    std::unique_ptr<rocksdb::Env> ownedEnv;
    const rocksdb::Status configured = configureDatabase(options, &dbOptions, &ownedEnv);
    if (!configured.ok()) {
        std::cerr << configured.ToString() << '\n';
        return 2;
    }
    rocksdb::DB *rawDb = nullptr;
    const rocksdb::Status opened = rocksdb::DB::Open(dbOptions, options.dbPath, &rawDb);
    if (!opened.ok()) {
        std::cerr << "DB::Open failed: " << opened.ToString() << '\n';
        return 3;
    }
    std::unique_ptr<rocksdb::DB> db(rawDb);
    std::cout << "RocksDB version: " << rocksdb::GetRocksVersionAsString(true)
              << " mode=" << options.mode << " direct_reads=" << options.directReads
              << " block_size=" << options.blockSize << " cache_size=" << options.cacheSize
              << " compression=" << (options.compression ? "snappy" : "none")
              << " write_buffer_size=" << dbOptions.write_buffer_size << '\n';
    if (!options.skipFill) {
        if (!fillAndCompact(options, db.get())) {
            std::cerr << "fillseq/flush/compaction failed\n";
            return 4;
        }
    } else {
        std::cout << "Skipped fillseq/flush/compaction; using existing DB\n";
    }
    printDbShape(options, db.get());
    if (!dropOsCacheBeforeRead(options)) return 5;
    for (std::uint32_t round = 1; round <= options.rounds; ++round) {
        runRound(options, db.get(), round,
                 std::static_pointer_cast<rocksdb::Statistics>(dbOptions.statistics));
    }
    return 0;
}
