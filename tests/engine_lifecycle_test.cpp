#include <cstdio>
#include <memory>
#include <string>
#include <unistd.h>

#include "hfs/config.hpp"
#include "hfs/core.hpp"
#include "test_support.hpp"

int main() {
    const std::string lockPath = "/tmp/hfs2-lifecycle-" + std::to_string(::getpid()) + ".lock";

    auto parsed = hfs::parseConfig(validConfigText(lockPath), "memory");
    HFS_TEST_CHECK(parsed.ok());

    /* create rejects invalid configs */
    {
        hfs::Config broken = parsed.value();
        broken.instanceName.clear();
        auto engine = hfs::Engine::create(broken);
        HFS_TEST_CHECK(!engine.ok());
        HFS_TEST_CHECK(engine.status() == hfs::Status::InvalidArgument);
    }

    auto created = hfs::Engine::create(parsed.value());
    HFS_TEST_CHECK(created.ok());
    auto engine = std::move(created).value();
    HFS_TEST_CHECK(engine != nullptr);
    HFS_TEST_CHECK(!engine->mounted());

    /* one engine per instance: the second create fails while the lock is held */
    {
        auto second = hfs::Engine::create(parsed.value());
        HFS_TEST_CHECK(!second.ok());
        HFS_TEST_CHECK(second.status() == hfs::Status::Busy);
    }

    /* client attach requires a mounted engine */
    {
        auto client = hfs::Client::create(*engine);
        HFS_TEST_CHECK(!client.ok());
        HFS_TEST_CHECK(client.status() == hfs::Status::InvalidState);
    }

    /* format the volume so mount can validate the superblocks */
    {
        auto formatted = engine->format({true}).wait();
        HFS_TEST_CHECK(formatted.ok());
        HFS_TEST_CHECK(formatted.value().ok());
        HFS_TEST_CHECK(engine->formatted());
    }

    auto mount = engine->mount().wait();
    HFS_TEST_CHECK(mount.ok());        /* the task completed */
    HFS_TEST_CHECK(mount.value().ok()); /* the mount succeeded */
    HFS_TEST_CHECK(engine->mounted());

    /* double mount is invalid */
    {
        auto again = engine->mount().wait();
        HFS_TEST_CHECK(again.ok());
        HFS_TEST_CHECK(again.value().status() == hfs::Status::InvalidState);
    }

    {
        auto client = hfs::Client::create(*engine);
        HFS_TEST_CHECK(client.ok());
        auto close = client.value()->close().wait();
        HFS_TEST_CHECK(close.ok());
        HFS_TEST_CHECK(close.value().ok());
    }

    HFS_TEST_CHECK(engine->drain().wait().value().ok());
    HFS_TEST_CHECK(engine->stop().wait().value().ok());
    HFS_TEST_CHECK(engine->stop().wait().value().ok()); /* stop is idempotent */
    HFS_TEST_CHECK(!engine->mounted());

    /* lock release lets a new engine take over */
    {
        auto third = hfs::Engine::create(parsed.value());
        HFS_TEST_CHECK(third.ok());
        HFS_TEST_CHECK(third.value()->mount().wait().value().ok());
        HFS_TEST_CHECK(third.value()->stop().wait().value().ok());
    }

    engine.reset();
    ::unlink(lockPath.c_str());
    std::printf("engine_lifecycle_test: ok\n");
    return 0;
}
