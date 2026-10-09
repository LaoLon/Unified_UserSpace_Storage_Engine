#pragma once

/* C++-level accessors over the C ABI shim (Shard-117): let the RocksDB
 * FileSystem adapter share the process-wide UUSE engine owned by the shim, so
 * kvdb and raft-engine run on the same single UUSE engine per process. */

#include <memory>

#include "uuse_tikv_shim.h"

namespace uuse {
class Engine;
class Client;
} // namespace uuse

/* Returns the shared engine/client behind a shim handle. The caller must keep
 * the handle alive (process-lifetime keepalive) while using the pointers. */
std::shared_ptr<uuse::Engine> uuse_tikv_engine_shared_engine(uuse_tikv_engine *engine);
std::shared_ptr<uuse::Client> uuse_tikv_engine_shared_client(uuse_tikv_engine *engine);
