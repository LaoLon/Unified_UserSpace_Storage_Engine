#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <unistd.h>
#include <vector>

#include "hfs/config.hpp"
#include "hfs/core.hpp"
#include "hfs/journal.hpp"
#include "test_support.hpp"

namespace {

class CounterTarget final : public hfs::JournalTarget {
public:
    hfs::Result<void> apply(const hfs::JournalRecord &record) override {
        if (record.type == hfs::JournalRecordType::SetSize) {
            values[record.objectId] = record.arg0;
        }
        return ok();
    }

    hfs::Result<void> saveSnapshot(std::uint64_t txid,
                                   std::vector<std::byte> &out) override {
        txid_ = txid;
        out.clear();
        for (const auto &[key, value] : values) {
            appendU64(out, key);
            appendU64(out, value);
        }
        return ok();
    }

    hfs::Result<void> loadSnapshot(std::span<const std::byte> data) override {
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
    static hfs::Result<void> ok() { return hfs::Result<void>(hfs::Status::Ok); }
    static void appendU64(std::vector<std::byte> &out, std::uint64_t value) {
        const auto *raw = reinterpret_cast<const std::byte *>(&value);
        out.insert(out.end(), raw, raw + sizeof(value));
    }
    std::uint64_t txid_ = 0;
};

hfs::JournalRecord setSize(std::uint64_t objectId, std::uint64_t size) {
    hfs::JournalRecord record;
    record.type = hfs::JournalRecordType::SetSize;
    record.objectId = objectId;
    record.arg0 = size;
    return record;
}

} // namespace

int main() {
    const std::string lockPath = "/tmp/hfs2-journal-" + std::to_string(::getpid()) + ".lock";
    auto parsed = hfs::parseConfig(validConfigText(lockPath), "memory");
    HFS_TEST_CHECK(parsed.ok());

    /* session 1: format, append 4 transactions, checkpoint, append 2 more */
    {
        auto created = hfs::Engine::create(parsed.value());
        HFS_TEST_CHECK(created.ok());
        auto engine = std::move(created).value();
        HFS_TEST_CHECK(engine->format({true}).wait().value().ok());
        HFS_TEST_CHECK(engine->mount().wait().value().ok());

        CounterTarget target;
        auto journal = hfs::Journal::open(*engine, &target);
        HFS_TEST_CHECK(journal.ok());
        for (std::uint64_t i = 1; i <= 4; ++i) {
            auto appended = journal.value()->append({setSize(7, i * 100)}, true);
            HFS_TEST_CHECK(appended.ok());
            HFS_TEST_CHECK(appended.value() == i);
        }
        HFS_TEST_CHECK(journal.value()->checkpoint().ok());
        for (std::uint64_t i = 5; i <= 6; ++i) {
            HFS_TEST_CHECK(journal.value()->append({setSize(7, i * 100)}, true).ok());
        }
        HFS_TEST_CHECK(journal.value()->flush().ok());
        HFS_TEST_CHECK(engine->stop().wait().value().ok());
    }

    /* session 2: replay from checkpoint, then corrupt the last commit slot */
    {
        auto created = hfs::Engine::create(parsed.value());
        HFS_TEST_CHECK(created.ok());
        auto engine = std::move(created).value();
        HFS_TEST_CHECK(engine->mount().wait().value().ok());

        CounterTarget target;
        auto journal = hfs::Journal::open(*engine, &target);
        HFS_TEST_CHECK(journal.ok());
        HFS_TEST_CHECK(journal.value()->replay().ok());
        HFS_TEST_CHECK(target.values[7] == 600);
        const auto &stats = journal.value()->stats();
        HFS_TEST_CHECK(stats.lastTxid == 6);
        HFS_TEST_CHECK(stats.lastCheckpointTxid == 4);
        HFS_TEST_CHECK(stats.corruption.empty());

        /* txn 5 occupies slots 12-14, txn 6 slots 15-17 after the checkpoint. */
        const auto geometry = engine->deviceGeometries()[0];
        const std::uint32_t metaBlock = engine->config().metadataBlockSize;
        const std::uint32_t sector = engine->deviceBlockSize(0);
        const std::uint64_t commitSlot = 17;
        const std::uint64_t offsetMeta = geometry.journalStart + commitSlot;
        const std::uint64_t offsetDevice = offsetMeta * (metaBlock / sector);
        std::vector<std::byte> garbage(sector, std::byte{0xAB});
        HFS_TEST_CHECK(engine->writeDeviceBlocks(0, offsetDevice, garbage).ok());
        HFS_TEST_CHECK(engine->flushDevice(0).ok());
        HFS_TEST_CHECK(engine->stop().wait().value().ok());
    }

    /* session 3: torn commit is discarded, corruption is reported */
    {
        auto created = hfs::Engine::create(parsed.value());
        HFS_TEST_CHECK(created.ok());
        auto engine = std::move(created).value();
        HFS_TEST_CHECK(engine->mount().wait().value().ok());

        CounterTarget target;
        auto journal = hfs::Journal::open(*engine, &target);
        HFS_TEST_CHECK(journal.ok());
        HFS_TEST_CHECK(journal.value()->replay().ok());
        const auto &stats = journal.value()->stats();
        HFS_TEST_CHECK(stats.lastTxid == 5);
        HFS_TEST_CHECK(target.values[7] == 500);
        HFS_TEST_CHECK(!stats.corruption.empty());
        HFS_TEST_CHECK(engine->stop().wait().value().ok());
    }

    /* restore a clean formatted image for the other tests */
    {
        auto created = hfs::Engine::create(parsed.value());
        HFS_TEST_CHECK(created.ok());
        auto engine = std::move(created).value();
        HFS_TEST_CHECK(engine->format({true}).wait().value().ok());
        HFS_TEST_CHECK(engine->stop().wait().value().ok());
    }

    ::unlink(lockPath.c_str());
    std::printf("journal_device_test: ok\n");
    return 0;
}
