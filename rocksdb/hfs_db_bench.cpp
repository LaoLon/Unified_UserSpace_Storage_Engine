/* Minimal RocksDB workload driver for the HFS FileSystem contract. */

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <random>
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
    std::uint64_t cacheSize = 0; /* 0 = RocksDB default */
};

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
        } else if (argument.rfind("--cache_size=", 0) == 0) {
            options->cacheSize = std::stoull(argument.substr(13));
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
    if (options->configPath.empty() || options->dbPath.empty() || options->threads == 0 ||
        options->num == 0 || options->keySize < 8 || options->valueSize == 0) {
        return false;
    }
    for (const auto &benchmark : options->benchmarks) {
        if (benchmark != "fillseq" && benchmark != "overwrite" &&
            benchmark != "readrandom" && benchmark != "readwhilewriting") return false;
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

/* Runs one closure per worker thread and joins before returning. */
template <typename Fn> void runParallel(std::uint32_t threads, Fn &&fn) {
    std::vector<std::thread> workers;
    workers.reserve(threads);
    for (std::uint32_t worker = 0; worker < threads; ++worker) {
        workers.emplace_back(fn, worker);
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

void report(const char *name, std::uint64_t ops, const BenchOptions &options, double seconds,
            const char *extra = "") {
    const double rate = seconds > 0.0 ? static_cast<double>(ops) / seconds : 0.0;
    std::cout << "BENCH " << name << " ops=" << ops << " threads=" << options.threads
              << " seconds=" << seconds << " ops_per_sec=" << rate;
    if (extra[0] != '\0') std::cout << ' ' << extra;
    std::cout << "\n";
}

/* Executes one deterministic key/value operation against the composite Env. */
int runBenchmark(const BenchOptions &options, rocksdb::Env *sharedEnv = nullptr) {
    std::unique_ptr<rocksdb::Env> ownedEnv;
    if (sharedEnv == nullptr) ownedEnv = hfs::rocksdb_adapter::createCompositeEnv(options.configPath);
    rocksdb::Env *env = sharedEnv != nullptr ? sharedEnv : ownedEnv.get();
    if (env == nullptr) {
        std::cerr << "cannot create HFS RocksDB Env\n";
        return 2;
    }
    rocksdb::Options dbOptions;
    dbOptions.create_if_missing = true;
    dbOptions.env = env;
    dbOptions.max_background_jobs = static_cast<int>(options.threads);
    dbOptions.use_direct_io_for_flush_and_compaction = options.directIoForFlush;
    if (options.cacheSize != 0) {
        rocksdb::BlockBasedTableOptions tableOptions;
        tableOptions.block_cache = rocksdb::NewLRUCache(options.cacheSize);
        dbOptions.table_factory =
            std::shared_ptr<rocksdb::TableFactory>(rocksdb::NewBlockBasedTableFactory(tableOptions));
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
    std::cout << "BENCH start " << options.benchmark << " ops=" << ops
              << " threads=" << options.threads << " sync=" << (options.sync ? 1 : 0) << "\n";
    std::cout.flush();
    /* Crash-injection arming: the database is open, so tests may now crash. */
    if (const char *armFile = std::getenv("HFS_TEST_CRASH_ARM_FILE");
        armFile != nullptr && *armFile != '\0') {
        if (std::FILE *arm = std::fopen(armFile, "w")) {
            std::fclose(arm);
        }
    }

    if (options.benchmark == "readrandom") {
        auto start = std::chrono::steady_clock::now();
        runParallel(options.threads, [&](std::uint32_t worker) {
            std::mt19937_64 rng(0x9E3779B97F4A7C15ULL * (worker + 1));
            std::string value;
            for (std::uint64_t i = 0; i < options.num; ++i) {
                const std::uint64_t index = rng() % ops;
                rocksdb::Status status =
                    db->Get(rocksdb::ReadOptions(), makeKey(index, options.keySize), &value);
                if (!status.ok() && !status.IsNotFound()) {
                    std::cerr << "Get failed: " << status.ToString() << "\n";
                    std::exit(4);
                }
            }
        });
        const double seconds = secondsSince(start);
        report("readrandom", ops, options, seconds);
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
        report("overwrite", ops, options, seconds);
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
        report("readwhilewriting", options.num + reads, options, seconds, extra.str().c_str());
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
        report("fillseq", ops, options, seconds);
    }

    if (options.benchmark != "readrandom") {
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
    std::cout << options.benchmark << " completed " << ops << " operations\n";
    return 0;
}

} // namespace

int main(int argc, char **argv) {
    BenchOptions options;
    if (!parseArguments(argc, argv, &options)) {
        std::cerr << "usage: hfs_db_bench --hfs-config=CONFIG --db=PATH "
                     "--benchmarks=fillseq,readrandom,overwrite,readwhilewriting "
                     "[--num=N] [--threads=N] [--key_size=N] [--value_size=N] "
                     "[--use_direct_io_for_flush_and_compaction=1] [--disable_wal=1] [--sync=1]\n";
        return 1;
    }
    auto sharedEnv = hfs::rocksdb_adapter::createCompositeEnv(options.configPath);
    if (sharedEnv == nullptr) {
        std::cerr << "cannot create HFS RocksDB Env\n";
        return 2;
    }
    for (const auto &benchmark : options.benchmarks) {
        BenchOptions run = options;
        run.benchmark = benchmark;
        const int result = runBenchmark(run, sharedEnv.get());
        if (result != 0) return result;
    }
    return 0;
}
