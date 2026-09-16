#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

#include "hfs/export.hpp"
#include "hfs/task.hpp"
#include "hfs/types.hpp"

namespace hfs {

class Client;

/* One buffer vector for read/write (v1 uses a single contiguous segment). */
struct IoVec {
    std::byte *data = nullptr;
    std::size_t size = 0;
};

struct ReadResult {
    std::size_t bytes = 0;
};

struct WriteResult {
    std::size_t bytes = 0;
};

/* Namespace handle: inode id plus generation, so a stale handle never resolves
 * to a reused inode (hfs2.0.md §13.2). */
struct FileHandle {
    std::uint64_t inodeId = 0;
    std::uint32_t generation = 0;
    bool valid() const noexcept { return inodeId != 0 && generation != 0; }
};

/* Open flags; None opens an existing path only. */
enum class OpenFlags : std::uint32_t {
    None = 0,
    Create = 1U << 0,
    Exclusive = 1U << 1,
    Directory = 1U << 2,
    Truncate = 1U << 3,
};

constexpr OpenFlags operator|(OpenFlags left, OpenFlags right) noexcept {
    return static_cast<OpenFlags>(static_cast<std::uint32_t>(left) |
                                  static_cast<std::uint32_t>(right));
}

constexpr bool hasFlag(OpenFlags value, OpenFlags flag) noexcept {
    return (static_cast<std::uint32_t>(value) & static_cast<std::uint32_t>(flag)) != 0;
}

struct FileStat {
    std::uint64_t inodeId = 0;
    std::uint32_t generation = 0;
    bool directory = false;
    std::uint32_t linkCount = 0;
    std::uint32_t openCount = 0;
    std::uint64_t logicalSize = 0;
    std::uint64_t mtimeNs = 0;
    std::uint64_t ctimeNs = 0;
};

struct DirectoryEntry {
    std::string name;
    std::uint64_t inodeId = 0;
    std::uint32_t generation = 0;
    bool directory = false;
};

struct DirectoryPage {
    std::vector<DirectoryEntry> entries;
    std::string nextCursor; /* empty when iteration is complete */
};

/* Long-lived frontend handle. Data operations land with the allocator
 * (Shard-77) and durability (Shard-78). */
class HFS_API File {
public:
    ~File();
    File(const File &) = delete;
    File &operator=(const File &) = delete;

    Task<Result<ReadResult>> read(std::span<IoVec> buffers, std::uint64_t offset,
                                  std::stop_token stop = {});
    Task<Result<WriteResult>> write(std::span<const IoVec> buffers, std::uint64_t offset,
                                    std::stop_token stop = {});
    Task<Result<WriteResult>> append(std::span<const IoVec> buffers,
                                     std::stop_token stop = {});
    Task<Result<void>> allocate(std::uint64_t offset, std::uint64_t length);
    Task<Result<void>> truncate(std::uint64_t size);
    Task<Result<void>> datasync();
    Task<Result<void>> sync();

    Result<FileStat> stat() const;
    FileHandle handle() const noexcept;

private:
    friend class Client;
    struct Impl;
    explicit File(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace hfs
