#include <cstdio>
#include <string>

#include "hfs/task.hpp"
#include "test_support.hpp"

namespace {

hfs::Task<int> answer() { co_return 42; }

hfs::Task<void> nothing() { co_return; }

hfs::Task<hfs::Result<int>> maybe(bool ok) {
    if (ok) {
        co_return hfs::Result<int>(7);
    }
    co_return hfs::Result<int>(hfs::Status::NoSpace, "extent pool full");
}

hfs::Task<int> nested() {
    auto inner = co_await answer();
    if (!inner.ok()) {
        co_return -1;
    }
    co_return inner.value() + 1;
}

hfs::Task<hfs::Result<void>> chain() {
    co_await nothing();
    co_return hfs::Result<void>(hfs::Status::Ok);
}

} // namespace

int main() {
    HFS_TEST_CHECK(std::string(hfs::statusName(hfs::Status::NoSpace)) == "NoSpace");

    {
        auto result = answer().wait();
        HFS_TEST_CHECK(result.ok());
        HFS_TEST_CHECK(result.value() == 42);
    }

    {
        auto result = nothing().wait();
        HFS_TEST_CHECK(result.ok());
    }

    {
        auto result = maybe(true).wait();
        HFS_TEST_CHECK(result.ok());
        HFS_TEST_CHECK(result.value().ok());
        HFS_TEST_CHECK(result.value().value() == 7);

        auto failed = maybe(false).wait();
        HFS_TEST_CHECK(failed.ok()); /* the task succeeded; the payload carries the error */
        HFS_TEST_CHECK(!failed.value().ok());
        HFS_TEST_CHECK(failed.value().status() == hfs::Status::NoSpace);
        HFS_TEST_CHECK(failed.value().message() == "extent pool full");
    }

    {
        auto result = nested().wait();
        HFS_TEST_CHECK(result.ok());
        HFS_TEST_CHECK(result.value() == 43);
    }

    {
        auto result = chain().wait();
        HFS_TEST_CHECK(result.ok());
    }

    /* an unconsumed task frame is destroyed without leaking or crashing */
    { auto dropped = answer(); (void)dropped; }

    std::printf("task_test: ok\n");
    return 0;
}
