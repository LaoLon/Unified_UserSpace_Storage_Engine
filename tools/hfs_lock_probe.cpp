/* Holds the HFS process lock without opening a storage runtime. */

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include "hfs/config.hpp"
#include "hfs/core.hpp"

int main(int argc, char **argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: hfs_lock_probe CONFIG SECONDS\n");
        return 2;
    }
    char *end = nullptr;
    const unsigned long seconds = std::strtoul(argv[2], &end, 10);
    if (end == argv[2] || *end != '\0' || seconds == 0) {
        std::fprintf(stderr, "invalid hold duration\n");
        return 2;
    }
    auto config = hfs::parseConfigFile(argv[1]);
    if (!config.ok()) {
        std::fprintf(stderr, "config parse failed: %s\n", config.message().c_str());
        return 3;
    }
    auto engine = hfs::Engine::create(config.value());
    if (!engine.ok()) {
        std::fprintf(stderr, "engine create failed: status=%u message=%s\n",
                     static_cast<unsigned>(engine.status()), engine.message().c_str());
        return engine.status() == hfs::Status::Busy ? 4 : 3;
    }
    std::printf("lock acquired\n");
    std::fflush(stdout);
    std::this_thread::sleep_for(std::chrono::seconds(seconds));
    return 0;
}
