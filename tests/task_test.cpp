#include <cstdio>
#include <string>

#include "uuse/task.hpp"
#include "test_support.hpp"

namespace {

uuse::Task<int> answer() { co_return 42; }

uuse::Task<void> nothing() { co_return; }

uuse::Task<uuse::Result<int>> maybe(bool ok) {
    if (ok) {
        co_return uuse::Result<int>(7);
    }
    co_return uuse::Result<int>(uuse::Status::NoSpace, "extent pool full");
}

uuse::Task<int> nested() {
    auto inner = co_await answer();
    if (!inner.ok()) {
        co_return -1;
    }
    co_return inner.value() + 1;
}

uuse::Task<uuse::Result<void>> chain() {
    co_await nothing();
    co_return uuse::Result<void>(uuse::Status::Ok);
}

} // namespace

int main() {
    UUSE_TEST_CHECK(std::string(uuse::statusName(uuse::Status::NoSpace)) == "NoSpace");

    {
        auto result = answer().wait();
        UUSE_TEST_CHECK(result.ok());
        UUSE_TEST_CHECK(result.value() == 42);
    }

    {
        auto result = nothing().wait();
        UUSE_TEST_CHECK(result.ok());
    }

    {
        auto result = maybe(true).wait();
        UUSE_TEST_CHECK(result.ok());
        UUSE_TEST_CHECK(result.value().ok());
        UUSE_TEST_CHECK(result.value().value() == 7);

        auto failed = maybe(false).wait();
        UUSE_TEST_CHECK(failed.ok()); /* the task succeeded; the payload carries the error */
        UUSE_TEST_CHECK(!failed.value().ok());
        UUSE_TEST_CHECK(failed.value().status() == uuse::Status::NoSpace);
        UUSE_TEST_CHECK(failed.value().message() == "extent pool full");
    }

    {
        auto result = nested().wait();
        UUSE_TEST_CHECK(result.ok());
        UUSE_TEST_CHECK(result.value() == 43);
    }

    {
        auto result = chain().wait();
        UUSE_TEST_CHECK(result.ok());
    }

    /* an unconsumed task frame is destroyed without leaking or crashing */
    { auto dropped = answer(); (void)dropped; }

    std::printf("task_test: ok\n");
    return 0;
}
