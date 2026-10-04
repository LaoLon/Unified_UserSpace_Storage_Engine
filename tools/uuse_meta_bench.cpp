/* UUSE 2.0 metadata microbenchmark (design §17.1): measures create/stat/rename/
 * set-size+commit throughput, the mount/replay budget and (through the engine's
 * UUSE_DIAG_SYNC_STATS log line) the checkpoint freeze/encode split on the real
 * device. Runs entirely through the public Engine/Client/File API. */

#include <algorithm>
#include <array>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#include "uuse/config.hpp"
#include "uuse/core.hpp"
#include "uuse/file.hpp"

using namespace uuse;

namespace {

struct Options {
    std::string configPath;
    std::string dir = "/meta-bench";
    std::uint64_t inodes = 100000;
    std::uint64_t statOps = 10000;
    std::uint64_t renameOps = 10000;
    std::uint64_t commitOps = 1000;
    /* Periodic sync keeps the journal bounded and exercises checkpoints. */
    std::uint64_t syncEvery = 100000;
    bool format = true;
    bool mountOnly = false;
};

bool parseU64(const std::string &value, std::uint64_t *out) {
    if (value.empty() || out == nullptr) {
        return false;
    }
    char *end = nullptr;
    const unsigned long long parsed = std::strtoull(value.c_str(), &end, 10);
    if (end == value.c_str() || *end != '\0') {
        return false;
    }
    *out = parsed;
    return true;
}

bool parseArguments(int argc, char **argv, Options *options) {
    for (int index = 1; index < argc; ++index) {
        const std::string arg = argv[index];
        const auto split = arg.find('=');
        const std::string name = split == std::string::npos ? arg : arg.substr(0, split);
        const std::string value = split == std::string::npos ? "" : arg.substr(split + 1);
        if (name == "--config") {
            options->configPath = value;
        } else if (name == "--dir") {
            options->dir = value;
        } else if (name == "--inodes") {
            if (!parseU64(value, &options->inodes)) return false;
        } else if (name == "--stat-ops") {
            if (!parseU64(value, &options->statOps)) return false;
        } else if (name == "--rename-ops") {
            if (!parseU64(value, &options->renameOps)) return false;
        } else if (name == "--commit-ops") {
            if (!parseU64(value, &options->commitOps)) return false;
        } else if (name == "--sync-every") {
            if (!parseU64(value, &options->syncEvery)) return false;
        } else if (name == "--format") {
            options->format = value != "0";
        } else if (name == "--mount-only") {
            options->mountOnly = value != "0";
        } else {
            return false;
        }
    }
    return !options->configPath.empty() && options->inodes != 0 && options->dir.size() > 1;
}

double elapsedSeconds(const std::chrono::steady_clock::time_point &start) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

std::string filePath(const std::string &dir, const char *prefix, std::uint64_t index) {
    char buffer[512];
    std::snprintf(buffer, sizeof(buffer), "%s/%s%08" PRIu64, dir.c_str(), prefix,
                  static_cast<std::uint64_t>(index));
    return buffer;
}

void reportPhase(const char *name, std::uint64_t operations, double seconds) {
    const double rate = seconds > 0.0 ? static_cast<double>(operations) / seconds : 0.0;
    std::printf("%s_ops=%" PRIu64 " %s_seconds=%.3f %s_ops_per_sec=%.1f\n", name,
                static_cast<std::uint64_t>(operations), name, seconds, name, rate);
}

/* Opens one path and releases it immediately (close-on-destruction). */
bool touchFile(const std::shared_ptr<Client> &client, const std::string &path,
               std::string *error) {
    auto opened = client->open(path, OpenFlags::Create | OpenFlags::Truncate).wait().value();
    if (!opened.ok()) {
        *error = opened.message();
        return false;
    }
    auto file = std::move(opened).value();
    file.reset();
    return true;
}

} // namespace

int main(int argc, char **argv) {
    Options options;
    if (!parseArguments(argc, argv, &options)) {
        std::fprintf(stderr,
                     "usage: uuse_meta_bench --config=PATH [--dir=/meta-bench] [--inodes=N] "
                     "[--stat-ops=N] [--rename-ops=N] [--commit-ops=N] [--sync-every=N] "
                     "[--format=0|1] [--mount-only=0|1]\n");
        return 2;
    }
    auto parsed = parseConfigFile(options.configPath);
    if (!parsed.ok()) {
        std::fprintf(stderr, "config parse failed: %s\n", parsed.message().c_str());
        return 3;
    }
    auto created = Engine::create(parsed.value());
    if (!created.ok()) {
        std::fprintf(stderr, "engine create failed: %s\n", created.message().c_str());
        return 3;
    }
    auto engine = std::move(created).value();
    if (options.format) {
        auto formatted = engine->format({true}).wait().value();
        if (!formatted.ok()) {
            std::fprintf(stderr, "format failed: %s\n", formatted.message().c_str());
            return 4;
        }
    }
    const auto mountStart = std::chrono::steady_clock::now();
    auto mounted = engine->mount().wait().value();
    if (!mounted.ok()) {
        std::fprintf(stderr, "mount failed: %s\n", mounted.message().c_str());
        return 4;
    }
    std::printf("uuse_meta_bench: inodes=%" PRIu64 " dir=%s mount_only=%d\n",
                static_cast<std::uint64_t>(options.inodes), options.dir.c_str(),
                options.mountOnly ? 1 : 0);
    std::printf("mount_seconds=%.6f\n", elapsedSeconds(mountStart));
    if (options.mountOnly) {
        auto stopped = engine->stop().wait().value();
        return stopped.ok() ? 0 : 5;
    }

    auto clientCreated = Client::create(*engine, ClientOptions{});
    if (!clientCreated.ok()) {
        std::fprintf(stderr, "client create failed: %s\n", clientCreated.message().c_str());
        return 5;
    }
    auto client = std::move(clientCreated).value();
    auto dirCreated = client->createDirectories(options.dir).wait().value();
    if (!dirCreated.ok()) {
        std::fprintf(stderr, "createDirectories failed: %s\n", dirCreated.message().c_str());
        return 5;
    }
    std::string error;
    if (!touchFile(client, options.dir + "/anchor", &error)) {
        std::fprintf(stderr, "anchor create failed: %s\n", error.c_str());
        return 5;
    }
    auto anchorOpened = client->open(options.dir + "/anchor").wait().value();
    if (!anchorOpened.ok()) {
        std::fprintf(stderr, "anchor open failed: %s\n", anchorOpened.message().c_str());
        return 5;
    }
    auto anchor = std::move(anchorOpened).value();

    /* Phase 1: create (inode + directory entry + journal records). */
    std::uint64_t createdFiles = 0;
    {
        const auto start = std::chrono::steady_clock::now();
        for (std::uint64_t index = 0; index < options.inodes; ++index) {
            auto opened = client->open(filePath(options.dir, "f", index),
                                       OpenFlags::Create | OpenFlags::Truncate)
                              .wait()
                              .value();
            if (!opened.ok()) {
                if (opened.status() == Status::NoMemory) {
                    /* Namespace inode/byte limit reached: stop the phase and
                     * report how many files were created. */
                    std::printf("create_limit_reached_at=%" PRIu64 "\n",
                                static_cast<std::uint64_t>(index));
                    break;
                }
                std::fprintf(stderr, "create %" PRIu64 " failed: %s\n",
                             static_cast<std::uint64_t>(index), opened.message().c_str());
                return 6;
            }
            auto file = std::move(opened).value();
            file.reset();
            ++createdFiles;
            if (options.syncEvery != 0 && createdFiles % options.syncEvery == 0) {
                auto synced = anchor->sync().wait().value();
                if (!synced.ok()) {
                    std::fprintf(stderr, "periodic sync failed: %s\n",
                                 synced.message().c_str());
                    return 6;
                }
            }
        }
        reportPhase("create", createdFiles, elapsedSeconds(start));
    }

    /* Phase 2: stat (namespace read path). */
    if (options.statOps != 0) {
        const std::uint64_t ops = std::min(options.statOps, createdFiles);
        const auto start = std::chrono::steady_clock::now();
        for (std::uint64_t index = 0; index < ops; ++index) {
            auto stat = client->stat(filePath(options.dir, "f", index)).wait().value();
            if (!stat.ok()) {
                std::fprintf(stderr, "stat %" PRIu64 " failed: %s\n",
                             static_cast<std::uint64_t>(index), stat.message().c_str());
                return 6;
            }
        }
        reportPhase("stat", ops, elapsedSeconds(start));
    }

    /* Phase 3: rename (directory entry + journal commit). */
    if (options.renameOps != 0) {
        const std::uint64_t ops = std::min(options.renameOps, createdFiles);
        const auto start = std::chrono::steady_clock::now();
        for (std::uint64_t index = 0; index < ops; ++index) {
            auto renamed = client->rename(filePath(options.dir, "f", index),
                                          filePath(options.dir, "r", index))
                               .wait()
                               .value();
            if (!renamed.ok()) {
                std::fprintf(stderr, "rename %" PRIu64 " failed: %s\n",
                             static_cast<std::uint64_t>(index), renamed.message().c_str());
                return 6;
            }
        }
        reportPhase("rename", ops, elapsedSeconds(start));
    }

    /* Phase 4: set-size + durable commit (data write + fsync barrier). */
    if (options.commitOps != 0) {
        std::array<std::byte, 4096> buffer{};
        const std::uint64_t ops = std::min(options.commitOps, createdFiles);
        const auto start = std::chrono::steady_clock::now();
        for (std::uint64_t index = 0; index < ops; ++index) {
            auto opened =
                client->open(filePath(options.dir, "r", index), OpenFlags::None).wait().value();
            if (!opened.ok()) {
                std::fprintf(stderr, "open %" PRIu64 " failed: %s\n",
                             static_cast<std::uint64_t>(index), opened.message().c_str());
                return 6;
            }
            auto file = std::move(opened).value();
            std::array<IoVec, 1> vecs{IoVec{buffer.data(), buffer.size()}};
            auto written = file->write(vecs, 0).wait().value();
            if (!written.ok()) {
                std::fprintf(stderr, "write %" PRIu64 " failed: %s\n",
                             static_cast<std::uint64_t>(index), written.message().c_str());
                return 6;
            }
            auto synced = file->sync().wait().value();
            if (!synced.ok()) {
                std::fprintf(stderr, "sync %" PRIu64 " failed: %s\n",
                             static_cast<std::uint64_t>(index), synced.message().c_str());
                return 6;
            }
            file.reset();
        }
        reportPhase("commit", ops, elapsedSeconds(start));
    }

    anchor.reset();
    auto closed = client->close().wait().value();
    if (!closed.ok()) {
        std::fprintf(stderr, "client close failed: %s\n", closed.message().c_str());
        return 7;
    }
    auto stopped = engine->stop().wait().value();
    if (!stopped.ok()) {
        std::fprintf(stderr, "engine stop failed: %s\n", stopped.message().c_str());
        return 7;
    }

    /* Mount/replay budget (design §17.1): reopen the same volume. */
    auto remountCreated = Engine::create(parsed.value());
    if (!remountCreated.ok()) {
        std::fprintf(stderr, "remount engine create failed: %s\n",
                     remountCreated.message().c_str());
        return 8;
    }
    auto engine2 = std::move(remountCreated).value();
    const auto remountStart = std::chrono::steady_clock::now();
    auto remounted = engine2->mount().wait().value();
    if (!remounted.ok()) {
        std::fprintf(stderr, "remount failed: %s\n", remounted.message().c_str());
        return 8;
    }
    std::printf("remount_seconds=%.6f\n", elapsedSeconds(remountStart));
    auto stopped2 = engine2->stop().wait().value();
    return stopped2.ok() ? 0 : 8;
}
