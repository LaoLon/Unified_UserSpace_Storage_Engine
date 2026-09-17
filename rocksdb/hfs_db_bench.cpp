/* Minimal RocksDB workload driver for the HFS FileSystem contract. */

#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <sstream>
#include <vector>

#include "rocksdb/db.h"
#include "rocksdb/hfs_filesystem.hpp"
#include "rocksdb/options.h"

namespace {

/* Parses both the canonical Shard-84 flags and the legacy smoke form. */
struct BenchOptions {
    std::string configPath;
    std::string dbPath;
    std::string benchmark = "fillseq";
    std::vector<std::string> benchmarks;
    std::uint64_t num = 100;
    std::uint32_t threads = 1;
    bool directIoForFlush = false;
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
        } else if (argument.rfind("--use_direct_io_for_flush_and_compaction=", 0) == 0) {
            options->directIoForFlush = argument.substr(argument.find('=') + 1) == "1";
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
    if (options->configPath.empty() || options->dbPath.empty() || options->threads == 0) {
        return false;
    }
    for (const auto &benchmark : options->benchmarks) {
        if (benchmark != "fillseq" && benchmark != "overwrite" &&
            benchmark != "readrandom" && benchmark != "readwhilewriting") return false;
    }
    return true;
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
    rocksdb::DB *database = nullptr;
    rocksdb::Status opened = rocksdb::DB::Open(dbOptions, options.dbPath, &database);
    if (!opened.ok()) {
        std::cerr << "DB::Open failed: " << opened.ToString() << "\n";
        return 3;
    }
    std::unique_ptr<rocksdb::DB> db(database);
    const bool readOnly = options.benchmark == "readrandom";
    const bool overwrite = options.benchmark == "overwrite";
    const bool readWhileWriting = options.benchmark == "readwhilewriting";

    /* Seed stable keys before overwrite so the mode exercises replacement of
     * existing values rather than another append-only workload. */
    if (overwrite) {
        for (std::uint64_t i = 0; i < options.num; ++i) {
            const std::string key = "key-" + std::to_string(i);
            const rocksdb::Status status =
                db->Put(rocksdb::WriteOptions(), key, "initial-" + std::to_string(i));
            if (!status.ok()) {
                std::cerr << "seed Put failed: " << status.ToString() << "\n";
                return 5;
            }
        }
    }

    for (std::uint64_t i = 0; i < options.num; ++i) {
        const std::string key = "key-" + std::to_string(i);
        if (readOnly) {
            std::string value;
            rocksdb::Status status = db->Get(rocksdb::ReadOptions(), key, &value);
            if (!status.ok() && !status.IsNotFound()) {
                std::cerr << "Get failed: " << status.ToString() << "\n";
                return 4;
            }
        } else {
            const std::string value = (overwrite ? "updated-" : "value-") +
                                      std::to_string(i);
            rocksdb::Status status = db->Put(rocksdb::WriteOptions(), key, value);
            if (!status.ok()) {
                std::cerr << "Put failed: " << status.ToString() << "\n";
                return 5;
            }
            if (readWhileWriting) {
                std::string readBack;
                status = db->Get(rocksdb::ReadOptions(), key, &readBack);
                if (!status.ok() || readBack != value) {
                    std::cerr << "readwhilewriting verification failed for " << key << "\n";
                    return 4;
                }
            }
        }
    }
    if (overwrite) {
        for (std::uint64_t i = 0; i < options.num; ++i) {
            std::string value;
            const std::string key = "key-" + std::to_string(i);
            const rocksdb::Status status = db->Get(rocksdb::ReadOptions(), key, &value);
            if (!status.ok() || value != "updated-" + std::to_string(i)) {
                std::cerr << "overwrite verification failed for " << key << "\n";
                return 4;
            }
        }
    }
    if (!readOnly) {
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
    std::cout << options.benchmark << " completed " << options.num << " operations\n";
    return 0;
}

} // namespace

int main(int argc, char **argv) {
    BenchOptions options;
    if (!parseArguments(argc, argv, &options)) {
        std::cerr << "usage: hfs_db_bench --hfs-config=CONFIG --db=PATH "
                     "--benchmarks=fillseq,readrandom,overwrite,readwhilewriting "
                     "[--num=N] [--threads=N] "
                     "[--use_direct_io_for_flush_and_compaction=1]\n";
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
