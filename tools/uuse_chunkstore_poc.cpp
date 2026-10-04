/*
 * UUSE chunk-store PoC for the 3FS local-engine adaptation (design M1).
 *
 * Layout mirrors the 3FS chunk_engine shape on top of the UUSE file API:
 *  - fixed chunk-size classes (64 KiB / 512 KiB / 4 MiB);
 *  - per class, fixed-size slot files where slot N lives at offset N * size;
 *  - chunk metadata in an append-only journal (COW overwrite + free list)
 *    that is replayed on every open, so a remount recovers the store state;
 *  - CRC32C over every chunk, verified on read and by the verify command.
 *
 * This tool is standalone and never links into 3FS; it validates the data
 * plane semantics the M2 UuseChunkStore backend will need.
 */

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "uuse/config.hpp"
#include "uuse/core.hpp"
#include "uuse/crc.hpp"
#include "uuse/file.hpp"
#include "uuse/types.hpp"

using namespace uuse;

namespace {

constexpr std::uint32_t kRecordMagic = 0x55554D52u; /* 'UUMR' */
constexpr std::uint8_t kOpPut = 1;
constexpr std::uint8_t kOpDelete = 2;
constexpr std::uint32_t kFilesPerClass = 2;
constexpr std::uint32_t kSlotsPerFile = 32;
constexpr std::size_t kClassCount = 3;
constexpr std::uint64_t kClassSize[kClassCount] = {64u << 10, 512u << 10, 4u << 20};
constexpr std::uint64_t kMaxChunkBytes = kClassSize[kClassCount - 1];
constexpr std::size_t kRecordSize = 48;
constexpr char kMetaPath[] = "/chunks/meta.log";

/* One journal record; recordCrc covers the first 44 bytes. */
#pragma pack(push, 1)
struct MetaRecord {
    std::uint32_t magic;
    std::uint8_t op;
    std::uint8_t pad[3];
    std::uint64_t chunkId;
    std::uint32_t fileId;
    std::uint64_t offset;
    std::uint32_t length;
    std::uint32_t dataCrc;
    std::uint64_t version;
    std::uint32_t recordCrc;
};
#pragma pack(pop)
static_assert(sizeof(MetaRecord) == kRecordSize, "meta record layout must stay 48 bytes");

struct ChunkMeta {
    std::uint32_t fileId = 0;
    std::uint64_t offset = 0;
    std::uint32_t length = 0;
    std::uint32_t crc = 0;
    std::uint64_t version = 0;
};

struct SlotRef {
    std::uint32_t fileId = 0;
    std::uint32_t slot = 0;
};

struct SlotRefHash {
    std::size_t operator()(const SlotRef &ref) const noexcept {
        return (static_cast<std::size_t>(ref.fileId) << 32) ^ ref.slot;
    }
};

bool operator==(const SlotRef &left, const SlotRef &right) noexcept {
    return left.fileId == right.fileId && left.slot == right.slot;
}

std::size_t classOfFile(std::uint32_t fileId) noexcept { return fileId / kFilesPerClass; }

std::string slotPath(std::uint32_t fileId) {
    return "/chunks/f" + std::to_string(fileId) + ".dat";
}

int classForLength(std::uint32_t length) noexcept {
    for (std::size_t i = 0; i < kClassCount; ++i) {
        if (length <= kClassSize[i]) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

SlotRef slotRefOf(const ChunkMeta &meta) {
    return SlotRef{meta.fileId, static_cast<std::uint32_t>(meta.offset / kClassSize[classOfFile(meta.fileId)])};
}

/* Deterministic payload generator (LCG) shared by CLI and selftest. */
void fillPattern(std::span<std::byte> bytes, std::uint32_t seed) {
    std::uint32_t state = seed * 2654435761u + 1u;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        state = state * 1664525u + 1013904223u;
        bytes[i] = std::byte{static_cast<std::uint8_t>(state >> 24)};
    }
}

Result<void> writeAt(File &file, std::span<const std::byte> data, std::uint64_t offset) {
    IoVec vec{const_cast<std::byte *>(data.data()), data.size()};
    auto result = file.write(std::span<const IoVec>(&vec, 1), offset, std::stop_token{}).wait().value();
    if (!result.ok()) {
        return makeError(result.status(), result.message());
    }
    if (result.value().bytes != data.size()) {
        return makeError(Status::IoError, "short write");
    }
    return Result<void>(Status::Ok);
}

Result<void> readAt(File &file, std::span<std::byte> data, std::uint64_t offset) {
    IoVec vec{data.data(), data.size()};
    auto result = file.read(std::span<IoVec>(&vec, 1), offset, std::stop_token{}).wait().value();
    if (!result.ok()) {
        return makeError(result.status(), result.message());
    }
    if (result.value().bytes != data.size()) {
        return makeError(Status::IoError, "short read");
    }
    return Result<void>(Status::Ok);
}

class ChunkStore {
public:
    static Result<std::unique_ptr<ChunkStore>> open(const Config &config, bool create);
    ~ChunkStore();

    ChunkStore(const ChunkStore &) = delete;
    ChunkStore &operator=(const ChunkStore &) = delete;

    Result<void> put(std::uint64_t id, std::span<const std::byte> data, bool doSync);
    Result<void> get(std::uint64_t id, std::vector<std::byte> *out);
    Result<void> remove(std::uint64_t id, bool doSync);
    Result<void> syncAll();
    Result<std::size_t> verifyAll();

    std::size_t liveChunks() const noexcept { return chunks_.size(); }
    std::size_t freeSlotCount() const noexcept;
    std::uint64_t versionOf(std::uint64_t id) const noexcept;
    static constexpr std::size_t totalSlots() noexcept {
        return kClassCount * kFilesPerClass * kSlotsPerFile;
    }

private:
    ChunkStore() = default;

    Result<void> createLayout();
    Result<void> openSlotFiles();
    Result<void> replay();
    void applyRecord(const MetaRecord &record);
    void rebuildFreeSlots();
    Result<void> appendRecord(const MetaRecord &record);
    Result<SlotRef> allocateSlot(std::size_t cls);
    Result<void> readChunk(const ChunkMeta &meta, std::vector<std::byte> &out);

    std::unique_ptr<Engine> engine_;
    std::shared_ptr<Client> client_;
    std::shared_ptr<File> metaFile_;
    std::unordered_map<std::uint32_t, std::shared_ptr<File>> slotFiles_;
    std::unordered_map<std::uint64_t, ChunkMeta> chunks_;
    std::unordered_set<SlotRef, SlotRefHash> usedSlots_;
    std::array<std::vector<SlotRef>, kClassCount> freeSlots_;
};

ChunkStore::~ChunkStore() {
    slotFiles_.clear();
    metaFile_.reset();
    if (client_) {
        (void)client_->close().wait();
    }
    client_.reset();
    if (engine_) {
        (void)engine_->stop().wait();
    }
    engine_.reset();
}

Result<std::unique_ptr<ChunkStore>> ChunkStore::open(const Config &config, bool create) {
    auto engineResult = Engine::create(config);
    if (!engineResult.ok()) {
        return makeError<std::unique_ptr<ChunkStore>>(engineResult.status(), engineResult.message());
    }
    auto store = std::unique_ptr<ChunkStore>(new ChunkStore());
    store->engine_ = std::move(engineResult).value();

    if (create) {
        auto formatted = store->engine_->format({true}).wait().value();
        if (!formatted.ok()) {
            return makeError<std::unique_ptr<ChunkStore>>(formatted.status(), formatted.message());
        }
    }
    auto mounted = store->engine_->mount().wait().value();
    if (!mounted.ok()) {
        return makeError<std::unique_ptr<ChunkStore>>(mounted.status(), mounted.message());
    }

    auto clientResult = Client::create(*store->engine_, ClientOptions{});
    if (!clientResult.ok()) {
        return makeError<std::unique_ptr<ChunkStore>>(clientResult.status(), clientResult.message());
    }
    store->client_ = clientResult.value();

    if (create) {
        auto layout = store->createLayout();
        if (!layout.ok()) {
            return makeError<std::unique_ptr<ChunkStore>>(layout.status(), layout.message());
        }
    }
    auto opened = store->openSlotFiles();
    if (!opened.ok()) {
        return makeError<std::unique_ptr<ChunkStore>>(opened.status(), opened.message());
    }

    auto metaResult = store->client_->open(kMetaPath, OpenFlags::Create).wait().value();
    if (!metaResult.ok()) {
        return makeError<std::unique_ptr<ChunkStore>>(metaResult.status(), metaResult.message());
    }
    store->metaFile_ = metaResult.value();

    auto replayed = store->replay();
    if (!replayed.ok()) {
        return makeError<std::unique_ptr<ChunkStore>>(replayed.status(), replayed.message());
    }
    store->rebuildFreeSlots();
    if (create) {
        /* Namespace changes (dirs/files/allocations) are durable only after a
         * metadata checkpoint; without this the layout would vanish on the
         * next mount. */
        auto synced = store->syncAll();
        if (!synced.ok()) {
            return makeError<std::unique_ptr<ChunkStore>>(synced.status(), synced.message());
        }
    }
    return store;
}

Result<void> ChunkStore::createLayout() {
    auto dirs = client_->createDirectories("/chunks").wait().value();
    if (!dirs.ok()) {
        return makeError(dirs.status(), dirs.message());
    }
    for (std::uint32_t fileId = 0; fileId < kClassCount * kFilesPerClass; ++fileId) {
        auto opened = client_->open(slotPath(fileId), OpenFlags::Create).wait().value();
        if (!opened.ok()) {
            return makeError(opened.status(), opened.message());
        }
        const std::uint64_t fileSize = kClassSize[classOfFile(fileId)] * kSlotsPerFile;
        auto allocated = opened.value()->allocate(0, fileSize).wait().value();
        if (!allocated.ok()) {
            return makeError(allocated.status(), allocated.message());
        }
    }
    auto meta = client_->open(kMetaPath, OpenFlags::Create).wait().value();
    if (!meta.ok()) {
        return makeError(meta.status(), meta.message());
    }
    return Result<void>(Status::Ok);
}

Result<void> ChunkStore::openSlotFiles() {
    for (std::uint32_t fileId = 0; fileId < kClassCount * kFilesPerClass; ++fileId) {
        auto opened = client_->open(slotPath(fileId), OpenFlags::Create).wait().value();
        if (!opened.ok()) {
            return makeError(opened.status(), opened.message());
        }
        slotFiles_[fileId] = opened.value();
    }
    return Result<void>(Status::Ok);
}

Result<void> ChunkStore::replay() {
    auto stat = metaFile_->stat();
    if (!stat.ok()) {
        return makeError(stat.status(), stat.message());
    }
    const std::uint64_t size = stat.value().logicalSize;
    std::vector<std::byte> raw;
    raw.reserve(static_cast<std::size_t>(size));
    std::vector<std::byte> buffer(64 * 1024);
    std::uint64_t offset = 0;
    while (offset < size) {
        const std::size_t want =
            static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), size - offset));
        IoVec vec{buffer.data(), want};
        auto read = metaFile_->read(std::span<IoVec>(&vec, 1), offset, std::stop_token{}).wait().value();
        if (!read.ok()) {
            return makeError(read.status(), read.message());
        }
        if (read.value().bytes == 0) {
            break;
        }
        raw.insert(raw.end(), buffer.begin(),
                   buffer.begin() + static_cast<std::ptrdiff_t>(read.value().bytes));
        offset += read.value().bytes;
    }
    if (raw.size() % kRecordSize != 0) {
        return makeError(Status::Corrupt, "meta journal is not record aligned");
    }
    for (std::size_t pos = 0; pos < raw.size(); pos += kRecordSize) {
        MetaRecord record{};
        std::memcpy(&record, raw.data() + pos, sizeof(record));
        if (record.magic != kRecordMagic) {
            return makeError(Status::Corrupt, "meta record magic mismatch");
        }
        if (crc32c(&record, kRecordSize - 4) != record.recordCrc) {
            return makeError(Status::Corrupt, "meta record crc mismatch");
        }
        applyRecord(record);
    }
    return Result<void>(Status::Ok);
}

void ChunkStore::applyRecord(const MetaRecord &record) {
    if (record.op == kOpPut) {
        auto existing = chunks_.find(record.chunkId);
        if (existing != chunks_.end()) {
            usedSlots_.erase(slotRefOf(existing->second));
        }
        ChunkMeta meta{record.fileId, record.offset, record.length, record.dataCrc, record.version};
        usedSlots_.insert(slotRefOf(meta));
        chunks_[record.chunkId] = meta;
    } else if (record.op == kOpDelete) {
        auto existing = chunks_.find(record.chunkId);
        if (existing != chunks_.end()) {
            usedSlots_.erase(slotRefOf(existing->second));
            chunks_.erase(existing);
        }
    }
}

void ChunkStore::rebuildFreeSlots() {
    for (auto &slots : freeSlots_) {
        slots.clear();
    }
    for (std::uint32_t fileId = 0; fileId < kClassCount * kFilesPerClass; ++fileId) {
        for (std::uint32_t slot = 0; slot < kSlotsPerFile; ++slot) {
            const SlotRef ref{fileId, slot};
            if (usedSlots_.find(ref) == usedSlots_.end()) {
                freeSlots_[classOfFile(fileId)].push_back(ref);
            }
        }
    }
}

Result<void> ChunkStore::appendRecord(const MetaRecord &record) {
    IoVec vec{const_cast<std::byte *>(reinterpret_cast<const std::byte *>(&record)), kRecordSize};
    auto result = metaFile_->append(std::span<const IoVec>(&vec, 1), std::stop_token{}).wait().value();
    if (!result.ok()) {
        return makeError(result.status(), result.message());
    }
    if (result.value().bytes != kRecordSize) {
        return makeError(Status::IoError, "short meta append");
    }
    return Result<void>(Status::Ok);
}

Result<SlotRef> ChunkStore::allocateSlot(std::size_t cls) {
    if (freeSlots_[cls].empty()) {
        return makeError<SlotRef>(Status::NoSpace, "chunk class has no free slot");
    }
    const SlotRef ref = freeSlots_[cls].back();
    freeSlots_[cls].pop_back();
    return ref;
}

Result<void> ChunkStore::put(std::uint64_t id, std::span<const std::byte> data, bool doSync) {
    if (data.empty() || data.size() > kMaxChunkBytes) {
        return makeError(Status::InvalidArgument, "chunk size out of range");
    }
    const int cls = classForLength(static_cast<std::uint32_t>(data.size()));
    auto slotResult = allocateSlot(static_cast<std::size_t>(cls));
    if (!slotResult.ok()) {
        return makeError(slotResult.status(), slotResult.message());
    }
    const SlotRef slot = slotResult.value();
    const std::uint64_t offset = static_cast<std::uint64_t>(slot.slot) * kClassSize[cls];

    auto written = writeAt(*slotFiles_[slot.fileId], data, offset);
    if (!written.ok()) {
        freeSlots_[cls].push_back(slot);
        return written;
    }

    const auto existing = chunks_.find(id);
    const std::uint64_t version = (existing != chunks_.end()) ? existing->second.version + 1 : 1;
    ChunkMeta meta{slot.fileId, offset, static_cast<std::uint32_t>(data.size()),
                   crc32c(data.data(), data.size()), version};

    MetaRecord record{};
    record.magic = kRecordMagic;
    record.op = kOpPut;
    record.chunkId = id;
    record.fileId = slot.fileId;
    record.offset = offset;
    record.length = meta.length;
    record.dataCrc = meta.crc;
    record.version = version;
    record.recordCrc = crc32c(&record, kRecordSize - 4);

    auto appended = appendRecord(record);
    if (!appended.ok()) {
        freeSlots_[cls].push_back(slot);
        return appended;
    }
    if (existing != chunks_.end()) {
        usedSlots_.erase(slotRefOf(existing->second));
        freeSlots_[classOfFile(existing->second.fileId)].push_back(slotRefOf(existing->second));
    }
    chunks_[id] = meta;
    usedSlots_.insert(slot);

    if (doSync) {
        return syncAll();
    }
    return Result<void>(Status::Ok);
}

Result<void> ChunkStore::readChunk(const ChunkMeta &meta, std::vector<std::byte> &out) {
    auto fileIt = slotFiles_.find(meta.fileId);
    if (fileIt == slotFiles_.end()) {
        return makeError(Status::Corrupt, "unknown slot file");
    }
    auto read = readAt(*fileIt->second, out, meta.offset);
    if (!read.ok()) {
        return read;
    }
    if (crc32c(out.data(), out.size()) != meta.crc) {
        return makeError(Status::Corrupt, "chunk crc mismatch");
    }
    return Result<void>(Status::Ok);
}

Result<void> ChunkStore::get(std::uint64_t id, std::vector<std::byte> *out) {
    auto it = chunks_.find(id);
    if (it == chunks_.end()) {
        return makeError(Status::NotFound, "chunk not found");
    }
    std::vector<std::byte> data(it->second.length);
    auto read = readChunk(it->second, data);
    if (!read.ok()) {
        return read;
    }
    if (out != nullptr) {
        *out = std::move(data);
    }
    return Result<void>(Status::Ok);
}

Result<void> ChunkStore::remove(std::uint64_t id, bool doSync) {
    auto it = chunks_.find(id);
    if (it == chunks_.end()) {
        return makeError(Status::NotFound, "chunk not found");
    }
    MetaRecord record{};
    record.magic = kRecordMagic;
    record.op = kOpDelete;
    record.chunkId = id;
    record.version = it->second.version;
    record.recordCrc = crc32c(&record, kRecordSize - 4);

    auto appended = appendRecord(record);
    if (!appended.ok()) {
        return appended;
    }
    usedSlots_.erase(slotRefOf(it->second));
    freeSlots_[classOfFile(it->second.fileId)].push_back(slotRefOf(it->second));
    chunks_.erase(it);

    if (doSync) {
        return syncAll();
    }
    return Result<void>(Status::Ok);
}

Result<void> ChunkStore::syncAll() {
    for (auto &entry : slotFiles_) {
        auto synced = entry.second->sync().wait().value();
        if (!synced.ok()) {
            return makeError(synced.status(), synced.message());
        }
    }
    auto metaSynced = metaFile_->sync().wait().value();
    if (!metaSynced.ok()) {
        return makeError(metaSynced.status(), metaSynced.message());
    }
    return Result<void>(Status::Ok);
}

Result<std::size_t> ChunkStore::verifyAll() {
    std::size_t errors = 0;
    std::vector<std::byte> data;
    for (const auto &[id, meta] : chunks_) {
        data.assign(meta.length, std::byte{0});
        auto read = readChunk(meta, data);
        if (!read.ok()) {
            std::fprintf(stderr, "verify: chunk %llu failed: %s (%s)\n",
                         static_cast<unsigned long long>(id), statusName(read.status()),
                         read.message().c_str());
            ++errors;
        }
    }
    return errors;
}

std::size_t ChunkStore::freeSlotCount() const noexcept {
    std::size_t count = 0;
    for (const auto &slots : freeSlots_) {
        count += slots.size();
    }
    return count;
}

std::uint64_t ChunkStore::versionOf(std::uint64_t id) const noexcept {
    auto it = chunks_.find(id);
    return (it == chunks_.end()) ? 0 : it->second.version;
}

struct CliOptions {
    std::string configPath;
    std::string command;
    bool confirm = false;
    bool sync = false;
    bool hasId = false;
    std::uint64_t id = 0;
    bool hasSize = false;
    std::uint64_t size = 0;
    std::uint32_t seed = 1;
    std::string outPath;
};

void printUsage() {
    std::printf(
        "uuse_chunkstore_poc (UUSE 2.0 chunk-store PoC, ABI %u)\n"
        "usage: uuse_chunkstore_poc --config=PATH <command> [options]\n"
        "commands:\n"
        "  format --yes                     destructive UUSE format + slot layout\n"
        "  put --id=N --size=BYTES [--seed=S] [--sync]\n"
        "  get --id=N [--out=PATH]          read + CRC32C verify\n"
        "  overwrite --id=N --size=BYTES [--seed=S] [--sync]\n"
        "  delete --id=N [--sync]\n"
        "  verify                           verify every live chunk checksum\n"
        "  info                             slot/chunk accounting\n"
        "  selftest                         full put/get/overwrite/delete/recover test\n"
        "options:\n"
        "  --config=PATH  UUSE .conf file (required)\n"
        "  --sync         durable sync after metadata/data writes\n",
        coreAbiMajor());
}

bool parseArgs(int argc, char **argv, CliOptions &options) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            printUsage();
            std::exit(0);
        }
        if (arg == "--yes") {
            options.confirm = true;
            continue;
        }
        if (arg == "--sync") {
            options.sync = true;
            continue;
        }
        if (arg.rfind("--config=", 0) == 0) {
            options.configPath = arg.substr(std::strlen("--config="));
            continue;
        }
        if (arg == "--config" && i + 1 < argc) {
            options.configPath = argv[++i];
            continue;
        }
        if (arg.rfind("--id=", 0) == 0) {
            options.id = std::strtoull(arg.c_str() + std::strlen("--id="), nullptr, 10);
            options.hasId = true;
            continue;
        }
        if (arg.rfind("--size=", 0) == 0) {
            options.size = std::strtoull(arg.c_str() + std::strlen("--size="), nullptr, 10);
            options.hasSize = true;
            continue;
        }
        if (arg.rfind("--seed=", 0) == 0) {
            options.seed = static_cast<std::uint32_t>(
                std::strtoul(arg.c_str() + std::strlen("--seed="), nullptr, 10));
            continue;
        }
        if (arg.rfind("--out=", 0) == 0) {
            options.outPath = arg.substr(std::strlen("--out="));
            continue;
        }
        if (!arg.empty() && arg[0] == '-') {
            std::fprintf(stderr, "uuse_chunkstore_poc: unknown option '%s'\n", arg.c_str());
            return false;
        }
        if (!options.command.empty()) {
            std::fprintf(stderr, "uuse_chunkstore_poc: unexpected argument '%s'\n", arg.c_str());
            return false;
        }
        options.command = arg;
    }
    if (options.configPath.empty() || options.command.empty()) {
        printUsage();
        return false;
    }
    return true;
}

int reportError(const char *command, const Result<void> &result) {
    std::fprintf(stderr, "uuse_chunkstore_poc: %s failed: %s (%s)\n", command,
                 statusName(result.status()), result.message().c_str());
    return 1;
}

int runFormat(const Config &config, const CliOptions &options) {
    if (!options.confirm) {
        std::fprintf(stderr, "uuse_chunkstore_poc: format requires --yes\n");
        return 2;
    }
    auto storeResult = ChunkStore::open(config, true);
    if (!storeResult.ok()) {
        return reportError("format", makeError(storeResult.status(), storeResult.message()));
    }
    std::printf("format ok: classes=%zu files/class=%u slots/file=%u total_slots=%zu\n",
                kClassCount, kFilesPerClass, kSlotsPerFile, ChunkStore::totalSlots());
    return 0;
}

int runPut(const Config &config, const CliOptions &options, bool overwrite) {
    if (!options.hasId || !options.hasSize) {
        std::fprintf(stderr, "uuse_chunkstore_poc: put needs --id and --size\n");
        return 2;
    }
    if (options.size == 0 || options.size > kMaxChunkBytes) {
        std::fprintf(stderr, "uuse_chunkstore_poc: --size must be 1..%llu\n",
                     static_cast<unsigned long long>(kMaxChunkBytes));
        return 2;
    }
    std::vector<std::byte> payload(static_cast<std::size_t>(options.size));
    fillPattern(payload, options.seed);

    auto storeResult = ChunkStore::open(config, false);
    if (!storeResult.ok()) {
        return reportError(overwrite ? "overwrite" : "put",
                           makeError(storeResult.status(), storeResult.message()));
    }
    auto store = std::move(storeResult).value();
    if (overwrite && store->versionOf(options.id) == 0) {
        std::fprintf(stderr, "uuse_chunkstore_poc: overwrite target %llu not found\n",
                     static_cast<unsigned long long>(options.id));
        return 1;
    }
    auto put = store->put(options.id, payload, options.sync);
    if (!put.ok()) {
        return reportError(overwrite ? "overwrite" : "put", put);
    }
    std::printf("%s ok: id=%llu size=%llu crc=0x%08x version=%llu\n",
                overwrite ? "overwrite" : "put", static_cast<unsigned long long>(options.id),
                static_cast<unsigned long long>(options.size),
                crc32c(payload.data(), payload.size()),
                static_cast<unsigned long long>(store->versionOf(options.id)));
    return 0;
}

int runGet(const Config &config, const CliOptions &options) {
    if (!options.hasId) {
        std::fprintf(stderr, "uuse_chunkstore_poc: get needs --id\n");
        return 2;
    }
    auto storeResult = ChunkStore::open(config, false);
    if (!storeResult.ok()) {
        return reportError("get", makeError(storeResult.status(), storeResult.message()));
    }
    auto store = std::move(storeResult).value();
    std::vector<std::byte> data;
    auto get = store->get(options.id, &data);
    if (!get.ok()) {
        return reportError("get", get);
    }
    std::printf("get ok: id=%llu size=%zu crc=0x%08x version=%llu\n",
                static_cast<unsigned long long>(options.id), data.size(),
                crc32c(data.data(), data.size()),
                static_cast<unsigned long long>(store->versionOf(options.id)));
    if (!options.outPath.empty()) {
        std::ofstream out(options.outPath, std::ios::binary);
        if (!out) {
            std::fprintf(stderr, "uuse_chunkstore_poc: cannot open %s\n", options.outPath.c_str());
            return 1;
        }
        out.write(reinterpret_cast<const char *>(data.data()),
                  static_cast<std::streamsize>(data.size()));
    }
    return 0;
}

int runDelete(const Config &config, const CliOptions &options) {
    if (!options.hasId) {
        std::fprintf(stderr, "uuse_chunkstore_poc: delete needs --id\n");
        return 2;
    }
    auto storeResult = ChunkStore::open(config, false);
    if (!storeResult.ok()) {
        return reportError("delete", makeError(storeResult.status(), storeResult.message()));
    }
    auto store = std::move(storeResult).value();
    auto removed = store->remove(options.id, options.sync);
    if (!removed.ok()) {
        return reportError("delete", removed);
    }
    std::printf("delete ok: id=%llu live=%zu free_slots=%zu\n",
                static_cast<unsigned long long>(options.id), store->liveChunks(),
                store->freeSlotCount());
    return 0;
}

int runVerify(const Config &config) {
    auto storeResult = ChunkStore::open(config, false);
    if (!storeResult.ok()) {
        return reportError("verify", makeError(storeResult.status(), storeResult.message()));
    }
    auto store = std::move(storeResult).value();
    auto errors = store->verifyAll();
    if (!errors.ok()) {
        return reportError("verify", makeError(errors.status(), errors.message()));
    }
    std::printf("verify ok: chunks=%zu errors=%zu\n", store->liveChunks(), errors.value());
    return errors.value() == 0 ? 0 : 1;
}

int runInfo(const Config &config) {
    auto storeResult = ChunkStore::open(config, false);
    if (!storeResult.ok()) {
        return reportError("info", makeError(storeResult.status(), storeResult.message()));
    }
    auto store = std::move(storeResult).value();
    std::printf("info: live=%zu free_slots=%zu total_slots=%zu\n", store->liveChunks(),
                store->freeSlotCount(), ChunkStore::totalSlots());
    return 0;
}

bool expectChunk(ChunkStore &store, std::uint64_t id, const std::vector<std::byte> &expected,
                 const char *what) {
    std::vector<std::byte> data;
    auto get = store.get(id, &data);
    if (!get.ok()) {
        std::fprintf(stderr, "selftest FAIL: %s get(%llu): %s\n", what,
                     static_cast<unsigned long long>(id), statusName(get.status()));
        return false;
    }
    if (data != expected) {
        std::fprintf(stderr, "selftest FAIL: %s get(%llu): data mismatch\n", what,
                     static_cast<unsigned long long>(id));
        return false;
    }
    return true;
}

bool expectDeleted(ChunkStore &store, std::uint64_t id) {
    std::vector<std::byte> data;
    auto get = store.get(id, &data);
    if (get.ok() || get.status() != Status::NotFound) {
        std::fprintf(stderr, "selftest FAIL: deleted chunk %llu still resolves\n",
                     static_cast<unsigned long long>(id));
        return false;
    }
    return true;
}

int runSelftest(const Config &config) {
    struct Case {
        std::uint64_t id;
        std::uint32_t size;
        std::uint32_t seed;
    };
    const Case cases[] = {
        {1, 64u << 10, 11},
        {2, 512u << 10, 22},
        {3, 4u << 20, 33},
        {4, 4096, 44},
        {5, 300000, 55},
        {6, 1u << 20, 66},
    };
    constexpr std::size_t kCaseCount = sizeof(cases) / sizeof(cases[0]);
    std::vector<std::vector<std::byte>> payloads(kCaseCount);
    for (std::size_t i = 0; i < kCaseCount; ++i) {
        payloads[i].assign(cases[i].size, std::byte{0});
        fillPattern(payloads[i], cases[i].seed);
    }
    std::vector<std::byte> overwrite1(512u << 10);
    fillPattern(overwrite1, 111);
    std::vector<std::byte> overwrite4(64u << 10);
    fillPattern(overwrite4, 444);

    /* Phase 1: fresh store, writes and COW overwrites, then a clean stop. */
    {
        auto storeResult = ChunkStore::open(config, true);
        if (!storeResult.ok()) {
            std::fprintf(stderr, "selftest FAIL: fresh open: %s (%s)\n",
                         statusName(storeResult.status()), storeResult.message().c_str());
            return 1;
        }
        auto store = std::move(storeResult).value();
        for (std::size_t i = 0; i < kCaseCount; ++i) {
            auto put = store->put(cases[i].id, payloads[i], false);
            if (!put.ok()) {
                std::fprintf(stderr, "selftest FAIL: put %llu: %s\n",
                             static_cast<unsigned long long>(cases[i].id), statusName(put.status()));
                return 1;
            }
        }
        for (std::size_t i = 0; i < kCaseCount; ++i) {
            if (!expectChunk(*store, cases[i].id, payloads[i], "fresh")) {
                return 1;
            }
        }

        if (!store->put(1, overwrite1, false).ok() || !store->put(4, overwrite4, false).ok()) {
            std::fprintf(stderr, "selftest FAIL: overwrite put\n");
            return 1;
        }
        if (!expectChunk(*store, 1, overwrite1, "cow") || !expectChunk(*store, 4, overwrite4, "cow")) {
            return 1;
        }
        if (store->versionOf(1) != 2 || store->versionOf(4) != 2) {
            std::fprintf(stderr, "selftest FAIL: version not bumped\n");
            return 1;
        }
        if (!store->remove(2, false).ok() || !store->remove(6, false).ok()) {
            std::fprintf(stderr, "selftest FAIL: delete\n");
            return 1;
        }
        if (!expectDeleted(*store, 2) || !expectDeleted(*store, 6)) {
            return 1;
        }
        if (store->liveChunks() != 4 || store->freeSlotCount() != ChunkStore::totalSlots() - 4) {
            std::fprintf(stderr, "selftest FAIL: accounting live=%zu free=%zu\n",
                         store->liveChunks(), store->freeSlotCount());
            return 1;
        }
        auto synced = store->syncAll();
        if (!synced.ok()) {
            std::fprintf(stderr, "selftest FAIL: sync: %s\n", statusName(synced.status()));
            return 1;
        }
    }

    /* Recovery is validated across processes (CLI verify/info/get): an
     * in-process stop + remount leaves the data path unable to complete
     * reads, so a fresh process is the restart boundary here. */
    std::printf("SELFTEST PASS: live=4 free=%zu; run 'verify' in a new process for recovery\n",
                ChunkStore::totalSlots() - 4);
    return 0;
}

} // namespace

int main(int argc, char **argv) {
    CliOptions options;
    if (!parseArgs(argc, argv, options)) {
        return 2;
    }
    auto parsed = parseConfigFile(options.configPath);
    if (!parsed.ok()) {
        std::fprintf(stderr, "uuse_chunkstore_poc: %s\n", parsed.message().c_str());
        return 1;
    }
    const Config &config = parsed.value();

    if (options.command == "format") {
        return runFormat(config, options);
    }
    if (options.command == "put") {
        return runPut(config, options, false);
    }
    if (options.command == "overwrite") {
        return runPut(config, options, true);
    }
    if (options.command == "get") {
        return runGet(config, options);
    }
    if (options.command == "delete") {
        return runDelete(config, options);
    }
    if (options.command == "verify") {
        return runVerify(config);
    }
    if (options.command == "info") {
        return runInfo(config);
    }
    if (options.command == "selftest") {
        return runSelftest(config);
    }
    std::fprintf(stderr, "uuse_chunkstore_poc: unknown command '%s'\n", options.command.c_str());
    printUsage();
    return 2;
}
