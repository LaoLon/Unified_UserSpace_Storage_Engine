/* Minimal UUSE-aware ldb consistency checker for the RocksDB acceptance path. */

#include <cstdint>
#include <iostream>
#include <memory>
#include <string>

#include "rocksdb/db.h"
#include "rocksdb/uuse_filesystem.hpp"
#include "rocksdb/options.h"

namespace {

/* Parses the stable checkconsistency command surface. */
bool parseArguments(int argc, char **argv, std::string *config, std::string *dbPath) {
    if (argc < 2 || config == nullptr || dbPath == nullptr) return false;
    bool commandSeen = false;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "checkconsistency") {
            commandSeen = true;
        } else if (argument.rfind("--uuse-config=", 0) == 0) {
            *config = argument.substr(13);
        } else if (argument.rfind("--db=", 0) == 0) {
            *dbPath = argument.substr(5);
        } else if (index == 1 && config->empty()) {
            *config = argument;
        } else {
            return false;
        }
    }
    return commandSeen && !config->empty() && !dbPath->empty();
}

/* Opens the UUSE database without writes and scans every visible key/value entry. */
int checkConsistency(const std::string &config, const std::string &dbPath) {
    auto env = uuse::rocksdb_adapter::createCompositeEnv(config);
    if (env == nullptr) {
        std::cerr << "cannot create UUSE RocksDB Env\n";
        return 2;
    }
    rocksdb::Options options;
    options.env = env.get();
    options.create_if_missing = false;
    options.use_direct_reads = true;
    rocksdb::DB *rawDatabase = nullptr;
    rocksdb::Status opened = rocksdb::DB::Open(options, dbPath, &rawDatabase);
    if (!opened.ok()) {
        std::cerr << "DB::Open failed: " << opened.ToString() << "\n";
        return 3;
    }
    std::unique_ptr<rocksdb::DB> database(rawDatabase);
    std::unique_ptr<rocksdb::Iterator> iterator(database->NewIterator(rocksdb::ReadOptions()));
    std::uint64_t entries = 0;
    for (iterator->SeekToFirst(); iterator->Valid(); iterator->Next()) ++entries;
    if (!iterator->status().ok()) {
        std::cerr << "iterator consistency check failed: " << iterator->status().ToString()
                  << "\n";
        return 4;
    }
    std::cout << "checkconsistency ok entries=" << entries << "\n";
    return 0;
}

} // namespace

int main(int argc, char **argv) {
    std::string config;
    std::string dbPath;
    if (!parseArguments(argc, argv, &config, &dbPath)) {
        std::cerr << "usage: uuse_ldb --uuse-config=CONFIG checkconsistency --db=PATH\n";
        return 1;
    }
    return checkConsistency(config, dbPath);
}
