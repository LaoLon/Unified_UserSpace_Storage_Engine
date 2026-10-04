#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <unistd.h>
#include <vector>

#include "uuse/config.hpp"
#include "uuse/core.hpp"
#include "uuse/journal.hpp"
#include "test_support.hpp"

namespace {

class CounterTarget final : public uuse::JournalTarget {
public:
    uuse::Result<void> apply(const uuse::JournalRecord &record) override {
        if (record.type == uuse::JournalRecordType::SetSize) {
            values[record.objectId] = record.arg0;
        }
        return ok();
    }

    uuse::Result<void> saveSnapshot(std::uint64_t txid,
                                   std::vector<std::byte> &out) override {
        txid_ = txid;
        out.clear();
        for (const auto &[key, value] : values) {
            appendU64(out, key);
            appendU64(out, value);
        }
        return ok();
    }

    uuse::Result<void> loadSnapshot(std::span<const std::byte> data) override {
        values.clear();
        for (std::size_t i = 0; i + 16 <= data.size(); i += 16) {
            std::uint64_t key = 0;
            std::uint64_t value = 0;
            std::memcpy(&key, data.data() + i, sizeof(key));
            std::memcpy(&value, data.data() + i + 8, sizeof(value));
            values[key] = value;
        }
        return ok();
    }

    std::uint64_t snapshotTxid() const noexcept override { return txid_; }

    std::map<std::uint64_t, std::uint64_t> values;

private:
    static uuse::Result<void> ok() { return uuse::Result<void>(uuse::Status::Ok); }
    static void appendU64(std::vector<std::byte> &out, std::uint64_t value) {
        const auto *raw = reinterpret_cast<const std::byte *>(&value);
        out.insert(out.end(), raw, raw + sizeof(value));
    }
    std::uint64_t txid_ = 0;
};

uuse::JournalRecord setSize(std::uint64_t objectId, std::uint64_t size) {
    uuse::JournalRecord record;
    record.type = uuse::JournalRecordType::SetSize;
    record.objectId = objectId;
    record.arg0 = size;
    return record;
}

} // namespace

int main() {
    const std::string lockPath = "/tmp/uuse-journal-" + std::to_string(::getpid()) + ".lock";
    auto parsed = uuse::parseConfig(validConfigText(lockPath), "memory");
    UUSE_TEST_CHECK(parsed.ok());

    /* Keep the direct Journal recovery scenarios inside one engine mount.
     * CounterTarget snapshots intentionally use a test-only format and must
     * not be replayed by the engine's namespace journal on a later mount. */
    {
        auto created = uuse::Engine::create(parsed.value());
        UUSE_TEST_CHECK(created.ok());
        auto engine = std::move(created).value();
        UUSE_TEST_CHECK(engine->format({true}).wait().value().ok());
        UUSE_TEST_CHECK(engine->mount().wait().value().ok());

        /* session 1: append 4 transactions, checkpoint, append 2 more */
        {
            CounterTarget target;
            auto journal = uuse::Journal::open(*engine, &target);
            UUSE_TEST_CHECK(journal.ok());
            for (std::uint64_t i = 1; i <= 4; ++i) {
                auto appended = journal.value()->append({setSize(7, i * 100)}, true);
                UUSE_TEST_CHECK(appended.ok());
                UUSE_TEST_CHECK(appended.value() == i);
            }
            UUSE_TEST_CHECK(journal.value()->checkpoint().ok());
            for (std::uint64_t i = 5; i <= 6; ++i) {
                UUSE_TEST_CHECK(
                    journal.value()->append({setSize(7, i * 100)}, true).ok());
            }
            UUSE_TEST_CHECK(journal.value()->flush().ok());
        }

        /* session 2: replay from checkpoint, then corrupt the last commit */
        {
            CounterTarget target;
            auto journal = uuse::Journal::open(*engine, &target);
            UUSE_TEST_CHECK(journal.ok());
            UUSE_TEST_CHECK(journal.value()->replay().ok());
            UUSE_TEST_CHECK(target.values[7] == 600);
            const auto &stats = journal.value()->stats();
            UUSE_TEST_CHECK(stats.lastTxid == 6);
            UUSE_TEST_CHECK(stats.lastCheckpointTxid == 4);
            UUSE_TEST_CHECK(stats.corruption.empty());

            /* txn 5 occupies slots 12-14, txn 6 slots 15-17 after checkpoint. */
            const auto geometry = engine->deviceGeometries()[0];
            const std::uint32_t metaBlock = engine->config().metadataBlockSize;
            const std::uint32_t sector = engine->deviceBlockSize(0);
            const std::uint64_t commitSlot = 17;
            const std::uint64_t offsetMeta = geometry.journalStart + commitSlot;
            const std::uint64_t offsetDevice = offsetMeta * (metaBlock / sector);
            std::vector<std::byte> garbage(sector, std::byte{0xAB});
            UUSE_TEST_CHECK(engine->writeDeviceBlocks(0, offsetDevice, garbage).ok());
            UUSE_TEST_CHECK(engine->flushDevice(0).ok());
        }

        /* session 3: torn commit is discarded and corruption is reported */
        {
            CounterTarget target;
            auto journal = uuse::Journal::open(*engine, &target);
            UUSE_TEST_CHECK(journal.ok());
            auto replay = journal.value()->replay();
            UUSE_TEST_CHECK(!replay.ok() && replay.status() == uuse::Status::Corrupt);
            const auto &stats = journal.value()->stats();
            UUSE_TEST_CHECK(stats.lastTxid == 5);
            UUSE_TEST_CHECK(target.values[7] == 500);
            UUSE_TEST_CHECK(!stats.corruption.empty());
        }
        UUSE_TEST_CHECK(engine->stop().wait().value().ok());
    }

    /* restore a clean formatted image for the other tests */
    {
        auto created = uuse::Engine::create(parsed.value());
        UUSE_TEST_CHECK(created.ok());
        auto engine = std::move(created).value();
        UUSE_TEST_CHECK(engine->format({true}).wait().value().ok());
        UUSE_TEST_CHECK(engine->stop().wait().value().ok());
    }

    ::unlink(lockPath.c_str());
    std::printf("journal_device_test: ok\n");
    return 0;
}
