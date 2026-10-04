#pragma once

#include <atomic>
#include <cstdint>

namespace uuse {

/* Aggregates child completions: first error, completed bytes, single wake. */
class ParentCompletion {
public:
    using DoneCallback = void (*)(void *ctx, int status, std::uint64_t completedBytes);

    void init(std::uint32_t childCount, DoneCallback callback, void *ctx) noexcept;

    /* Thread-safe; the last child fires the callback exactly once. */
    void childDone(int status, std::uint32_t bytes) noexcept;

    bool done() const noexcept;
    int firstError() const noexcept;
    std::uint64_t completedBytes() const noexcept;
    std::uint32_t pending() const noexcept;

private:
    std::atomic<std::uint32_t> pending_{0};
    std::atomic<std::uint64_t> completedBytes_{0};
    std::atomic<int> firstError_{0};
    std::atomic<bool> fired_{false};
    DoneCallback callback_ = nullptr;
    void *ctx_ = nullptr;
};

} // namespace uuse
