#pragma once

#include <memory>
#include <string>

#include "hfs/export.hpp"
#include "rocksdb/file_system.h"
#include "rocksdb/env.h"

namespace hfs::rocksdb_adapter {

/* Creates the Shard-82 buffered RocksDB FileSystem backed by an HFS engine. */
HFS_API std::shared_ptr<rocksdb::FileSystem>
createFileSystem(const std::string &configPath);

/* Creates RocksDB's composite Env wrapper around the HFS FileSystem. */
HFS_API std::unique_ptr<rocksdb::Env> createCompositeEnv(const std::string &configPath);

} // namespace hfs::rocksdb_adapter
