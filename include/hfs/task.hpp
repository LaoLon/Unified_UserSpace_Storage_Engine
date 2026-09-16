#pragma once

#include <coroutine>
#include <cstddef>
#include <utility>

#include "hfs/types.hpp"

namespace hfs {

namespace detail {

/* Resumes the awaiting coroutine when the task reaches final suspend. */
struct TaskFinalAwaiter {
    bool await_ready() const noexcept { return false; }

    template <typename Promise>
    std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> handle) noexcept {
        auto continuation = handle.promise().continuation;
        return continuation ? continuation : std::noop_coroutine();
    }

    void await_resume() const noexcept {}
};

} // namespace detail

/* Lazy coroutine result used by the core API. */
template <typename T>
class Task {
public:
    struct promise_type {
        Result<T> result{Status::InvalidState, "task not completed"};
        std::coroutine_handle<> continuation{};

        Task get_return_object() noexcept {
            return Task(std::coroutine_handle<promise_type>::from_promise(*this));
        }

        std::suspend_always initial_suspend() noexcept { return {}; }
        detail::TaskFinalAwaiter final_suspend() noexcept { return {}; }

        void return_value(T value) { result = Result<T>(std::move(value)); }
        void unhandled_exception() noexcept {} /* core is built without exceptions */

        /* Frame allocation hook; Shard-73 replaces this with the bounded pool. */
        static void *operator new(std::size_t size) { return ::operator new(size); }
        static void operator delete(void *ptr) noexcept { ::operator delete(ptr); }
    };

    Task() noexcept = default;
    explicit Task(std::coroutine_handle<promise_type> handle) noexcept : handle_(handle) {}

    Task(Task &&other) noexcept : handle_(std::exchange(other.handle_, {})) {}

    Task &operator=(Task &&other) noexcept {
        if (this != &other) {
            if (handle_) {
                handle_.destroy();
            }
            handle_ = std::exchange(other.handle_, {});
        }
        return *this;
    }

    Task(const Task &) = delete;
    Task &operator=(const Task &) = delete;

    ~Task() {
        if (handle_) {
            handle_.destroy();
        }
    }

    bool valid() const noexcept { return static_cast<bool>(handle_); }
    bool done() const noexcept { return handle_ && handle_.done(); }

    /* Start the lazy frame on the caller thread. */
    void start() {
        if (handle_ && !handle_.done()) {
            handle_.resume();
        }
    }

    /* Drive the task on the caller thread until completion. Phase 0 tasks do
     * not suspend on external events, so a resume loop is sufficient; Phase 2
     * replaces this with the reactor/actor scheduler. */
    Result<T> wait() {
        if (!handle_) {
            return Result<T>(Status::InvalidState, "task has no frame");
        }
        if (!handle_.done()) {
            handle_.resume();
        }
        while (!handle_.done()) {
            handle_.resume();
        }
        return std::move(handle_.promise().result);
    }

    bool await_ready() const noexcept { return false; }

    template <typename P>
    void await_suspend(std::coroutine_handle<P> caller) noexcept {
        handle_.promise().continuation = caller;
        handle_.resume();
    }

    Result<T> await_resume() { return std::move(handle_.promise().result); }

private:
    std::coroutine_handle<promise_type> handle_{};
};

/* void specialization. */
template <>
class Task<void> {
public:
    struct promise_type {
        Result<void> result{Status::InvalidState, "task not completed"};
        std::coroutine_handle<> continuation{};

        Task get_return_object() noexcept {
            return Task(std::coroutine_handle<promise_type>::from_promise(*this));
        }

        std::suspend_always initial_suspend() noexcept { return {}; }
        detail::TaskFinalAwaiter final_suspend() noexcept { return {}; }

        void return_void() noexcept { result = Result<void>(Status::Ok); }
        void unhandled_exception() noexcept {}

        static void *operator new(std::size_t size) { return ::operator new(size); }
        static void operator delete(void *ptr) noexcept { ::operator delete(ptr); }
    };

    Task() noexcept = default;
    explicit Task(std::coroutine_handle<promise_type> handle) noexcept : handle_(handle) {}

    Task(Task &&other) noexcept : handle_(std::exchange(other.handle_, {})) {}

    Task &operator=(Task &&other) noexcept {
        if (this != &other) {
            if (handle_) {
                handle_.destroy();
            }
            handle_ = std::exchange(other.handle_, {});
        }
        return *this;
    }

    Task(const Task &) = delete;
    Task &operator=(const Task &) = delete;

    ~Task() {
        if (handle_) {
            handle_.destroy();
        }
    }

    bool valid() const noexcept { return static_cast<bool>(handle_); }
    bool done() const noexcept { return handle_ && handle_.done(); }

    void start() {
        if (handle_ && !handle_.done()) {
            handle_.resume();
        }
    }

    Result<void> wait() {
        if (!handle_) {
            return Result<void>(Status::InvalidState, "task has no frame");
        }
        if (!handle_.done()) {
            handle_.resume();
        }
        while (!handle_.done()) {
            handle_.resume();
        }
        return std::move(handle_.promise().result);
    }

    bool await_ready() const noexcept { return false; }

    template <typename P>
    void await_suspend(std::coroutine_handle<P> caller) noexcept {
        handle_.promise().continuation = caller;
        handle_.resume();
    }

    Result<void> await_resume() { return std::move(handle_.promise().result); }

private:
    std::coroutine_handle<promise_type> handle_{};
};

} // namespace hfs
