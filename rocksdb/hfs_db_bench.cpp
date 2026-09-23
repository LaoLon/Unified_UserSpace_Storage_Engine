/* Minimal RocksDB workload driver for the HFS FileSystem contract. */

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <pthread.h>
#include <random>
#include <sched.h>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "rocksdb/cache.h"
#include "rocksdb/db.h"
#include "rocksdb/hfs_filesystem.hpp"
#include "rocksdb/options.h"
#include "rocksdb/table.h"

namespace {

/* Parses both the canonical Shard-84 flags and the legacy smoke form. */
struct BenchOptions {
    std::string configPath;
    std::string dbPath;
    std::string benchmark = "fillseq";
    std::vector<std::string> benchmarks;
    std::uint64_t num = 100;
    std::uint32_t threads = 1;
    std::uint32_t keySize = 16;
    std::uint32_t valueSize = 100;
    bool directIoForFlush = false;
    bool disableWal = false;
    bool sync = false;
    bool verbose = false;
    bool nativeEnv = false;
    bool compression = true; /* RocksDB default is Snappy */
    bool directReads = false;
    bool fresh = false; /* DestroyDB before open to force a clean fill */
    std::uint32_t blockSize = 4096;
    std::uint32_t rounds = 1; /* read benchmarks repeat within one open */
    std::uint64_t cacheSize = 0; /* 0 = RocksDB default */
    std::vector<unsigned int> workerCpus;
};

/* Parses a comma-separated CPU list for worker pinning. */
std::vector<unsigned int> parseCpus(const std::string &text) {
    std::vector<unsigned int> result;
    std::stringstream stream(text);
    std::string item;
    while (std::getline(stream, item, ',')) {
        if (!item.empty()) result.push_back(static_cast<unsigned int>(std::stoul(item)));
    }
    return result;
}

/* Pins the calling worker thread to one CPU within its inherited cpuset. */
void pinWorker(unsigned int cpu) {
    cpu_set_t allowed;
    CPU_ZERO(&allowed);
    if (pthread_getaffinity_np(pthread_self(), sizeof(allowed), &allowed) != 0 ||
        !CPU_ISSET(cpu, &allowed)) {
        return;
    }
    cpu_set_t selected;
    CPU_ZERO(&selected);
    CPU_SET(cpu, &selected);
    (void)pthread_setaffinity_np(pthread_self(), sizeof(selected), &selected);
}

/* Splits the comma-separated benchmark list while rejecting empty entries. */
std::vector<std::string> splitBenchmarks(const std::string &value) {
    std::vector<std::string> result;
    std::stringstream stream(value);
    std::string benchmark;
    while (std::getline(stream, benchmark, ',')) {
        if (benchmark.empty()) return {};
        result.push_back(benchmark);
    }
    return result;
}

bool parseArguments(int argc, char **argv, BenchOptions *options) {
    if (argc < 2 || options == nullptr) {
        return false;
    }
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument.rfind("--hfs-config=", 0) == 0) {
            options->configPath = argument.substr(13);
        } else if (argument.rfind("--db=", 0) == 0) {
            options->dbPath = argument.substr(5);
        } else if (argument.rfind("--benchmarks=", 0) == 0) {
            options->benchmarks = splitBenchmarks(argument.substr(13));
        } else if (argument.rfind("--threads=", 0) == 0) {
            options->threads = static_cast<std::uint32_t>(std::stoul(argument.substr(10)));
        } else if (argument.rfind("--key_size=", 0) == 0) {
            options->keySize = static_cast<std::uint32_t>(std::stoul(argument.substr(11)));
        } else if (argument.rfind("--value_size=", 0) == 0) {
            options->valueSize = static_cast<std::uint32_t>(std::stoul(argument.substr(13)));
        } else if (argument.rfind("--use_direct_io_for_flush_and_compaction=", 0) == 0) {
            options->directIoForFlush = argument.substr(argument.find('=') + 1) == "1";
        } else if (argument.rfind("--disable_wal=", 0) == 0) {
            options->disableWal = argument.substr(argument.find('=') + 1) == "1";
        } else if (argument.rfind("--sync=", 0) == 0) {
            options->sync = argument.substr(argument.find('=') + 1) == "1";
        } else if (argument == "--verbose" || argument == "--verbose=1") {
            options->verbose = true;
        } else if (argument.rfind("--cache_size=", 0) == 0) {
            options->cacheSize = std::stoull(argument.substr(13));
        } else if (argument.rfind("--compression=", 0) == 0) {
            options->compression = argument.substr(14) != "none";
        } else if (argument.rfind("--use_direct_reads=", 0) == 0) {
            options->directReads = argument.substr(argument.find('=') + 1) == "1";
        } else if (argument.rfind("--fresh=", 0) == 0) {
            options->fresh = argument.substr(argument.find('=') + 1) == "1";
        } else if (argument.rfind("--block_size=", 0) == 0) {
            options->blockSize = static_cast<std::uint32_t>(std::stoul(argument.substr(13)));
        } else if (argument.rfind("--rounds=", 0) == 0) {
            options->rounds = static_cast<std::uint32_t>(std::stoul(argument.substr(9)));
        } else if (argument.rfind("--env=", 0) == 0) {
            const std::string mode = argument.substr(6);
            if (mode != "native" && mode != "hfs") return false;
            options->nativeEnv = mode == "native";
        } else if (argument.rfind("--worker_cpus=", 0) == 0) {
            options->workerCpus = parseCpus(argument.substr(14));
        } else if (argument.rfind("--bench=", 0) == 0) {
            options->benchmark = argument.substr(8);
        } else if (argument.rfind("--num=", 0) == 0) {
            options->num = std::stoull(argument.substr(6));
        } else if (argument.front() != '-' && options->configPath.empty()) {
            options->configPath = argument;
        } else if (argument.front() != '-' && options->dbPath.empty()) {
            options->dbPath = argument;
        } else {
            return false;
        }
    }
    if (options->benchmarks.empty()) options->benchmarks.push_back(options->benchmark);
    if ((!options->nativeEnv && options->configPath.empty()) || options->dbPath.empty() ||
        options->threads == 0 || options->num == 0 || options->keySize < 8 ||
        options->valueSize == 0) {
        return false;
    }
    for (const auto &benchmark : options->benchmarks) {
        if (benchmark != "fillseq" && benchmark != "fillrandom" &&
            benchmark != "overwrite" && benchmark != "readseq" &&
            benchmark != "readrandom" && benchmark != "readwhilewriting") {
            return false;
        }
    }
    return true;
}

/* Builds a fixed-width binary key: big-endian index plus zero padding. */
std::string makeKey(std::uint64_t index, std::uint32_t keySize) {
    std::string key(keySize, '\0');
    const std::uint32_t bytes = keySize < 8 ? keySize : 8;
    for (std::uint32_t byte = 0; byte < bytes; ++byte) {
        key[byte] = static_cast<char>((index >> ((bytes - 1 - byte) * 8)) & 0xFF);
    }
    return key;
}

/* Builds a deterministic, compressible value of the requested size. */
std::string makeValue(std::uint64_t index, std::uint32_t valueSize) {
    std::string value(valueSize, 'v');
    const std::uint32_t bytes = valueSize < 8 ? valueSize : 8;
    for (std::uint32_t byte = 0; byte < bytes; ++byte) {
        value[byte] = static_cast<char>((index >> (byte * 8)) & 0xFF);
    }
    return value;
}

/* Worker affinity from --worker_cpus; applied by runParallel. */
std::vector<unsigned int> gWorkerCpus;

/* Runs one closure per worker thread, optionally pinned, and joins. */
template <typename Fn> void runParallel(std::uint32_t threads, Fn &&fn) {
    std::vector<std::thread> workers;
    workers.reserve(threads);
    for (std::uint32_t worker = 0; worker < threads; ++worker) {
        workers.emplace_back([&fn, worker]() {
            if (!gWorkerCpus.empty()) {
                pinWorker(gWorkerCpus[worker % gWorkerCpus.size()]);
            }
            fn(worker);
        });
    }
    for (auto &worker : workers) {
        worker.join();
    }
}

/* Per-thread op counts follow db_bench: num ops per worker. */
std::uint64_t totalOps(const BenchOptions &options) {
    return options.num * options.threads;
}

double secondsSince(const std::chrono::steady_clock::time_point &start) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

/* Computes the logical key/value bytes represented by a benchmark operation. */
std::uint64_t logicalBytes(const BenchOptions &options, std::uint64_t ops) {
    const std::uint64_t bytesPerOperation =
        static_cast<std::uint64_t>(options.keySize) + options.valueSize;
    if (bytesPerOperation != 0 &&
        ops > std::numeric_limits<std::uint64_t>::max() / bytesPerOperation) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return ops * bytesPerOperation;
}

void report(const char *name, std::uint64_t ops, std::uint64_t bytes, double seconds,
            const char *extra = "") {
    const double rate = seconds > 0.0 ? static_cast<double>(ops) / seconds : 0.0;
    const double microsPerOp = ops > 0 ? seconds * 1000000.0 / static_cast<double>(ops) : 0.0;
    const double megabytesPerSecond =
        seconds > 0.0 ? static_cast<double>(bytes) / (1024.0 * 1024.0) / seconds : 0.0;
    std::cout << std::left << std::setw(16) << name << ": " << std::right << std::fixed
              << std::setprecision(3) << std::setw(12) << microsPerOp << " micros/op "
              << std::setprecision(0) << rate << " ops/sec " << std::setprecision(3)
              << seconds << " seconds " << ops << " operations; " << std::setprecision(1)
              << megabytesPerSecond << " MB/s";
    if (extra[0] != '\0') std::cout << ' ' << extra;
    std::cout << "\n";
}

/* Executes one deterministic key/value operation against the composite Env. */
int runBenchmark(const BenchOptions &options, rocksdb::Env *sharedEnv = nullptr) {
    std::unique_ptr<rocksdb::Env> ownedEnv;
    rocksdb::Env *env = sharedEnv;
    if (env == nullptr && options.nativeEnv) {
        env = rocksdb::Env::Default();
    } else if (env == nullptr) {
        ownedEnv = hfs::rocksdb_adapter::createCompositeEnv(options.configPath);
        env = ownedEnv.get();
    }
    if (env == nullptr) {
        std::cerr << "cannot create RocksDB Env\n";
        return 2;
    }
    gWorkerCpus = options.workerCpus;
    rocksdb::Options dbOptions;
    dbOptions.create_if_missing = true;
    dbOptions.env = env;
    dbOptions.max_background_jobs = static_cast<int>(options.threads);
    dbOptions.compression = options.compression ? rocksdb::kSnappyCompression
                                                : rocksdb::kNoCompression;
    dbOptions.use_direct_reads = options.directReads;
    dbOptions.use_direct_io_for_flush_and_compaction = options.directIoForFlush;
    {
        rocksdb::BlockBasedTableOptions tableOptions;
        tableOptions.block_size = options.blockSize;
        if (options.cacheSize != 0) {
            tableOptions.block_cache = rocksdb::NewLRUCache(options.cacheSize);
        }
        dbOptions.table_factory = std::shared_ptr<rocksdb::TableFactory>(
            rocksdb::NewBlockBasedTableFactory(tableOptions));
    }
    if (options.fresh) {
        /* A clean fill needs a fresh DB; NotFound for a new path is fine. */
        (void)rocksdb::DestroyDB(options.dbPath, dbOptions);
    }
    rocksdb::DB *database = nullptr;
    rocksdb::Status opened = rocksdb::DB::Open(dbOptions, options.dbPath, &database);
    if (!opened.ok()) {
        std::cerr << "DB::Open failed: " << opened.ToString() << "\n";
        return 3;
    }
    std::unique_ptr<rocksdb::DB> db(database);
    const std::uint64_t ops = totalOps(options);
    rocksdb::WriteOptions writeOptions;
    writeOptions.disableWAL = options.disableWal;
    writeOptions.sync = options.sync;
    /* Crash-injection arming: the database is open, so tests may now crash. */
    if (const char *armFile = std::getenv("HFS_TEST_CRASH_ARM_FILE");
        armFile != nullptr && *armFile != '\0') {
        if (std::FILE *arm = std::fopen(armFile, "w")) {
            std::fclose(arm);
        }
    }

    const bool readBenchmark =
        options.benchmark == "readseq" || options.benchmark == "readrandom";
    if (readBenchmark) {
        for (std::uint32_t round = 1; round <= options.rounds; ++round) {
            auto start = std::chrono::steady_clock::now();
            runParallel(options.threads, [&](std::uint32_t worker) {
                std::mt19937_64 rng(0x9E3779B97F4A7C15ULL * (worker + 1));
                std::string value;
                for (std::uint64_t i = 0; i < options.num; ++i) {
                    const std::uint64_t index = options.benchmark == "readseq"
                                                    ? worker * options.num + i
                                                    : rng() % ops;
                    rocksdb::Status status =
                        db->Get(rocksdb::ReadOptions(), makeKey(index, options.keySize), &value);
                    if (!status.ok() && !status.IsNotFound()) {
                        std::cerr << "Get failed: " << status.ToString() << "\n";
                        std::exit(4);
                    }
                }
            });
            const double seconds = secondsSince(start);
            const std::string name =
                options.benchmark + " r" + std::to_string(round);
            report(name.c_str(), ops, logicalBytes(options, ops), seconds);
        }
    } else if (options.benchmark == "overwrite") {
        /* Seed stable keys before overwrite so the mode exercises replacement of
         * existing values rather than another append-only workload. */
        runParallel(options.threads, [&](std::uint32_t worker) {
            for (std::uint64_t i = 0; i < options.num; ++i) {
                const std::uint64_t index = worker * options.num + i;
                rocksdb::Status status = db->Put(writeOptions,
                                                 makeKey(index, options.keySize),
                                                 makeValue(index, options.valueSize));
                if (!status.ok()) {
                    std::cerr << "seed Put failed: " << status.ToString() << "\n";
                    std::exit(5);
                }
            }
        });
        auto start = std::chrono::steady_clock::now();
        runParallel(options.threads, [&](std::uint32_t worker) {
            for (std::uint64_t i = 0; i < options.num; ++i) {
                const std::uint64_t index = worker * options.num + i;
                rocksdb::Status status = db->Put(writeOptions,
                                                 makeKey(index, options.keySize),
                                                 makeValue(index + ops, options.valueSize));
                if (!status.ok()) {
                    std::cerr << "Put failed: " << status.ToString() << "\n";
                    std::exit(5);
                }
            }
        });
        const double seconds = secondsSince(start);
        report("overwrite", ops, logicalBytes(options, ops), seconds);
        /* Verification is untimed: it guards content, not throughput. */
        runParallel(options.threads, [&](std::uint32_t worker) {
            std::string value;
            for (std::uint64_t i = 0; i < options.num; ++i) {
                const std::uint64_t index = worker * options.num + i;
                const rocksdb::Status status =
                    db->Get(rocksdb::ReadOptions(), makeKey(index, options.keySize), &value);
                if (!status.ok() || value != makeValue(index + ops, options.valueSize)) {
                    std::cerr << "overwrite verification failed for " << index << "\n";
                    std::exit(4);
                }
            }
        });
    } else if (options.benchmark == "readwhilewriting") {
        /* One writer and N-1 readers, mirroring db_bench's shape. */
        std::atomic<bool> writerDone{false};
        std::atomic<std::uint64_t> readOps{0};
        std::vector<std::thread> readers;
        const std::uint32_t readerCount = options.threads > 1 ? options.threads - 1 : 1;
        auto start = std::chrono::steady_clock::now();
        for (std::uint32_t reader = 0; reader < readerCount; ++reader) {
            readers.emplace_back([&, reader]() {
                if (!gWorkerCpus.empty()) {
                    pinWorker(gWorkerCpus[reader % gWorkerCpus.size()]);
                }
                std::mt19937_64 rng(0xD1B54A32D192ED03ULL * (reader + 1));
                std::string value;
                while (!writerDone.load(std::memory_order_relaxed)) {
                    const std::uint64_t index = rng() % options.num;
                    rocksdb::Status status =
                        db->Get(rocksdb::ReadOptions(), makeKey(index, options.keySize), &value);
                    if (!status.ok() && !status.IsNotFound()) {
                        std::cerr << "Get failed: " << status.ToString() << "\n";
                        std::exit(4);
                    }
                    readOps.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }
        for (std::uint64_t i = 0; i < options.num; ++i) {
            rocksdb::Status status = db->Put(writeOptions,
                                             makeKey(i, options.keySize),
                                             makeValue(i, options.valueSize));
            if (!status.ok()) {
                std::cerr << "Put failed: " << status.ToString() << "\n";
                std::exit(5);
            }
        }
        writerDone.store(true, std::memory_order_relaxed);
        for (auto &reader : readers) reader.join();
        const double seconds = secondsSince(start);
        const std::uint64_t reads = readOps.load(std::memory_order_relaxed);
        std::ostringstream extra;
        extra << "writer_ops=" << options.num << " reader_ops=" << reads;
        report("readwhilewriting", options.num + reads,
               logicalBytes(options, options.num + reads), seconds, extra.str().c_str());
    } else if (options.benchmark == "fillrandom") {
        /* Each worker fills its own key partition in random order so the run
         * stays insert-only (no cross-thread duplicate overwrites). */
        auto start = std::chrono::steady_clock::now();
        runParallel(options.threads, [&](std::uint32_t worker) {
            std::mt19937_64 rng(0x2545F4914F6CDD1DULL * (worker + 1));
            for (std::uint64_t i = 0; i < options.num; ++i) {
                const std::uint64_t index =
                    worker * options.num + (rng() % options.num);
                rocksdb::Status status = db->Put(writeOptions,
                                                 makeKey(index, options.keySize),
                                                 makeValue(index, options.valueSize));
                if (!status.ok()) {
                    std::cerr << "Put failed: " << status.ToString() << "\n";
                    std::exit(5);
                }
            }
        });
        const double seconds = secondsSince(start);
        report("fillrandom", ops, logicalBytes(options, ops), seconds);
    } else {
        auto start = std::chrono::steady_clock::now();
        runParallel(options.threads, [&](std::uint32_t worker) {
            for (std::uint64_t i = 0; i < options.num; ++i) {
                const std::uint64_t index = worker * options.num + i;
                rocksdb::Status status = db->Put(writeOptions,
                                                 makeKey(index, options.keySize),
                                                 makeValue(index, options.valueSize));
                if (!status.ok()) {
                    std::cerr << "Put failed: " << status.ToString() << "\n";
                    std::exit(5);
                }
            }
        });
        const double seconds = secondsSince(start);
        report("fillseq", ops, logicalBytes(options, ops), seconds);
    }

    if (!readBenchmark) {
        /* Force the direct-I/O flush and compaction contract before close. */
        rocksdb::FlushOptions flushOptions;
        rocksdb::Status status = db->Flush(flushOptions);
        if (!status.ok()) {
            std::cerr << "Flush failed: " << status.ToString() << "\n";
            return 7;
        }
        rocksdb::CompactRangeOptions compactOptions;
        status = db->CompactRange(compactOptions, nullptr, nullptr);
        if (!status.ok()) {
            std::cerr << "CompactRange failed: " << status.ToString() << "\n";
            return 7;
        }
    }
    /* Close RocksDB before checking publication of CURRENT and MANIFEST. */
    db.reset();
    /* Verify the RocksDB CURRENT publication before this process exits. */
    rocksdb::FileOptions fileOptions;
    rocksdb::IODebugContext debug;
    if (!env->GetFileSystem()->FileExists(options.dbPath + "/CURRENT",
                                          fileOptions.io_options, &debug).ok()) {
        std::cerr << "CURRENT was not published in HFS namespace\n";
        return 6;
    }
    return 0;
}

} // namespace

int main(int argc, char **argv) {
    BenchOptions options;
    if (!parseArguments(argc, argv, &options)) {
        std::cerr << "usage: hfs_db_bench [--env=hfs|native] [--hfs-config=CONFIG] --db=PATH "
                     "--benchmarks=fillseq,fillrandom,overwrite,readseq,readrandom,"
                     "readwhilewriting "
                     "[--num=N] [--threads=N] [--key_size=N] [--value_size=N] "
                     "[--worker_cpus=1,2] [--cache_size=N] [--block_size=N] "
                     "[--rounds=N] [--fresh=1] [--use_direct_reads=1] "
                     "[--use_direct_io_for_flush_and_compaction=1] [--disable_wal=1] "
                     "[--sync=1] [--verbose]\n";
        return 1;
    }
    /* Match stock db_bench output by hiding routine HFS lifecycle messages. */
    if (options.verbose) {
        ::setenv("HFS_LOG_LEVEL", "info", 1);
    } else if (std::getenv("HFS_LOG_LEVEL") == nullptr) {
        ::setenv("HFS_LOG_LEVEL", "error", 1);
    }
    std::cout << "DB path: [" << options.dbPath << "]\n";
    std::unique_ptr<rocksdb::Env> sharedEnv;
    if (!options.nativeEnv) {
        sharedEnv = hfs::rocksdb_adapter::createCompositeEnv(options.configPath);
        if (sharedEnv == nullptr) {
            std::cerr << "cannot create HFS RocksDB Env\n";
            return 2;
        }
    }
    for (const auto &benchmark : options.benchmarks) {
        BenchOptions run = options;
        run.benchmark = benchmark;
        const int result = runBenchmark(run, sharedEnv.get());
        if (result != 0) return result;
    }
    return 0;
}
