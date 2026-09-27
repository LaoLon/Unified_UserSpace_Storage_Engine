#include <cstdio>
#include <cstring>
#include <string>

#include "hfs/config.hpp"
#include "hfs/core.hpp"
#include "hfs/journal.hpp"

namespace {

void printUsage() {
    std::fprintf(stderr,
                 "usage: hfsctl [--config=PATH] <command> [options]\n"
                 "\n"
                 "commands:\n"
                 "  validate          parse and validate the .conf\n"
                 "  format --yes      destructive format of the configured device set\n"
                 "  info              mount read-only metadata and print the volume layout\n"
                 "  check             scan superblocks, checkpoints and the journal\n"
                 "  dump              verbose per-device superblock/checkpoint/journal dump\n"
                 "  import            Phase 2 data-plane import (not implemented yet)\n"
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

const hfs::Hfs2Superblock *newestSuperblock(const hfs::SuperblockSlots &slots) {
    if (slots.validA && slots.validB) {
        return slots.slotB.generation > slots.slotA.generation ? &slots.slotB : &slots.slotA;
    }
    if (slots.validA) {
        return &slots.slotA;
    }
    if (slots.validB) {
        return &slots.slotB;
    }
    return nullptr;
}

int runCheck(const hfs::Config &config) {
    auto created = hfs::Engine::create(config);
    if (!created.ok()) {
        std::fprintf(stderr, "hfsctl: engine create failed: %s\n", created.message().c_str());
        return 1;
    }
    auto engine = std::move(created).value();
    auto mounted = engine->mount().wait();
    if (!mounted.ok() || !mounted.value().ok()) {
        const std::string mountError =
            mounted.ok() ? mounted.value().message() : mounted.message();
        std::fprintf(stderr, "hfsctl: mount failed, falling back to scan-only check: %s\n",
                     mountError.c_str());
        auto scanEngine = hfs::Engine::create(config);
        if (!scanEngine.ok()) {
            std::fprintf(stderr, "hfsctl: scan engine create failed: %s\n",
                         scanEngine.message().c_str());
            return 1;
        }
        auto scans = scanEngine.value()->scanMetadataJournals();
        if (!scans.ok()) {
            std::fprintf(stderr, "hfsctl: scan-only check failed: %s\n",
                         scans.message().c_str());
            return 1;
        }
        bool scanOk = true;
        for (const auto &scan : scans.value()) {
            const auto &stats = scan.stats;
            std::printf("metadata device %zu: checkpointA=%s(txid %llu seq %llu) "
                        "checkpointB=%s(txid %llu seq %llu) journal head=%llu tail=%llu "
                        "last_txid=%llu transactions=%u usage=%.1f%% corruption=%s error=%s\n",
                        scan.deviceIndex, stats.checkpointAValid ? "valid" : "empty",
                        static_cast<unsigned long long>(stats.checkpointATxid),
                        static_cast<unsigned long long>(stats.checkpointASequence),
                        stats.checkpointBValid ? "valid" : "empty",
                        static_cast<unsigned long long>(stats.checkpointBTxid),
                        static_cast<unsigned long long>(stats.checkpointBSequence),
                        static_cast<unsigned long long>(stats.head),
                        static_cast<unsigned long long>(stats.tail),
                        static_cast<unsigned long long>(stats.lastTxid), stats.validTransactions,
                        scan.usageRatio * 100.0,
                        stats.corruption.empty() ? "none" : stats.corruption.c_str(),
                        scan.error.empty() ? "none" : scan.error.c_str());
            if (!scan.error.empty() || !stats.corruption.empty()) scanOk = false;
        }
        return scanOk ? 0 : 1;
    }

    bool ok = true;
    std::array<std::uint8_t, 16> uuid{};
    bool haveUuid = false;
    const auto geometries = engine->deviceGeometries();
    for (std::size_t i = 0; i < geometries.size(); ++i) {
        auto slots = engine->readSuperblockSlots(i);
        if (!slots.ok()) {
            std::printf("device %zu: superblock read failed: %s\n", i,
                        slots.message().c_str());
            ok = false;
            continue;
        }
        const hfs::Hfs2Superblock *sb = newestSuperblock(slots.value());
        if (sb == nullptr) {
            std::printf("device %zu: no valid superblock\n", i);
            ok = false;
            continue;
        }
        std::printf("device %zu (%s): SB A=%s B=%s generation=%llu replica=%u/%u "
                    "clean=%u mount_epoch=%llu\n", i,
                    config.devices[i].bdevName.c_str(),
                    slots.value().validA ? "valid" : "invalid",
                    slots.value().validB ? "valid" : "invalid",
                    static_cast<unsigned long long>(sb->generation), sb->replicaId,
                    sb->replicaCount, static_cast<unsigned int>(sb->cleanShutdown),
                    static_cast<unsigned long long>(sb->mountEpoch));
        if (!haveUuid) {
            uuid = sb->uuid;
            haveUuid = true;
        } else if (sb->uuid != uuid) {
            std::printf("  volume uuid mismatch with device 0\n");
            ok = false;
        }
    }

    for (std::size_t i = 0; i < config.devices.size(); ++i) {
        if (!config.devices[i].metadata) {
            continue;
        }
        auto journal = hfs::Journal::open(*engine, nullptr, i);
        if (!journal.ok()) {
            std::printf("metadata device %zu: journal open failed: %s\n", i,
                        journal.message().c_str());
            ok = false;
            continue;
        }
        auto replayed = journal.value()->replay();
        const auto stats = journal.value()->stats();
        std::printf("metadata device %zu: checkpointA=%s(txid %llu seq %llu) "
                    "checkpointB=%s(txid %llu seq %llu) journal head=%llu tail=%llu "
                    "last_txid=%llu transactions=%u usage=%.1f%% corruption=%s\n",
                    i, stats.checkpointAValid ? "valid" : "empty",
                    static_cast<unsigned long long>(stats.checkpointATxid),
                    static_cast<unsigned long long>(stats.checkpointASequence),
                    stats.checkpointBValid ? "valid" : "empty",
                    static_cast<unsigned long long>(stats.checkpointBTxid),
                    static_cast<unsigned long long>(stats.checkpointBSequence),
                    static_cast<unsigned long long>(stats.head),
                    static_cast<unsigned long long>(stats.tail),
                    static_cast<unsigned long long>(stats.lastTxid),
                    stats.validTransactions, journal.value()->usageRatio() * 100.0,
                    stats.corruption.empty() ? "none" : stats.corruption.c_str());
        if (!replayed.ok() || !stats.corruption.empty()) {
            ok = false;
        }
    }

    (void)engine->stop().wait();
    return ok ? 0 : 1;
}

int runDump(const hfs::Config &config) {
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

    const auto geometries = engine->deviceGeometries();
    for (std::size_t i = 0; i < geometries.size(); ++i) {
        auto slots = engine->readSuperblockSlots(i);
        if (!slots.ok()) {
            std::printf("device %zu: superblock read failed: %s\n", i,
                        slots.message().c_str());
            continue;
        }
        const auto &value = slots.value();
        auto printSlot = [&](const char *label, bool valid, const hfs::Hfs2Superblock &sb) {
            if (!valid) {
                std::printf("  %s: invalid\n", label);
                return;
            }
            std::printf("  %s: generation=%llu device=%u replica=%u/%u sector=%u meta=%u "
                        "blocks=%llu journal=[%llu,%llu) checkpoint=[%llu,%llu) "
                        "data=[%llu,%llu) clean=%u uuid=",
                        label, static_cast<unsigned long long>(sb.generation), sb.deviceId,
                        sb.replicaId, sb.replicaCount, sb.sectorSize, sb.metadataBlockSize,
                        static_cast<unsigned long long>(sb.deviceBlocks),
                        static_cast<unsigned long long>(sb.journalStart),
                        static_cast<unsigned long long>(sb.journalStart + sb.journalBlocks),
                        static_cast<unsigned long long>(sb.checkpointAStart),
                        static_cast<unsigned long long>(sb.checkpointAStart +
                                                        sb.checkpointBlocks),
                        static_cast<unsigned long long>(sb.dataStart),
                        static_cast<unsigned long long>(sb.dataStart + sb.dataBlocks),
                        sb.cleanShutdown);
            printUuid(sb.uuid);
            std::printf("\n");
        };
        std::printf("device %zu (%s):\n", i, config.devices[i].bdevName.c_str());
        printSlot("SB A", value.validA, value.slotA);
        printSlot("SB B", value.validB, value.slotB);
    }

    for (std::size_t i = 0; i < config.devices.size(); ++i) {
        if (!config.devices[i].metadata) {
            continue;
        }
        auto journal = hfs::Journal::open(*engine, nullptr, i);
        if (!journal.ok()) {
            std::printf("metadata device %zu: journal open failed: %s\n", i,
                        journal.message().c_str());
            continue;
        }
        (void)journal.value()->replay();
        const auto stats = journal.value()->stats();
        std::printf("metadata device %zu:\n", i);
        std::printf("  checkpoint A: %s txid=%llu sequence=%llu\n",
                    stats.checkpointAValid ? "valid" : "empty",
                    static_cast<unsigned long long>(stats.checkpointATxid),
                    static_cast<unsigned long long>(stats.checkpointASequence));
        std::printf("  checkpoint B: %s txid=%llu sequence=%llu\n",
                    stats.checkpointBValid ? "valid" : "empty",
                    static_cast<unsigned long long>(stats.checkpointBTxid),
                    static_cast<unsigned long long>(stats.checkpointBSequence));
        std::printf("  journal: head=%llu tail=%llu last_txid=%llu transactions=%u "
                    "usage=%.1f%% corruption=%s\n",
                    static_cast<unsigned long long>(stats.head),
                    static_cast<unsigned long long>(stats.tail),
                    static_cast<unsigned long long>(stats.lastTxid),
                    stats.validTransactions, journal.value()->usageRatio() * 100.0,
                    stats.corruption.empty() ? "none" : stats.corruption.c_str());
    }

    (void)engine->stop().wait();
    return 0;
}

int runImport(const hfs::Config &config) {
    (void)config;
    std::fprintf(stderr,
                 "hfsctl: import requires the Phase 2 data plane (namespace/extent write "
                 "path); not implemented yet\n");
    return 2;
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
    if (command == "check") {
        return runCheck(parsed.value());
    }
    if (command == "dump") {
        return runDump(parsed.value());
    }
    if (command == "import") {
        return runImport(parsed.value());
    }

    std::fprintf(stderr, "hfsctl: command '%s' is not implemented\n", command.c_str());
    return 2;
}
