#pragma once

#include <memory>
#include <string>

#include "hfs/export.hpp"
#include "rocksdb/file_system.h"

namespace hfs::rocksdb_adapter {

/* Creates the Shard-82 buffered RocksDB FileSystem backed by an HFS engine. */
HFS_API std::shared_ptr<rocksdb::FileSystem>
createFileSystem(const std::string &configPath);

} // namespace hfs::rocksdb_adapter
