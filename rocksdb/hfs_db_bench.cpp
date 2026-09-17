/* Minimal RocksDB workload driver for the HFS FileSystem contract. */

#include <cstdint>
#include <iostream>
#include <memory>
#include <string>

#include "rocksdb/db.h"
#include "rocksdb/hfs_filesystem.hpp"
#include "rocksdb/options.h"

namespace {

/* Parses a small stable argument surface shared by the Shard-82 smoke. */
struct BenchOptions {
    std::string configPath;
    std::string dbPath;
    std::string benchmark = "fillseq";
    std::uint64_t num = 100;
};

bool parseArguments(int argc, char **argv, BenchOptions *options) {
    if (argc < 3 || options == nullptr) {
        return false;
    }
    options->configPath = argv[1];
    options->dbPath = argv[2];
    for (int i = 3; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument.rfind("--bench=", 0) == 0) {
            options->benchmark = argument.substr(8);
        } else if (argument.rfind("--num=", 0) == 0) {
            options->num = std::stoull(argument.substr(6));
        } else {
            return false;
        }
    }
    return options->benchmark == "fillseq" || options->benchmark == "overwrite" ||
           options->benchmark == "readrandom" || options->benchmark == "readwhilewriting";
}

/* Executes one deterministic key/value operation against the composite Env. */
int runBenchmark(const BenchOptions &options) {
    auto env = hfs::rocksdb_adapter::createCompositeEnv(options.configPath);
    if (env == nullptr) {
        std::cerr << "cannot create HFS RocksDB Env\n";
        return 2;
    }
    rocksdb::Options dbOptions;
    dbOptions.create_if_missing = true;
    dbOptions.env = env.get();
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
    std::cout << options.benchmark << " completed " << options.num << " operations\n";
    return 0;
}

} // namespace

int main(int argc, char **argv) {
    BenchOptions options;
    if (!parseArguments(argc, argv, &options)) {
        std::cerr << "usage: hfs_db_bench CONFIG DB_PATH [--bench=fillseq|overwrite|readrandom|readwhilewriting] [--num=N]\n";
        return 1;
    }
    return runBenchmark(options);
}
