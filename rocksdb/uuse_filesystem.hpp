#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "uuse/export.hpp"
#include "rocksdb/file_system.h"
#include "rocksdb/env.h"

namespace uuse {
class Engine;
} // namespace uuse

namespace uuse::rocksdb_adapter {

/* Creates the Shard-82 buffered RocksDB FileSystem backed by an UUSE engine. */
UUSE_API std::shared_ptr<rocksdb::FileSystem>
createFileSystem(const std::string &configPath);

/* Creates the FileSystem over an already-mounted shared UUSE engine
 * (Shard-117). The caller keeps `engine` alive; the adapter does not stop it. */
UUSE_API std::shared_ptr<rocksdb::FileSystem>
createFileSystemWithEngine(std::shared_ptr<uuse::Engine> engine);

/* Creates RocksDB's composite Env wrapper around the UUSE FileSystem. */
UUSE_API std::unique_ptr<rocksdb::Env> createCompositeEnv(const std::string &configPath);

/* Test-only injection: delays each asynchronous read by N milliseconds so the
 * timeout/abort contract can be exercised deterministically. The value is
 * cached in an atomic, so ReadAsync never calls getenv; tests switch it at
 * runtime and 0 disables it. UUSE_TEST_ASYNC_READ_DELAY_MS remains the startup
 * default until the setter is used. */
UUSE_API void setAsyncReadDelayMs(std::uint32_t milliseconds) noexcept;

} // namespace uuse::rocksdb_adapter
