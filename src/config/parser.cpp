#include "hfs/config.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <fstream>
#include <initializer_list>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace hfs {

namespace {

constexpr std::uint32_t kSupportedSchemaVersion = 1;
constexpr const char *kDefaultTraceDir = "/mnt/hfs-data";

/* ── text helpers ─────────────────────────────────────────────────────── */

std::string trim(const std::string &text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin])) != 0) {
        ++begin;
    }
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) {
        --end;
    }
    return text.substr(begin, end - begin);
}

bool parseUint64(const std::string &text, std::uint64_t &out) {
    if (text.empty()) {
        return false;
    }
    std::uint64_t value = 0;
    const char *begin = text.data();
    const char *end = begin + text.size();
    auto result = std::from_chars(begin, end, value);
    if (result.ec != std::errc() || result.ptr != end) {
        return false;
    }
    out = value;
    return true;
}

bool parseUint32(const std::string &text, std::uint32_t &out) {
    std::uint64_t value = 0;
    if (!parseUint64(text, value) || value > 0xffffffffULL) {
        return false;
    }
    out = static_cast<std::uint32_t>(value);
    return true;
}

bool parseBool(const std::string &text, bool &out) {
    if (text == "true") {
        out = true;
        return true;
    }
    if (text == "false") {
        out = false;
        return true;
    }
    return false;
}

std::vector<std::string> splitList(const std::string &text) {
    std::vector<std::string> items;
    std::size_t begin = 0;
    while (begin <= text.size()) {
        std::size_t comma = text.find(',', begin);
        std::string item = comma == std::string::npos
                               ? text.substr(begin)
                               : text.substr(begin, comma - begin);
        item = trim(item);
        if (!item.empty()) {
            items.push_back(std::move(item));
        }
        if (comma == std::string::npos) {
            break;
        }
        begin = comma + 1;
    }
    return items;
}

bool isPowerOfTwo(std::uint64_t value) {
    return value != 0 && (value & (value - 1)) == 0;
}

/* ── schema ───────────────────────────────────────────────────────────── */

struct SectionSpec {
    const char *name;
    std::vector<const char *> keys;
};

const std::vector<SectionSpec> kSections = {
    {"hfs", {"schema_version", "instance_name", "volume_uuid", "process_lock", "process_lock_path"}},
    {"spdk", {"env_driver", "hugepages"}},
    {"storage",
     {"metadata_devices", "data_devices", "placement", "metadata_replication",
      "require_all_devices", "metadata_block_size", "journal_mb", "checkpoint_mb",
      "extent_mb", "prealloc_extents", "max_extents_per_file"}},
    {"runtime",
     {"data_reactors", "max_data_reactors", "metadata_cpu", "data_cpus", "ring_depth",
      "client_depth"}},
    {"limits",
     {"dma_arena_mb", "operation_pool", "coroutine_frame_pool", "metadata_wait_timeout_ms",
      "backend_retry_limit"}},
    {"metadata",
     {"max_inodes", "max_namespace_bytes", "checkpoint_trigger_percent",
      "checkpoint_force_percent"}},
    {"observability", {"trace", "trace_sample", "trace_dir", "stats"}},
};

const SectionSpec kDeviceSection{"device", {"bdev_name", "pci_bdf", "role", "weight", "required"}};

const SectionSpec *findSection(const std::string &name) {
    for (const auto &section : kSections) {
        if (name == section.name) {
            return &section;
        }
    }
    if (name.rfind("device.", 0) == 0 && name.size() > std::string("device.").size()) {
        return &kDeviceSection;
    }
    return nullptr;
}

bool keyAllowed(const SectionSpec &section, const std::string &key) {
    for (const char *allowed : section.keys) {
        if (key == allowed) {
            return true;
        }
    }
    return false;
}

/* ── line parser ──────────────────────────────────────────────────────── */

struct Entry {
    std::string value;
    std::size_t line = 0;
};

class KeyStore {
public:
    bool insert(const std::string &fullKey, std::string value, std::size_t line,
                std::string &error) {
        auto [it, inserted] = entries_.emplace(fullKey, Entry{std::move(value), line});
        if (!inserted) {
            error = "duplicate key '" + fullKey + "' at line " + std::to_string(line) +
                    " (first at line " + std::to_string(it->second.line) + ")";
            return false;
        }
        return true;
    }

    const Entry *find(const std::string &fullKey) const {
        auto it = entries_.find(fullKey);
        return it == entries_.end() ? nullptr : &it->second;
    }

    bool requireString(const std::string &fullKey, std::string &out, std::string &error) const {
        const Entry *entry = find(fullKey);
        if (entry == nullptr) {
            error = "missing key '" + fullKey + "'";
            return false;
        }
        out = entry->value;
        return true;
    }

    bool requireUint32(const std::string &fullKey, std::uint32_t &out,
                       std::string &error) const {
        const Entry *entry = find(fullKey);
        if (entry == nullptr) {
            error = "missing key '" + fullKey + "'";
            return false;
        }
        if (!parseUint32(entry->value, out)) {
            error = "key '" + fullKey + "' must be an unsigned 32-bit integer at line " +
                    std::to_string(entry->line);
            return false;
        }
        return true;
    }

    bool requireUint64(const std::string &fullKey, std::uint64_t &out,
                       std::string &error) const {
        const Entry *entry = find(fullKey);
        if (entry == nullptr) {
            error = "missing key '" + fullKey + "'";
            return false;
        }
        if (!parseUint64(entry->value, out)) {
            error = "key '" + fullKey + "' must be an unsigned 64-bit integer at line " +
                    std::to_string(entry->line);
            return false;
        }
        return true;
    }

    bool requireBool(const std::string &fullKey, bool &out, std::string &error) const {
        const Entry *entry = find(fullKey);
        if (entry == nullptr) {
            error = "missing key '" + fullKey + "'";
            return false;
        }
        if (!parseBool(entry->value, out)) {
            error = "key '" + fullKey + "' must be true or false at line " +
                    std::to_string(entry->line);
            return false;
        }
        return true;
    }

private:
    std::map<std::string, Entry> entries_;
};

/* ── validation helpers ───────────────────────────────────────────────── */

bool isAutoOrCpuList(const std::string &text) {
    if (text == "auto") {
        return true;
    }
    for (const auto &item : splitList(text)) {
        std::uint32_t cpu = 0;
        if (!parseUint32(item, cpu)) {
            return false;
        }
    }
    return true;
}

} // namespace

/* ── Config helpers ───────────────────────────────────────────────────── */

const DeviceConfig *Config::findDevice(const std::string &id) const noexcept {
    for (const auto &device : devices) {
        if (device.id == id) {
            return &device;
        }
    }
    return nullptr;
}

/* ── parse ────────────────────────────────────────────────────────────── */

Result<Config> parseConfig(const std::string &text, const std::string &origin) {
    auto invalid = [](const std::string &message) -> Result<Config> {
        return makeError<Config>(Status::InvalidArgument, message);
    };
    KeyStore store;
    std::string section;
    std::set<std::string> deviceIds;
    std::istringstream stream(text);
    std::string rawLine;
    std::size_t lineNumber = 0;

    while (std::getline(stream, rawLine)) {
        ++lineNumber;
        std::string line = trim(rawLine);
        if (line.empty() || line[0] == '#' || line[0] == ';') {
            continue;
        }
        if (line.front() == '[') {
            if (line.back() != ']') {
                return invalid(origin + ":" + std::to_string(lineNumber) +
                               ": malformed section header");
            }
            section = trim(line.substr(1, line.size() - 2));
            if (findSection(section) == nullptr) {
                return invalid(origin + ":" + std::to_string(lineNumber) +
                               ": unknown section [" + section + "]");
            }
            if (section.rfind("device.", 0) == 0) {
                std::string id = section.substr(std::string("device.").size());
                if (!deviceIds.insert(id).second) {
                    return invalid(origin + ":" + std::to_string(lineNumber) +
                                   ": duplicate device section [" + section + "]");
                }
            }
            continue;
        }
        std::size_t equals = line.find('=');
        if (equals == std::string::npos) {
            return invalid(origin + ":" + std::to_string(lineNumber) +
                           ": expected 'key = value'");
        }
        if (section.empty()) {
            return invalid(origin + ":" + std::to_string(lineNumber) +
                           ": key outside any section");
        }
        const SectionSpec *spec = findSection(section);
        std::string key = trim(line.substr(0, equals));
        std::string value = trim(line.substr(equals + 1));
        if (key.empty()) {
            return invalid(origin + ":" + std::to_string(lineNumber) + ": empty key");
        }
        if (!keyAllowed(*spec, key)) {
            return invalid(origin + ":" + std::to_string(lineNumber) + ": unknown key '" +
                           section + "." + key + "'");
        }
        std::string error;
        if (!store.insert(section + "." + key, std::move(value), lineNumber, error)) {
            return invalid(origin + ":" + std::to_string(lineNumber) + ": " + error);
        }
    }

    Config config;
    std::string error;
    auto fail = [&](const std::string &message) {
        return invalid(origin + ": " + message);
    };
    auto deviceKey = [](const std::string &id, const std::string &key) {
        return "device." + id + "." + key;
    };

    /* [hfs] */
    if (!store.requireUint32("hfs.schema_version", config.schemaVersion, error)) {
        return fail(error);
    }
    if (!store.requireString("hfs.instance_name", config.instanceName, error) ||
        !store.requireString("hfs.volume_uuid", config.volumeUuid, error) ||
        !store.requireString("hfs.process_lock", config.processLock, error)) {
        return fail(error);
    }
    if (const Entry *entry = store.find("hfs.process_lock_path"); entry != nullptr) {
        config.processLockPath = entry->value;
    }

    /* [spdk] */
    if (!store.requireString("spdk.env_driver", config.envDriver, error) ||
        !store.requireUint64("spdk.hugepages", config.hugepages, error)) {
        return fail(error);
    }

    /* [storage] */
    std::string metadataDevices;
    std::string dataDevices;
    std::string placement;
    if (!store.requireString("storage.metadata_devices", metadataDevices, error) ||
        !store.requireString("storage.data_devices", dataDevices, error) ||
        !store.requireString("storage.placement", placement, error) ||
        !store.requireUint32("storage.metadata_replication", config.metadataReplication,
                             error) ||
        !store.requireBool("storage.require_all_devices", config.requireAllDevices, error) ||
        !store.requireUint32("storage.metadata_block_size", config.metadataBlockSize, error) ||
        !store.requireUint64("storage.journal_mb", config.journalMb, error) ||
        !store.requireUint64("storage.checkpoint_mb", config.checkpointMb, error) ||
        !store.requireUint64("storage.extent_mb", config.extentMb, error) ||
        !store.requireUint32("storage.prealloc_extents", config.preallocExtents, error) ||
        !store.requireUint64("storage.max_extents_per_file", config.maxExtentsPerFile,
                             error)) {
        return fail(error);
    }
    config.metadataDevices = splitList(metadataDevices);
    config.dataDevices = splitList(dataDevices);
    config.placement = placement;

    /* [device.<id>] */
    for (const auto &id : deviceIds) {
        DeviceConfig device;
        device.id = id;
        std::string role;
        if (!store.requireString(deviceKey(id, "bdev_name"), device.bdevName, error) ||
            !store.requireString(deviceKey(id, "role"), role, error) ||
            !store.requireUint32(deviceKey(id, "weight"), device.weight, error) ||
            !store.requireBool(deviceKey(id, "required"), device.required, error)) {
            return fail(error);
        }
        if (const Entry *entry = store.find(deviceKey(id, "pci_bdf")); entry != nullptr) {
            device.pciBdf = entry->value;
        }
        for (const auto &item : splitList(role)) {
            if (item == "metadata") {
                device.metadata = true;
            } else if (item == "data") {
                device.data = true;
            } else {
                return fail("device '" + id + "' has unknown role '" + item + "'");
            }
        }
        config.devices.push_back(std::move(device));
    }

    /* [runtime] */
    std::string dataCpus;
    if (!store.requireUint32("runtime.data_reactors", config.dataReactors, error) ||
        !store.requireUint32("runtime.max_data_reactors", config.maxDataReactors, error) ||
        !store.requireUint32("runtime.metadata_cpu", config.metadataCpu, error) ||
        !store.requireString("runtime.data_cpus", dataCpus, error) ||
        !store.requireUint32("runtime.ring_depth", config.ringDepth, error) ||
        !store.requireUint32("runtime.client_depth", config.clientDepth, error)) {
        return fail(error);
    }
    config.dataCpus = dataCpus;

    /* [limits] */
    if (!store.requireUint64("limits.dma_arena_mb", config.dmaArenaMb, error) ||
        !store.requireUint32("limits.operation_pool", config.operationPool, error) ||
        !store.requireUint32("limits.coroutine_frame_pool", config.coroutineFramePool,
                             error) ||
        !store.requireUint64("limits.metadata_wait_timeout_ms", config.metadataWaitTimeoutMs,
                             error) ||
        !store.requireUint32("limits.backend_retry_limit", config.backendRetryLimit, error)) {
        return fail(error);
    }

    /* [metadata] */
    if (!store.requireUint64("metadata.max_inodes", config.maxInodes, error) ||
        !store.requireUint64("metadata.max_namespace_bytes", config.maxNamespaceBytes, error) ||
        !store.requireUint32("metadata.checkpoint_trigger_percent",
                             config.checkpointTriggerPercent, error) ||
        !store.requireUint32("metadata.checkpoint_force_percent",
                             config.checkpointForcePercent, error)) {
        return fail(error);
    }

    /* [observability] */
    if (!store.requireBool("observability.trace", config.trace, error) ||
        !store.requireUint32("observability.trace_sample", config.traceSample, error) ||
        !store.requireString("observability.trace_dir", config.traceDir, error) ||
        !store.requireBool("observability.stats", config.stats, error)) {
        return fail(error);
    }
    if (config.traceDir.empty()) {
        config.traceDir = kDefaultTraceDir;
    }

    auto valid = validateConfig(config);
    if (!valid.ok()) {
        return invalid(origin + ": " + valid.message());
    }
    return config;
}

Result<Config> parseConfigFile(const std::string &path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        return makeError<Config>(Status::NotFound, "cannot open config file: " + path);
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    if (file.bad()) {
        return makeError<Config>(Status::IoError, "cannot read config file: " + path);
    }
    return parseConfig(buffer.str(), path);
}

/* ── semantic validation ──────────────────────────────────────────────── */

Result<void> validateConfig(const Config &config) {
    auto invalid = [](const std::string &message) -> Result<void> {
        return makeError<void>(Status::InvalidArgument, message);
    };
    if (config.schemaVersion != kSupportedSchemaVersion) {
        return invalid("unsupported schema_version " + std::to_string(config.schemaVersion) +
                       " (expected " + std::to_string(kSupportedSchemaVersion) + ")");
    }
    if (config.instanceName.empty()) {
        return invalid("instance_name must not be empty");
    }
    if (config.volumeUuid.empty()) {
        return invalid("volume_uuid must not be empty");
    }
    if (config.processLock != "exclusive") {
        return invalid("process_lock must be 'exclusive'");
    }
    if (config.processLockPath.empty()) {
        return invalid("process_lock_path must not be empty");
    }

    if (config.envDriver.empty()) {
        return invalid("env_driver must not be empty");
    }
    if (config.envDriver != "uio" && config.envDriver != "vfio") {
        return invalid("env_driver must be 'uio' or 'vfio'");
    }
    if (config.hugepages == 0) {
        return invalid("hugepages must be greater than zero");
    }

    if (config.metadataDevices.empty()) {
        return invalid("metadata_devices must not be empty");
    }
    if (config.dataDevices.empty()) {
        return invalid("data_devices must not be empty");
    }
    if (config.placement != "capacity_weighted" && config.placement != "round_robin") {
        return invalid("placement must be 'capacity_weighted' or 'round_robin'");
    }
    if (!config.requireAllDevices) {
        return invalid("require_all_devices must be true in schema_version 1");
    }
    if (config.metadataReplication == 0 ||
        config.metadataReplication > config.metadataDevices.size()) {
        return invalid("metadata_replication must be between 1 and the metadata device count");
    }
    if (config.metadataBlockSize < 512 || !isPowerOfTwo(config.metadataBlockSize)) {
        return invalid("metadata_block_size must be a power of two >= 512");
    }
    if (config.journalMb == 0 || config.checkpointMb == 0) {
        return invalid("journal_mb and checkpoint_mb must be greater than zero");
    }
    if (config.extentMb == 0 || !isPowerOfTwo(config.extentMb)) {
        return invalid("extent_mb must be a power of two");
    }
    if (config.preallocExtents == 0 || config.preallocExtents > 4096) {
        return invalid("prealloc_extents must be between 1 and 4096");
    }
    if (config.maxExtentsPerFile == 0) {
        return invalid("max_extents_per_file must be greater than zero");
    }

    std::set<std::string> bdevNames;
    std::set<std::string> pciBdfs;
    for (const auto &device : config.devices) {
        if (device.bdevName.empty()) {
            return invalid("device '" + device.id + "' bdev_name must not be empty");
        }
        if (config.envDriver == "uio" && device.pciBdf.empty()) {
            return invalid("device '" + device.id + "' pci_bdf is required for uio");
        }
        if (!bdevNames.insert(device.bdevName).second) {
            return invalid("duplicate bdev_name '" + device.bdevName + "'");
        }
        if (!device.pciBdf.empty() && !pciBdfs.insert(device.pciBdf).second) {
            return invalid("duplicate pci_bdf '" + device.pciBdf + "'");
        }
        if (!device.metadata && !device.data) {
            return invalid("device '" + device.id + "' needs a metadata and/or data role");
        }
    }

    auto checkDeviceList = [&](const std::vector<std::string> &list,
                               const std::string &label) -> Result<void> {
        std::set<std::string> ids;
        for (const auto &id : list) {
            if (!ids.insert(id).second) {
                return invalid(label + " contains duplicate device '" + id + "'");
            }
            const DeviceConfig *device = config.findDevice(id);
            if (device == nullptr) {
                return invalid(label + " references unknown device '" + id + "'");
            }
        }
        return Result<void>(Status::Ok);
    };
    auto metadataCheck = checkDeviceList(config.metadataDevices, "metadata_devices");
    if (!metadataCheck.ok()) {
        return metadataCheck;
    }
    auto dataCheck = checkDeviceList(config.dataDevices, "data_devices");
    if (!dataCheck.ok()) {
        return dataCheck;
    }
    std::size_t metadataRoleCount = 0;
    for (const auto &device : config.devices) {
        if (device.metadata) {
            ++metadataRoleCount;
        }
    }
    if (metadataRoleCount != config.metadataDevices.size() ||
        config.metadataReplication != metadataRoleCount) {
        return invalid("metadata_replication must match the complete metadata device set");
    }
    for (const auto &id : config.metadataDevices) {
        if (!config.findDevice(id)->metadata) {
            return invalid("device '" + id + "' is listed in metadata_devices but its role "
                           "does not include metadata");
        }
    }
    for (const auto &id : config.dataDevices) {
        if (!config.findDevice(id)->data) {
            return invalid("device '" + id + "' is listed in data_devices but its role "
                           "does not include data");
        }
    }

    if (config.maxDataReactors == 0) {
        return invalid("max_data_reactors must be greater than zero");
    }
    if (config.dataReactors > config.maxDataReactors) {
        return invalid("data_reactors must not exceed max_data_reactors");
    }
    if (!isAutoOrCpuList(config.dataCpus)) {
        return invalid("data_cpus must be 'auto' or a comma-separated CPU list");
    }
    if (config.ringDepth == 0 || config.clientDepth == 0) {
        return invalid("ring_depth and client_depth must be greater than zero");
    }

    if (config.dmaArenaMb == 0) {
        return invalid("dma_arena_mb must be greater than zero");
    }
    if (config.operationPool == 0 || config.coroutineFramePool == 0) {
        return invalid("operation_pool and coroutine_frame_pool must be greater than zero");
    }
    if (config.metadataWaitTimeoutMs == 0) {
        return invalid("metadata_wait_timeout_ms must be greater than zero");
    }
    if (config.backendRetryLimit == 0) {
        return invalid("backend_retry_limit must be greater than zero");
    }

    if (config.maxInodes == 0 || config.maxNamespaceBytes == 0) {
        return invalid("max_inodes and max_namespace_bytes must be greater than zero");
    }
    if (config.checkpointTriggerPercent == 0 ||
        config.checkpointTriggerPercent >= config.checkpointForcePercent ||
        config.checkpointForcePercent > 100) {
        return invalid("checkpoint thresholds must satisfy 0 < trigger < force <= 100");
    }

    if (config.traceSample == 0) {
        return invalid("trace_sample must be greater than zero");
    }
    if (config.traceDir.empty()) {
        return invalid("trace_dir must not be empty");
    }
    return Result<void>(Status::Ok);
}

} // namespace hfs
