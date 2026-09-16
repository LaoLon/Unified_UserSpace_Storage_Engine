#include <cstdio>
#include <cstring>
#include <string>

#include "hfs/config.hpp"
#include "hfs/core.hpp"

namespace {

void printUsage() {
    std::fprintf(stderr,
                 "usage: hfsctl [--config=PATH] <command>\n"
                 "\n"
                 "commands (schema_version 1 skeleton):\n"
                 "  validate   parse and validate the .conf\n"
                 "\n"
                 "options:\n"
                 "  --config=PATH   HFS 2.0 .conf file (required)\n"
                 "  --version       print version and ABI\n"
                 "  -h, --help      show this help\n");
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

} // namespace

int main(int argc, char **argv) {
    std::string configPath;
    std::string command;

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
        printSummary(parsed.value());
        return 0;
    }

    std::fprintf(stderr, "hfsctl: command '%s' is not implemented in schema_version 1\n",
                 command.c_str());
    return 2;
}
