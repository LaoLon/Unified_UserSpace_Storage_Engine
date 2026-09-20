#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "hfs/config.hpp"

namespace hfs {

/* NUMA placement observed for one configured PCI device. */
struct NumaDevicePlacement {
    std::string id;
    std::string pciBdf;
    std::int32_t node = -1;
};

/* CPU-to-node placement used to choose a local reactor without shelling out. */
struct NumaCpuPlacement {
    std::uint32_t cpu = 0;
    std::int32_t node = -1;
};

/* Immutable startup snapshot used for diagnostics and placement gates. */
struct NumaTopologySnapshot {
    bool available = false;
    std::vector<std::uint32_t> onlineNodes;
    std::vector<std::uint32_t> onlineCpus;
    std::vector<NumaCpuPlacement> cpuPlacements;
    std::vector<NumaDevicePlacement> devices;

    /* Formats a compact, stable record for startup logs and evidence. */
    std::string summary() const;
};

/* Parses Linux CPU/node list syntax such as "0-3,8" without shell tools. */
Result<std::vector<std::uint32_t>> parseNumaIdList(const std::string &text);

/* Reads a topology snapshot from sysfs; an unavailable sysfs is not fatal. */
NumaTopologySnapshot snapshotNumaTopology(const Config &config,
                                          const std::string &sysfsRoot = "/sys");

} // namespace hfs
