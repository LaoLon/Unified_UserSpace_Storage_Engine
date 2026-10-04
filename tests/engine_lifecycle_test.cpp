#include <cstdio>
#include <memory>
#include <string>
#include <unistd.h>

#include "uuse/config.hpp"
#include "uuse/core.hpp"
#include "test_support.hpp"

int main() {
    const std::string lockPath = "/tmp/uuse-lifecycle-" + std::to_string(::getpid()) + ".lock";

    auto parsed = uuse::parseConfig(validConfigText(lockPath), "memory");
    UUSE_TEST_CHECK(parsed.ok());

    /* create rejects invalid configs */
    {
        uuse::Config broken = parsed.value();
        broken.instanceName.clear();
        auto engine = uuse::Engine::create(broken);
        UUSE_TEST_CHECK(!engine.ok());
        UUSE_TEST_CHECK(engine.status() == uuse::Status::InvalidArgument);
    }

    auto created = uuse::Engine::create(parsed.value());
    UUSE_TEST_CHECK(created.ok());
    auto engine = std::move(created).value();
    UUSE_TEST_CHECK(engine != nullptr);
    UUSE_TEST_CHECK(!engine->mounted());

    /* one engine per instance: the second create fails while the lock is held */
    {
        auto second = uuse::Engine::create(parsed.value());
        UUSE_TEST_CHECK(!second.ok());
        UUSE_TEST_CHECK(second.status() == uuse::Status::Busy);
    }

    /* client attach requires a mounted engine */
    {
        auto client = uuse::Client::create(*engine);
        UUSE_TEST_CHECK(!client.ok());
        UUSE_TEST_CHECK(client.status() == uuse::Status::InvalidState);
    }

    /* format the volume so mount can validate the superblocks */
    {
        auto formatted = engine->format({true}).wait();
        UUSE_TEST_CHECK(formatted.ok());
        UUSE_TEST_CHECK(formatted.value().ok());
        UUSE_TEST_CHECK(engine->formatted());
    }

    auto mount = engine->mount().wait();
    UUSE_TEST_CHECK(mount.ok());        /* the task completed */
    UUSE_TEST_CHECK(mount.value().ok()); /* the mount succeeded */
    UUSE_TEST_CHECK(engine->mounted());

    /* double mount is invalid */
    {
        auto again = engine->mount().wait();
        UUSE_TEST_CHECK(again.ok());
        UUSE_TEST_CHECK(again.value().status() == uuse::Status::InvalidState);
    }

    {
        auto client = uuse::Client::create(*engine);
        UUSE_TEST_CHECK(client.ok());
        auto close = client.value()->close().wait();
        UUSE_TEST_CHECK(close.ok());
        UUSE_TEST_CHECK(close.value().ok());
    }

    UUSE_TEST_CHECK(engine->drain().wait().value().ok());
    UUSE_TEST_CHECK(engine->stop().wait().value().ok());
    UUSE_TEST_CHECK(engine->stop().wait().value().ok()); /* stop is idempotent */
    UUSE_TEST_CHECK(!engine->mounted());

    /* lock release lets a new engine take over */
    {
        auto third = uuse::Engine::create(parsed.value());
        UUSE_TEST_CHECK(third.ok());
        UUSE_TEST_CHECK(third.value()->mount().wait().value().ok());
        UUSE_TEST_CHECK(third.value()->stop().wait().value().ok());
    }

    engine.reset();
    ::unlink(lockPath.c_str());
    std::printf("engine_lifecycle_test: ok\n");
    return 0;
}
