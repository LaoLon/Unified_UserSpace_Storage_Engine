#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

#include "metadata/inode_map.hpp"

namespace hfs::metadata {

/* Reserved root directory inode id; slot 0 stays unused. */
constexpr std::uint64_t kRootInodeId = 1;
constexpr std::size_t kMaxNameBytes = 255;

/* Transparent name hashing so lookups never allocate a temporary string. */
struct StringHash {
    using is_transparent = void;
    std::size_t operator()(std::string_view value) const noexcept {
        return std::hash<std::string_view>{}(value);
    }
    std::size_t operator()(const std::string &value) const noexcept {
        return std::hash<std::string_view>{}(value);
    }
};

/* Per-directory name index: hash lookup plus ordered cursor (hfs2.0.md §13.2). */
struct DirectoryIndex {
    std::unordered_map<std::string, std::uint64_t, StringHash, std::equal_to<>> byName;
    std::map<std::string, std::uint64_t, std::less<>> ordered;
};

/* In-memory inode (hfs2.0.md §13.3). */
struct Inode {
    std::uint64_t id = 0;
    std::uint32_t generation = 1;
    bool directory = false;
    std::uint32_t linkCount = 0; /* visible directory entries */
    std::uint32_t openCount = 0; /* live File handles */
    std::uint64_t logicalSize = 0;
    std::uint64_t mtimeNs = 0;
    std::uint64_t ctimeNs = 0;
    std::uint64_t parent = 0;     /* parent directory (root points to itself) */
    std::uint64_t childCount = 0; /* directories only */
    std::shared_ptr<DirectoryIndex> index;
    InodeExtentMap extents;

    bool orphan() const noexcept { return linkCount == 0 && openCount > 0; }
};

} // namespace hfs::metadata
