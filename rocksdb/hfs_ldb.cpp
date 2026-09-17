/* Minimal HFS-aware ldb consistency checker for the RocksDB acceptance path. */

#include <cstdint>
#include <iostream>
#include <memory>
#include <string>

#include "rocksdb/db.h"
#include "rocksdb/hfs_filesystem.hpp"
#include "rocksdb/options.h"

namespace {

/* Parses the stable checkconsistency command surface. */
bool parseArguments(int argc, char **argv, std::string *config, std::string *dbPath) {
    if (argc != 4 || config == nullptr || dbPath == nullptr ||
        std::string(argv[2]) != "checkconsistency") return false;
    const std::string dbArgument = argv[3];
    if (dbArgument.rfind("--db=", 0) != 0 || dbArgument.size() == 5) return false;
    *config = argv[1];
    *dbPath = dbArgument.substr(5);
    return true;
}

/* Opens the HFS database without writes and scans every visible key/value entry. */
int checkConsistency(const std::string &config, const std::string &dbPath) {
    auto env = hfs::rocksdb_adapter::createCompositeEnv(config);
    if (env == nullptr) {
        std::cerr << "cannot create HFS RocksDB Env\n";
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
        std::cerr << "usage: hfs_ldb CONFIG checkconsistency --db=PATH\n";
        return 1;
    }
    return checkConsistency(config, dbPath);
}
