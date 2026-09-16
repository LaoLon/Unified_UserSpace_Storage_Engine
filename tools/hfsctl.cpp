#include <cstdio>
#include <cstring>
#include <string>

#include "hfs/config.hpp"
#include "hfs/core.hpp"

namespace {

void printUsage() {
    std::fprintf(stderr,
                 "usage: hfsctl [--config=PATH] <command> [options]\n"
                 "\n"
                 "commands:\n"
                 "  validate          parse and validate the .conf\n"
                 "  format --yes      destructive format of the configured device set\n"
                 "  info              mount read-only metadata and print the volume layout\n"
                 "\n"
                 "options:\n"
                 "  --config=PATH     HFS 2.0 .conf file (required)\n"
                 "  --yes             required confirmation for format\n"
                 "  --version         print version and ABI\n"
                 "  -h, --help        show this help\n");
}

void printSummary(const hfs::Config &config) {
    std::printf("instance:   %s\n", config.instanceName.c_str());
    std::printf("schema:     %u\n", config.schemaVersion);
    std::printf("env:        %s, hugepages=%llu\n", config.envDriver.c_str(),
                static_cast<unsigned long long>(config.hugepages));
    std::printf("placement:  %s, metadata_replication=%u\n", config.placement.c_str(),
                config.metadataReplication);
    std::printf("devices:    %zu\n", config.devices.size());
    for (const auto &device : config.devices) {
        std::printf("  - %-8s bdev=%-10s bdf=%-12s role=%s%s weight=%u required=%s\n",
                    device.id.c_str(), device.bdevName.c_str(), device.pciBdf.c_str(),
                    device.metadata ? "metadata" : "", device.data ? ",data" : "",
                    device.weight, device.required ? "true" : "false");
    }
    std::printf("runtime:    data_reactors=%u (max %u), ring_depth=%u, client_depth=%u\n",
                config.dataReactors, config.maxDataReactors, config.ringDepth,
                config.clientDepth);
    std::printf("limits:     dma_arena_mb=%llu, operation_pool=%u, frame_pool=%u\n",
                static_cast<unsigned long long>(config.dmaArenaMb), config.operationPool,
                config.coroutineFramePool);
    std::printf("trace:      %s (sample=%u, dir=%s), stats=%s\n",
                config.trace ? "on" : "off", config.traceSample, config.traceDir.c_str(),
                config.stats ? "on" : "off");
}

void printUuid(const std::array<std::uint8_t, 16> &uuid) {
    for (std::size_t i = 0; i < uuid.size(); ++i) {
        std::printf("%02x", uuid[i]);
        if (i == 3 || i == 5 || i == 7 || i == 9) {
            std::printf("-");
        }
    }
}

int runValidate(const hfs::Config &config) {
    printSummary(config);
    return 0;
}

int runFormat(const hfs::Config &config, bool confirm) {
    if (!confirm) {
        std::fprintf(stderr,
                     "hfsctl: format destroys all data on the configured device set; "
                     "re-run with --yes\n");
        return 2;
    }
    auto created = hfs::Engine::create(config);
    if (!created.ok()) {
        std::fprintf(stderr, "hfsctl: engine create failed: %s\n", created.message().c_str());
        return 1;
    }
    auto engine = std::move(created).value();
    auto result = engine->format({true}).wait();
    if (!result.ok() || !result.value().ok()) {
        std::fprintf(stderr, "hfsctl: format failed: %s\n",
                     result.ok() ? result.value().message().c_str() : result.message().c_str());
        return 1;
    }
    std::printf("formatted volume uuid ");
    printUuid(engine->volumeUuid());
    std::printf("\ndevices:\n");
    for (const auto &geometry : engine->deviceGeometries()) {
        std::printf("  - device %u: blocks=%llu data_start=%llu data_blocks=%llu\n",
                    geometry.deviceId, static_cast<unsigned long long>(geometry.deviceBlocks),
                    static_cast<unsigned long long>(geometry.dataStart),
                    static_cast<unsigned long long>(geometry.dataBlocks));
    }
    return 0;
}

int runInfo(const hfs::Config &config) {
    auto created = hfs::Engine::create(config);
    if (!created.ok()) {
        std::fprintf(stderr, "hfsctl: engine create failed: %s\n", created.message().c_str());
        return 1;
    }
    auto engine = std::move(created).value();
    auto mounted = engine->mount().wait();
    if (!mounted.ok() || !mounted.value().ok()) {
        std::fprintf(stderr, "hfsctl: mount failed: %s\n",
                     mounted.ok() ? mounted.value().message().c_str() : mounted.message().c_str());
        return 1;
    }
    std::printf("volume uuid ");
    printUuid(engine->volumeUuid());
    std::printf("\ndevices:\n");
    for (const auto &geometry : engine->deviceGeometries()) {
        std::printf("  - device %u: blocks=%llu journal=[%llu,%llu) checkpoint=[%llu,%llu) "
                    "data=[%llu,%llu)\n",
                    geometry.deviceId, static_cast<unsigned long long>(geometry.deviceBlocks),
                    static_cast<unsigned long long>(geometry.journalStart),
                    static_cast<unsigned long long>(geometry.journalStart +
                                                    geometry.journalBlocks),
                    static_cast<unsigned long long>(geometry.checkpointAStart),
                    static_cast<unsigned long long>(geometry.checkpointAStart +
                                                    geometry.checkpointBlocks),
                    static_cast<unsigned long long>(geometry.dataStart),
                    static_cast<unsigned long long>(geometry.dataStart +
                                                    geometry.dataBlocks));
    }
    auto stopped = engine->stop().wait();
    if (!stopped.ok() || !stopped.value().ok()) {
        std::fprintf(stderr, "hfsctl: stop failed\n");
        return 1;
    }
    return 0;
}

} // namespace

int main(int argc, char **argv) {
    std::string configPath;
    std::string command;
    bool confirm = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--version") {
            std::printf("hfsctl (HFS 2.0) ABI %u\n", hfs::coreAbiMajor());
            return 0;
        }
        if (arg == "-h" || arg == "--help") {
            printUsage();
            return 0;
        }
        if (arg == "--yes") {
            confirm = true;
            continue;
        }
        if (arg.rfind("--config=", 0) == 0) {
            configPath = arg.substr(std::strlen("--config="));
            continue;
        }
        if (arg == "--config" && i + 1 < argc) {
            configPath = argv[++i];
            continue;
        }
        if (!arg.empty() && arg[0] == '-') {
            std::fprintf(stderr, "hfsctl: unknown option '%s'\n", arg.c_str());
            return 2;
        }
        if (!command.empty()) {
            std::fprintf(stderr, "hfsctl: unexpected argument '%s'\n", arg.c_str());
            return 2;
        }
        command = arg;
    }

    if (configPath.empty()) {
        printUsage();
        return 2;
    }
    if (command.empty()) {
        command = "validate";
    }

    auto parsed = hfs::parseConfigFile(configPath);
    if (!parsed.ok()) {
        std::fprintf(stderr, "hfsctl: %s\n", parsed.message().c_str());
        return 1;
    }

    if (command == "validate") {
        return runValidate(parsed.value());
    }
    if (command == "format") {
        return runFormat(parsed.value(), confirm);
    }
    if (command == "info") {
        return runInfo(parsed.value());
    }

    std::fprintf(stderr, "hfsctl: command '%s' is not implemented\n", command.c_str());
    return 2;
}
