#pragma once

#include <cstddef>
#include <cstdint>

#include "hfs/types.hpp"

namespace hfs {

class Operation;

/* Value token validated by the frontend slot table; never a raw pointer. */
struct CompletionToken {
    std::uint32_t slot = 0;
    std::uint32_t generation = 0;

    bool valid() const noexcept { return generation != 0; }
};

/* Value completion record published from a shard to one frontend client. */
struct CompletionEvent {
    std::uint64_t requestId = 0;
    CompletionToken token{};
    Status status = Status::Ok;
    int backendErrno = 0;
    std::uint64_t completedBytes = 0;
};

/* Move-only frontend reference to one accepted internal operation. */
class IoHandle {
public:
    IoHandle() noexcept = default;
    IoHandle(IoHandle &&other) noexcept;
    IoHandle &operator=(IoHandle &&other) noexcept;
    IoHandle(const IoHandle &) = delete;
    IoHandle &operator=(const IoHandle &) = delete;
    ~IoHandle();

    bool valid() const noexcept { return operation_ != nullptr; }
    std::uint64_t requestId() const noexcept { return requestId_; }
    CompletionToken token() const noexcept { return token_; }
    void reset() noexcept;

private:
    friend class Operation;

    /* Adopts the frontend reference acquired by the internal operation. */
    IoHandle(Operation *operation, std::uint64_t requestId,
             CompletionToken token) noexcept
        : operation_(operation), requestId_(requestId), token_(token) {}

    Operation *operation_ = nullptr;
    std::uint64_t requestId_ = 0;
    CompletionToken token_{};
};

/* Result of a nonblocking or timed completion poll. */
struct PollResult {
    Status status = Status::Ok;
    std::size_t count = 0;
};

} // namespace hfs
