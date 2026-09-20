#include "runtime/numa_topology.hpp"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <filesystem>
#include <sstream>
#include <string_view>
#include <system_error>
#include <utility>

namespace hfs {

namespace {

constexpr std::size_t kMaxExpandedIds = 1U << 20;

std::string trim(std::string_view text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && (text[begin] == ' ' || text[begin] == '\t' ||
                           text[begin] == '\r' || text[begin] == '\n')) {
        ++begin;
    }
    while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t' ||
                           text[end - 1] == '\r' || text[end - 1] == '\n')) {
        --end;
    }
    return std::string(text.substr(begin, end - begin));
}

bool parseUnsigned(std::string_view text, std::uint32_t &value) {
    if (text.empty()) {
        return false;
    }
    const char *begin = text.data();
    const char *end = begin + text.size();
    std::uint32_t parsed = 0;
    const auto result = std::from_chars(begin, end, parsed);
    if (result.ec != std::errc() || result.ptr != end) {
        return false;
    }
    value = parsed;
    return true;
}

bool parseSigned(std::string_view text, std::int32_t &value) {
    const std::string cleaned = trim(text);
    if (cleaned.empty()) {
        return false;
    }
    const char *begin = cleaned.data();
    const char *end = begin + cleaned.size();
    std::int32_t parsed = 0;
    const auto result = std::from_chars(begin, end, parsed);
    if (result.ec != std::errc() || result.ptr != end) {
        return false;
    }
    value = parsed;
    return true;
}

bool readText(const std::string &path, std::string &text) {
    std::ifstream input(path);
    if (!input.is_open()) {
        return false;
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    text = buffer.str();
    return true;
}

bool readSigned(const std::string &path, std::int32_t &value) {
    std::string text;
    return readText(path, text) && parseSigned(text, value);
}

std::string joinPath(const std::string &root, const std::string &suffix);

std::int32_t cpuNode(const std::string &root, std::uint32_t cpu,
                     const std::vector<std::uint32_t> &nodes) {
    for (const std::uint32_t node : nodes) {
        std::error_code error;
        const std::string path = joinPath(
            root, "devices/system/cpu/cpu" + std::to_string(cpu) + "/node" +
                      std::to_string(node));
        if (std::filesystem::exists(path, error) && !error) {
            return static_cast<std::int32_t>(node);
        }
    }
    return -1;
}

std::string joinPath(const std::string &root, const std::string &suffix) {
    if (root.empty() || root.back() == '/') {
        return root + suffix;
    }
    return root + "/" + suffix;
}

} // namespace

Result<std::vector<std::uint32_t>> parseNumaIdList(const std::string &text) {
    const std::string cleaned = trim(text);
    if (cleaned.empty()) {
        return makeError<std::vector<std::uint32_t>>(Status::InvalidArgument,
                                                     "NUMA list is empty");
    }

    std::vector<std::uint32_t> result;
    std::size_t begin = 0;
    while (begin <= cleaned.size()) {
        const std::size_t comma = cleaned.find(',', begin);
        const std::string item = trim(cleaned.substr(
            begin, comma == std::string::npos ? std::string::npos : comma - begin));
        if (item.empty()) {
            return makeError<std::vector<std::uint32_t>>(
                Status::InvalidArgument, "NUMA list contains an empty item");
        }

        const std::size_t dash = item.find('-');
        std::uint32_t first = 0;
        std::uint32_t last = 0;
        if (dash == std::string::npos) {
            if (!parseUnsigned(item, first)) {
                return makeError<std::vector<std::uint32_t>>(
                    Status::InvalidArgument, "NUMA list contains a non-numeric item");
            }
            last = first;
        } else {
            if (item.find('-', dash + 1) != std::string::npos ||
                !parseUnsigned(trim(item.substr(0, dash)), first) ||
                !parseUnsigned(trim(item.substr(dash + 1)), last) || first > last) {
                return makeError<std::vector<std::uint32_t>>(
                    Status::InvalidArgument, "NUMA list contains an invalid range");
            }
        }

        const std::uint64_t width = static_cast<std::uint64_t>(last) - first + 1;
        if (width > kMaxExpandedIds || result.size() > kMaxExpandedIds - width) {
            return makeError<std::vector<std::uint32_t>>(
                Status::InvalidArgument, "NUMA list expands beyond the supported limit");
        }
        for (std::uint32_t value = first;; ++value) {
            if (std::find(result.begin(), result.end(), value) != result.end()) {
                return makeError<std::vector<std::uint32_t>>(
                    Status::InvalidArgument, "NUMA list contains a duplicate id");
            }
            result.push_back(value);
            if (value == last) {
                break;
            }
        }

        if (comma == std::string::npos) {
            break;
        }
        begin = comma + 1;
    }
    return Result<std::vector<std::uint32_t>>(std::move(result));
}

std::string NumaTopologySnapshot::summary() const {
    std::ostringstream output;
    output << "available=" << (available ? "true" : "false") << " node_ids=";
    if (onlineNodes.empty()) {
        output << "none";
    } else {
        for (std::size_t index = 0; index < onlineNodes.size(); ++index) {
            if (index != 0) {
                output << ',';
            }
            output << onlineNodes[index];
        }
    }
    output << " cpu_count=" << onlineCpus.size() << " cpu_nodes=";
    if (cpuPlacements.empty()) {
        output << "none";
    } else {
        for (std::size_t index = 0; index < cpuPlacements.size(); ++index) {
            if (index != 0) {
                output << ',';
            }
            output << cpuPlacements[index].cpu << ':' << cpuPlacements[index].node;
        }
    }
    output << " devices=";
    if (devices.empty()) {
        output << "none";
        return output.str();
    }
    for (std::size_t index = 0; index < devices.size(); ++index) {
        if (index != 0) {
            output << ',';
        }
        output << devices[index].id << '@' << devices[index].pciBdf << "->"
               << devices[index].node;
    }
    return output.str();
}

NumaTopologySnapshot snapshotNumaTopology(const Config &config,
                                          const std::string &sysfsRoot) {
    NumaTopologySnapshot snapshot;
    std::string nodeList;
    std::string cpuList;
    const bool hasNodes = readText(joinPath(sysfsRoot, "devices/system/node/online"), nodeList);
    const bool hasCpus = readText(joinPath(sysfsRoot, "devices/system/cpu/online"), cpuList);
    if (hasNodes) {
        auto parsed = parseNumaIdList(nodeList);
        if (parsed.ok()) {
            snapshot.onlineNodes = std::move(parsed).value();
            snapshot.available = true;
        }
    }
    if (hasCpus) {
        auto parsed = parseNumaIdList(cpuList);
        if (parsed.ok()) {
            snapshot.onlineCpus = std::move(parsed).value();
            snapshot.cpuPlacements.reserve(snapshot.onlineCpus.size());
            for (const std::uint32_t cpu : snapshot.onlineCpus) {
                NumaCpuPlacement placement;
                placement.cpu = cpu;
                placement.node = cpuNode(sysfsRoot, cpu, snapshot.onlineNodes);
                snapshot.cpuPlacements.push_back(placement);
            }
        }
    }

    snapshot.devices.reserve(config.devices.size());
    for (const auto &device : config.devices) {
        NumaDevicePlacement placement;
        placement.id = device.id;
        placement.pciBdf = device.pciBdf;
        readSigned(joinPath(sysfsRoot, "bus/pci/devices/" + device.pciBdf + "/numa_node"),
                   placement.node);
        snapshot.devices.push_back(std::move(placement));
    }
    return snapshot;
}

} // namespace hfs
