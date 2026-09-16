#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "hfs/export.hpp"
#include "hfs/types.hpp"

namespace hfs {

/* One SPDK bdev declared in the .conf device set. */
struct DeviceConfig {
    std::string id;       /* logical id used by metadata_devices/data_devices */
    std::string bdevName; /* SPDK bdev name, e.g. Nvme0n1 */
    std::string pciBdf;   /* PCI BDF for the DPDK allowlist, e.g. 0000:0b:00.0 */
    bool metadata = false;
    bool data = false;
    std::uint32_t weight = 1;
    bool required = true;
};

/* Parsed HFS 2.0 `.conf` configuration (schema_version 1). */
struct Config {
    /* [hfs] */
    std::uint32_t schemaVersion = 0;
    std::string instanceName;
    std::string volumeUuid;
    std::string processLock;     /* only "exclusive" is supported */
    std::string processLockPath; /* optional; defaults to /tmp/hfs2-<instance>.lock */
    /* [spdk] */
    std::string envDriver;
    std::uint64_t hugepages = 0;
    /* [storage] */
    std::vector<std::string> metadataDevices;
    std::vector<std::string> dataDevices;
    std::string placement;
    std::uint32_t metadataReplication = 1;
    bool requireAllDevices = true;
    std::uint32_t metadataBlockSize = 0;
    std::uint64_t journalMb = 0;
    std::uint64_t checkpointMb = 0;
    std::uint64_t extentMb = 0;
    std::uint32_t preallocExtents = 0;
    std::uint64_t maxExtentsPerFile = 0;
    /* [runtime] */
    std::uint32_t dataReactors = 0; /* 0 = auto */
    std::uint32_t maxDataReactors = 0;
    std::uint32_t metadataCpu = 0;
    std::string dataCpus; /* "auto" or comma list */
    std::uint32_t ringDepth = 0;
    std::uint32_t clientDepth = 0;
    /* [limits] */
    std::uint64_t dmaArenaMb = 0;
    std::uint32_t operationPool = 0;
    std::uint32_t coroutineFramePool = 0;
    std::uint64_t metadataWaitTimeoutMs = 0;
    std::uint32_t backendRetryLimit = 0;
    /* [metadata] */
    std::uint64_t maxInodes = 0;
    std::uint64_t maxNamespaceBytes = 0;
    std::uint32_t checkpointTriggerPercent = 0;
    std::uint32_t checkpointForcePercent = 0;
    /* [observability] */
    bool trace = false;
    std::uint32_t traceSample = 1;
    std::string traceDir;
    bool stats = false;
    /* [device.<id>] */
    std::vector<DeviceConfig> devices;

    const DeviceConfig *findDevice(const std::string &id) const noexcept;
};

/* Check the semantic rules of a parsed config (shared by parser and engine). */
HFS_API Result<void> validateConfig(const Config &config);

/* Parse and validate a `.conf` file; unknown keys fail with the line number. */
HFS_API Result<Config> parseConfigFile(const std::string &path);

/* Parse from an in-memory buffer; origin is used in diagnostics. */
HFS_API Result<Config> parseConfig(const std::string &text, const std::string &origin);

} // namespace hfs
