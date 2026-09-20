#include <cstdio>

#include "runtime/numa_topology.hpp"

int main() {
    {
        auto parsed = hfs::parseNumaIdList("0-2,4");
        if (!parsed.ok() || parsed.value().size() != 4 || parsed.value()[0] != 0 ||
            parsed.value()[3] != 4) {
            std::fprintf(stderr, "NUMA range parsing failed\n");
            return 1;
        }
    }
    {
        auto duplicate = hfs::parseNumaIdList("0-2,2");
        auto reversed = hfs::parseNumaIdList("3-1");
        if (duplicate.ok() || reversed.ok()) {
            std::fprintf(stderr, "NUMA invalid range accepted\n");
            return 1;
        }
    }
    {
        hfs::Config config;
        config.devices.push_back(hfs::DeviceConfig{"nvme0", "Nvme0n1",
                                                   "0000:0b:00.0", true, true, 1, true});
        const auto snapshot = hfs::snapshotNumaTopology(config, "/path/that/does/not/exist");
        if (snapshot.available || snapshot.devices.size() != 1 ||
            snapshot.devices.front().node != -1) {
            std::fprintf(stderr, "NUMA unavailable fallback failed\n");
            return 1;
        }
    }
    std::printf("numa_topology_test: ok\n");
    return 0;
}
