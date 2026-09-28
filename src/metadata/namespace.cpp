#include "metadata/namespace.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
#include <unordered_set>

namespace hfs::metadata {

namespace {

constexpr std::uint64_t kNamespaceSnapshotMagic = 0x484653324e535031ULL; /* HFS2NSP1 */
constexpr std::uint32_t kNamespaceSnapshotVersion = 1;

/* Appends fixed-width fields so checkpoint encoding stays architecture-neutral. */
template <typename T>
void appendSnapshotField(std::vector<std::byte> &out, T value) {
    const auto *raw = reinterpret_cast<const std::byte *>(&value);
    out.insert(out.end(), raw, raw + sizeof(T));
}

/* Reads one fixed-width field with a bounded cursor. */
template <typename T>
bool readSnapshotField(std::span<const std::byte> data, std::size_t &cursor, T *value) {
    if (value == nullptr || cursor > data.size() || data.size() - cursor < sizeof(T)) {
        return false;
    }
    std::memcpy(value, data.data() + cursor, sizeof(T));
    cursor += sizeof(T);
    return true;
}

/* Encodes a bounded directory name. */
bool appendSnapshotString(std::vector<std::byte> &out, std::string_view value) {
    if (value.size() > std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }
    appendSnapshotField(out, static_cast<std::uint32_t>(value.size()));
    const auto *raw = reinterpret_cast<const std::byte *>(value.data());
    out.insert(out.end(), raw, raw + value.size());
    return true;
}

/* Decodes a bounded directory name. */
bool readSnapshotString(std::span<const std::byte> data, std::size_t &cursor,
                        std::string *value) {
    std::uint32_t length = 0;
    if (value == nullptr || !readSnapshotField(data, cursor, &length) ||
        length > data.size() - cursor) {
        return false;
    }
    value->assign(reinterpret_cast<const char *>(data.data() + cursor), length);
    cursor += length;
    return true;
}

/* Extent-run packing for journal records: arg1 = device | start | count. */
constexpr std::uint64_t kRunDeviceShift = 56;
constexpr std::uint64_t kRunStartShift = 16;
constexpr std::uint64_t kRunCountMask = (1ULL << 16) - 1;
constexpr std::uint64_t kRunStartMask = (1ULL << 40) - 1;

bool runFitsRecord(std::uint64_t device, std::uint64_t start, std::uint64_t count) {
    return device < (1ULL << (64 - kRunDeviceShift)) && start <= kRunStartMask &&
           count != 0 && count <= kRunCountMask;
}

std::uint64_t packRun(std::uint64_t device, std::uint64_t start, std::uint64_t count) {
    return (device << kRunDeviceShift) | ((start & kRunStartMask) << kRunStartShift) |
           (count & kRunCountMask);
}

bool unpackRun(std::uint64_t packed, std::uint64_t *device, std::uint64_t *start,
               std::uint64_t *count) {
    if (device == nullptr || start == nullptr || count == nullptr) {
        return false;
    }
    *device = packed >> kRunDeviceShift;
    *start = (packed >> kRunStartShift) & kRunStartMask;
    *count = packed & kRunCountMask;
    return *count != 0;
}

std::uint64_t systemNowNs(void *) {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

} // namespace

Namespace::Namespace(const NamespaceConfig &config, Clock clock, void *clockContext)
    : config_(config), clock_(clock != nullptr ? clock : systemNowNs),
      clockContext_(clockContext),
      picker_(config.placement, config.deviceExtents.size()) {
    /* Value-initialized atomic versions for every possible inode id. */
    dataVersionCount_ = static_cast<std::size_t>(config_.maxInodes) + 1;
    dataVersions_ = std::make_unique<std::atomic<std::uint64_t>[]>(dataVersionCount_);
    inodes_.resize(kRootInodeId + 1);
    Inode &root = inodes_[kRootInodeId];
    root.id = kRootInodeId;
    root.generation = 1;
    root.directory = true;
    root.parent = kRootInodeId;
    root.index = std::make_shared<DirectoryIndex>();
    root.mtimeNs = nowNs();
    root.ctimeNs = root.mtimeNs;
    namespaceBytes_ = config_.bytesPerInode;
    stats_.inodes = 1;
}

Namespace::~Namespace() = default;

Result<std::unique_ptr<Namespace>> Namespace::create(const NamespaceConfig &config,
                                                     Clock clock, void *clockContext) {
    if (config.bytesPerInode == 0 || config.maxInodes == 0 ||
        config.maxNamespaceBytes < config.bytesPerInode) {
        return makeError<std::unique_ptr<Namespace>>(Status::InvalidArgument,
                                                     "invalid namespace limits");
    }
    if (config.extentBytes == 0 ||
        config.deviceStartExtents.size() != config.deviceExtents.size()) {
        return makeError<std::unique_ptr<Namespace>>(Status::InvalidArgument,
                                                     "invalid extent placement config");
    }
    auto namespaceState = std::unique_ptr<Namespace>(new Namespace(config, clock, clockContext));
    for (std::size_t i = 0; i < config.deviceExtents.size(); ++i) {
        AllocatorConfig allocatorConfig;
        allocatorConfig.deviceId = static_cast<std::uint32_t>(i);
        allocatorConfig.totalExtents = config.deviceExtents[i];
        allocatorConfig.maxReserveExtents = config.maxReserveExtents;
        if (config.capacityOverrideExtents != 0 &&
            config.capacityOverrideExtents < config.deviceExtents[i]) {
            allocatorConfig.capacityExtents = config.capacityOverrideExtents;
        }
        auto allocator = Allocator::create(allocatorConfig);
        if (!allocator.ok()) {
            return makeError<std::unique_ptr<Namespace>>(allocator.status(),
                                                         allocator.message());
        }
        namespaceState->allocators_.push_back(std::move(allocator).value());
    }
    return namespaceState;
}

std::uint64_t Namespace::nowNs() const noexcept { return clock_(clockContext_); }

void Namespace::touch(Inode &inode, std::uint64_t now, bool content) noexcept {
    inode.ctimeNs = now;
    if (content) {
        inode.mtimeNs = now;
    }
    if (logicalEpoch_ != std::numeric_limits<std::uint64_t>::max()) {
        ++logicalEpoch_;
    }
    inode.logicalEpoch = logicalEpoch_;
    logicalEpochPublished_.store(logicalEpoch_, std::memory_order_release);
    if (content) {
        /* Size/content changes invalidate the frontend data snapshot; pure
         * namespace-shape touches (rename/link/times) do not. */
        markSizeDirtyLocked(inode);
        publishDataVersionLocked(inode);
    }
}

/* Bumps the per-inode data version; readers use it as the snapshot key. */
void Namespace::publishDataVersionLocked(Inode &inode) noexcept {
    if (inode.id < dataVersionCount_) {
        dataVersions_[inode.id].fetch_add(1, std::memory_order_release);
    }
}

/* Tracks inodes whose durable size must advance on the next publish. */
void Namespace::markSizeDirtyLocked(Inode &inode) noexcept {
    if (inode.id != 0 && !inode.sizeDirty) {
        inode.sizeDirty = true;
        dirtyInodes_.push_back(inode.id);
    }
}

std::uint64_t Namespace::dataVersionFast(FileHandle handle) const noexcept {
    if (handle.inodeId >= dataVersionCount_) {
        return 0;
    }
    return dataVersions_[handle.inodeId].load(std::memory_order_acquire);
}

std::uint64_t Namespace::entryBytes(std::string_view name) const noexcept {
    return name.size() + config_.bytesPerDirectoryEntry;
}

Result<std::vector<std::string_view>> Namespace::splitPath(std::string_view path) {
    if (path.empty() || path.front() != '/') {
        return makeError<std::vector<std::string_view>>(Status::InvalidArgument,
                                                        "path must be absolute");
    }
    std::vector<std::string_view> parts;
    if (path.size() == 1) {
        return parts; /* root */
    }
    if (path.back() == '/') {
        return makeError<std::vector<std::string_view>>(Status::InvalidArgument,
                                                        "trailing slash is not allowed");
    }
    std::size_t start = 1;
    while (start <= path.size()) {
        std::size_t slash = path.find('/', start);
        if (slash == std::string_view::npos) {
            slash = path.size();
        }
        const std::string_view component = path.substr(start, slash - start);
        if (component.empty() || component == "." || component == "..") {
            return makeError<std::vector<std::string_view>>(
                Status::InvalidArgument, "invalid path component");
        }
        if (component.size() > kMaxNameBytes) {
            return makeError<std::vector<std::string_view>>(Status::InvalidArgument,
                                                            "path component too long");
        }
        parts.push_back(component);
        start = slash + 1;
    }
    return parts;
}

Result<FileHandle> Namespace::makeHandle(std::uint64_t inodeId) const {
    const Inode &inode = inodes_[inodeId];
    return FileHandle{inodeId, inode.generation};
}

Result<Inode *> Namespace::lookupInode(FileHandle handle) noexcept {
    if (!handle.valid() || handle.inodeId >= inodes_.size()) {
        return makeError<Inode *>(Status::NotFound, "stale handle");
    }
    Inode &inode = inodes_[handle.inodeId];
    if (inode.id == 0 || inode.generation != handle.generation) {
        return makeError<Inode *>(Status::NotFound, "stale handle");
    }
    return &inode;
}

Result<const Inode *> Namespace::lookupInode(FileHandle handle) const noexcept {
    if (!handle.valid() || handle.inodeId >= inodes_.size()) {
        return makeError<const Inode *>(Status::NotFound, "stale handle");
    }
    const Inode &inode = inodes_[handle.inodeId];
    if (inode.id == 0 || inode.generation != handle.generation) {
        return makeError<const Inode *>(Status::NotFound, "stale handle");
    }
    return &inode;
}

Result<std::uint64_t> Namespace::resolveDirectory(std::string_view path) const {
    auto parts = splitPath(path);
    if (!parts.ok()) {
        return makeError<std::uint64_t>(parts.status(), parts.message());
    }
    std::uint64_t current = kRootInodeId;
    for (const std::string_view component : parts.value()) {
        const Inode &inode = inodes_[current];
        if (!inode.directory) {
            return makeError<std::uint64_t>(Status::NotFound,
                                            "path component is not a directory");
        }
        const auto it = inode.index->byName.find(component);
        if (it == inode.index->byName.end()) {
            return makeError<std::uint64_t>(Status::NotFound, "path not found");
        }
        current = it->second;
    }
    if (!inodes_[current].directory) {
        return makeError<std::uint64_t>(Status::NotFound, "path is not a directory");
    }
    return current;
}

Result<Namespace::ParentRef> Namespace::resolveParent(std::string_view path) const {
    auto parts = splitPath(path);
    if (!parts.ok()) {
        return makeError<ParentRef>(parts.status(), parts.message());
    }
    if (parts.value().empty()) {
        return makeError<ParentRef>(Status::InvalidArgument, "root has no parent");
    }
    std::uint64_t current = kRootInodeId;
    for (std::size_t i = 0; i + 1 < parts.value().size(); ++i) {
        const Inode &inode = inodes_[current];
        if (!inode.directory) {
            return makeError<ParentRef>(Status::NotFound,
                                        "path component is not a directory");
        }
        const auto it = inode.index->byName.find(parts.value()[i]);
        if (it == inode.index->byName.end()) {
            return makeError<ParentRef>(Status::NotFound, "path not found");
        }
        current = it->second;
    }
    if (!inodes_[current].directory) {
        return makeError<ParentRef>(Status::NotFound, "parent is not a directory");
    }
    return ParentRef{current, parts.value().back()};
}

Result<Namespace::Resolved> Namespace::resolveEntry(std::string_view path) const {
    auto parts = splitPath(path);
    if (!parts.ok()) {
        return makeError<Resolved>(parts.status(), parts.message());
    }
    if (parts.value().empty()) {
        return makeError<Resolved>(Status::InvalidArgument, "root is a directory");
    }
    std::uint64_t current = kRootInodeId;
    for (std::size_t i = 0; i + 1 < parts.value().size(); ++i) {
        const Inode &inode = inodes_[current];
        if (!inode.directory) {
            return makeError<Resolved>(Status::NotFound,
                                       "path component is not a directory");
        }
        const auto it = inode.index->byName.find(parts.value()[i]);
        if (it == inode.index->byName.end()) {
            return makeError<Resolved>(Status::NotFound, "path not found");
        }
        current = it->second;
    }
    const Inode &parent = inodes_[current];
    if (!parent.directory) {
        return makeError<Resolved>(Status::NotFound, "parent is not a directory");
    }
    const auto it = parent.index->byName.find(parts.value().back());
    if (it == parent.index->byName.end()) {
        return makeError<Resolved>(Status::NotFound, "path not found");
    }
    return Resolved{current, parts.value().back(), it->second};
}

Result<Inode *> Namespace::createInode(bool directory, std::uint64_t parent,
                                       std::uint64_t now) noexcept {
    std::uint64_t inodeId = 0;
    if (!freeSlots_.empty()) {
        inodeId = freeSlots_.back();
        freeSlots_.pop_back();
        Inode &inode = inodes_[inodeId];
        const std::uint32_t next = inode.generation + 1;
        inode = Inode{};
        inode.id = inodeId;
        inode.generation = next == 0 ? 1 : next;
    } else {
        inodes_.push_back(Inode{});
        inodeId = inodes_.size() - 1;
        Inode &inode = inodes_[inodeId];
        inode.id = inodeId;
        inode.generation = 1;
    }
    Inode &inode = inodes_[inodeId];
    inode.directory = directory;
    inode.parent = parent;
    inode.mtimeNs = now;
    inode.ctimeNs = now;
    inode.extents.configure(config_.extentBytes);
    if (directory) {
        inode.index = std::make_shared<DirectoryIndex>();
    }
    ++stats_.inodes;
    namespaceBytes_ += config_.bytesPerInode;
    return &inode;
}

void Namespace::freeInode(std::uint64_t inodeId) noexcept {
    Inode &inode = inodes_[inodeId];
    for (const ExtentDescriptor &descriptor : inode.extents.snapshot()) {
        releaseDescriptor(descriptor);
    }
    inode.extents.removeFrom(0);
    publishDataVersionLocked(inode);
    inode.id = 0; /* slot is free; generation is kept for the next reuse */
    inode.directory = false;
    inode.linkCount = 0;
    inode.openCount = 0;
    inode.logicalSize = 0;
    inode.sizeDirty = false;
    inode.parent = 0;
    inode.childCount = 0;
    inode.index.reset();
    freeSlots_.push_back(inodeId);
    --stats_.inodes;
    namespaceBytes_ -= config_.bytesPerInode;
}

void Namespace::linkEntry(Inode &parent, std::string_view name, std::uint64_t inodeId) {
    parent.index->byName.emplace(std::string(name), inodeId);
    parent.index->ordered.emplace(std::string(name), inodeId);
    ++parent.childCount;
}

std::uint64_t Namespace::unlinkEntry(Inode &parent, std::string_view name) {
    const auto hashIt = parent.index->byName.find(name);
    const std::uint64_t inodeId = hashIt->second;
    parent.index->byName.erase(hashIt);
    const auto orderedIt = parent.index->ordered.find(name);
    if (orderedIt != parent.index->ordered.end()) {
        parent.index->ordered.erase(orderedIt);
    }
    --parent.childCount;
    return inodeId;
}

bool Namespace::isAncestor(const Inode &maybeAncestor, std::uint64_t inodeId) const noexcept {
    std::uint64_t current = inodeId;
    for (;;) {
        if (current == maybeAncestor.id) {
            return true;
        }
        if (current == kRootInodeId) {
            return false;
        }
        current = inodes_[current].parent;
    }
}

Result<FileHandle> Namespace::create(std::string_view path, bool exclusive, bool truncate,
                                     bool directory) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto parentRef = resolveParent(path);
    if (!parentRef.ok()) {
        return makeError<FileHandle>(parentRef.status(), parentRef.message());
    }
    const std::uint64_t parentId = parentRef.value().inode;
    const std::string_view name = parentRef.value().name;

    {
        Inode &parent = inodes_[parentId];
        const auto it = parent.index->byName.find(name);
        if (it != parent.index->byName.end()) {
            if (exclusive) {
                return makeError<FileHandle>(Status::AlreadyExists, "path already exists");
            }
            Inode &existing = inodes_[it->second];
            if (existing.directory != directory) {
                return makeError<FileHandle>(Status::AlreadyExists,
                                             "path exists with another type");
            }
            if (truncate && !existing.directory) {
                existing.logicalSize = 0;
                touch(existing, nowNs(), true);
                requireCheckpointLocked();
            }
            ++existing.openCount;
            ++stats_.opens;
            return makeHandle(existing.id);
        }
    }

    if (stats_.inodes + 1 > config_.maxInodes) {
        ++stats_.limitRejects;
        return makeError<FileHandle>(Status::NoMemory, "max_inodes exceeded");
    }
    const std::uint64_t entryCost = entryBytes(name);
    if (namespaceBytes_ + config_.bytesPerInode + entryCost > config_.maxNamespaceBytes) {
        ++stats_.limitRejects;
        return makeError<FileHandle>(Status::NoMemory, "max_namespace_bytes exceeded");
    }

    const std::uint64_t now = nowNs();
    auto inodeResult = createInode(directory, parentId, now);
    if (!inodeResult.ok()) {
        return makeError<FileHandle>(inodeResult.status(), inodeResult.message());
    }
    const std::uint64_t inodeId = inodeResult.value()->id;
    {
        Inode &parent = inodes_[parentId];
        linkEntry(parent, name, inodeId);
        touch(parent, now, true);
    }
    Inode &inode = inodes_[inodeId];
    inode.linkCount = 1;
    inode.openCount = 1;
    namespaceBytes_ += entryCost;
    ++stats_.creates;
    ++stats_.opens;
    /* Namespace shape changes stay on the full checkpoint path (§9.3). */
    requireCheckpointLocked();
    return makeHandle(inodeId);
}

Result<FileHandle> Namespace::open(std::string_view path) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (path.size() == 1 && path.front() == '/') {
        Inode &root = inodes_[kRootInodeId];
        ++root.openCount;
        ++stats_.opens;
        return makeHandle(kRootInodeId);
    }
    auto resolved = resolveEntry(path);
    if (!resolved.ok()) {
        return makeError<FileHandle>(resolved.status(), resolved.message());
    }
    Inode &inode = inodes_[resolved.value().inode];
    ++inode.openCount;
    ++stats_.opens;
    return makeHandle(inode.id);
}

Result<void> Namespace::remove(std::string_view path) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto resolved = resolveEntry(path);
    if (!resolved.ok()) {
        return makeError(resolved.status(), resolved.message());
    }
    const std::uint64_t inodeId = resolved.value().inode;
    if (inodeId == kRootInodeId) {
        return makeError(Status::InvalidArgument, "cannot remove the root directory");
    }
    Inode &inode = inodes_[inodeId];
    if (inode.directory && inode.childCount != 0) {
        return makeError(Status::InvalidArgument, "directory not empty");
    }
    const std::uint64_t now = nowNs();
    const std::uint64_t entryCost = entryBytes(resolved.value().name);
    {
        Inode &parent = inodes_[resolved.value().parent];
        unlinkEntry(parent, resolved.value().name);
        touch(parent, now, true);
    }
    namespaceBytes_ -= entryCost;
    --inode.linkCount;
    touch(inode, now, true);
    if (inode.linkCount == 0 && inode.openCount == 0) {
        freeInode(inodeId);
    }
    ++stats_.removes;
    requireCheckpointLocked();
    return Result<void>(Status::Ok);
}

Result<void> Namespace::rename(std::string_view from, std::string_view to) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (from == to) {
        return Result<void>(Status::Ok);
    }
    auto source = resolveEntry(from);
    if (!source.ok()) {
        return makeError(source.status(), source.message());
    }
    const std::uint64_t inodeId = source.value().inode;
    if (inodeId == kRootInodeId) {
        return makeError(Status::InvalidArgument, "cannot rename the root directory");
    }
    auto targetRef = resolveParent(to);
    if (!targetRef.ok()) {
        return makeError(targetRef.status(), targetRef.message());
    }
    const std::uint64_t fromParentId = source.value().parent;
    const std::string_view fromName = source.value().name;
    const std::uint64_t toParentId = targetRef.value().inode;
    const std::string_view toName = targetRef.value().name;
    const std::uint64_t now = nowNs();

    const bool sourceIsDirectory = inodes_[inodeId].directory;
    if (sourceIsDirectory && isAncestor(inodes_[inodeId], toParentId)) {
        return makeError(Status::InvalidArgument,
                         "cannot move a directory into its own subtree");
    }

    std::uint64_t replacedId = 0;
    {
        Inode &toParent = inodes_[toParentId];
        const auto it = toParent.index->byName.find(toName);
        if (it != toParent.index->byName.end()) {
            replacedId = it->second;
            if (replacedId == inodeId) {
                return Result<void>(Status::Ok);
            }
            Inode &target = inodes_[replacedId];
            if (target.directory != sourceIsDirectory) {
                return makeError(Status::AlreadyExists, "target type mismatch");
            }
            if (target.directory && target.childCount != 0) {
                return makeError(Status::InvalidArgument, "target directory not empty");
            }
        }
    }

    const std::uint64_t freed = entryBytes(fromName) +
                                (replacedId != 0 ? entryBytes(toName) : 0);
    const std::uint64_t added = entryBytes(toName);
    if (namespaceBytes_ - freed + added > config_.maxNamespaceBytes) {
        ++stats_.limitRejects;
        return makeError(Status::NoMemory, "max_namespace_bytes exceeded");
    }

    /* All checks passed: the mutation below cannot fail or expose a partial
     * state to other clients (the namespace mutex covers the whole rename). */
    if (replacedId != 0) {
        Inode &toParent = inodes_[toParentId];
        unlinkEntry(toParent, toName);
        namespaceBytes_ -= entryBytes(toName);
        Inode &target = inodes_[replacedId];
        --target.linkCount;
        touch(target, now, false);
        if (target.linkCount == 0 && target.openCount == 0) {
            freeInode(replacedId);
        }
    }
    {
        Inode &fromParent = inodes_[fromParentId];
        unlinkEntry(fromParent, fromName);
        namespaceBytes_ -= entryBytes(fromName);
        touch(fromParent, now, true);
    }
    {
        Inode &toParent = inodes_[toParentId];
        linkEntry(toParent, toName, inodeId);
        namespaceBytes_ += entryBytes(toName);
        touch(toParent, now, true);
    }
    Inode &inode = inodes_[inodeId];
    if (inode.directory) {
        inode.parent = toParentId;
    }
    touch(inode, now, false);
    ++stats_.renames;
    requireCheckpointLocked();
    return Result<void>(Status::Ok);
}

/* Adds one directory entry to an existing regular inode atomically. */
Result<void> Namespace::link(std::string_view from, std::string_view to) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto source = resolveEntry(from);
    if (!source.ok()) {
        return makeError(source.status(), source.message());
    }
    Inode &inode = inodes_[source.value().inode];
    if (inode.directory) {
        return makeError(Status::InvalidArgument, "cannot hard-link a directory");
    }
    auto target = resolveParent(to);
    if (!target.ok()) {
        return makeError(target.status(), target.message());
    }
    Inode &parent = inodes_[target.value().inode];
    const std::string_view name = target.value().name;
    if (parent.index->byName.find(name) != parent.index->byName.end()) {
        return makeError(Status::AlreadyExists, "link target already exists");
    }
    const std::uint64_t entryCost = entryBytes(name);
    if (namespaceBytes_ + entryCost > config_.maxNamespaceBytes) {
        ++stats_.limitRejects;
        return makeError(Status::NoMemory, "max_namespace_bytes exceeded");
    }
    const std::uint64_t now = nowNs();
    linkEntry(parent, name, inode.id);
    namespaceBytes_ += entryCost;
    ++inode.linkCount;
    touch(parent, now, true);
    touch(inode, now, false);
    requireCheckpointLocked();
    return Result<void>(Status::Ok);
}

Result<void> Namespace::setTimes(FileHandle handle, std::uint64_t mtimeNs,
                                 std::uint64_t ctimeNs) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto inodeResult = lookupInode(handle);
    if (!inodeResult.ok()) {
        return makeError(inodeResult.status(), inodeResult.message());
    }
    Inode &inode = *inodeResult.value();
    inode.mtimeNs = mtimeNs;
    inode.ctimeNs = ctimeNs;
    stageRecord(JournalRecord{JournalRecordType::SetTimes, inode.id, mtimeNs, ctimeNs});
    return Result<void>(Status::Ok);
}

Result<DirectoryPage> Namespace::getChildren(std::string_view path,
                                             std::string_view cursor,
                                             std::size_t maxEntries) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (maxEntries == 0) {
        return makeError<DirectoryPage>(Status::InvalidArgument,
                                        "maxEntries must be greater than zero");
    }
    auto dirId = resolveDirectory(path);
    if (!dirId.ok()) {
        return makeError<DirectoryPage>(dirId.status(), dirId.message());
    }
    const Inode &dir = inodes_[dirId.value()];
    DirectoryPage page;
    auto it = cursor.empty() ? dir.index->ordered.begin()
                             : dir.index->ordered.upper_bound(cursor);
    for (; it != dir.index->ordered.end() && page.entries.size() < maxEntries; ++it) {
        const Inode &entry = inodes_[it->second];
        page.entries.push_back(
            DirectoryEntry{it->first, entry.id, entry.generation, entry.directory});
    }
    if (it != dir.index->ordered.end() && !page.entries.empty()) {
        page.nextCursor = page.entries.back().name;
    }
    return page;
}

Result<FileStat> Namespace::stat(FileHandle handle) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto inodeResult = lookupInode(handle);
    if (!inodeResult.ok()) {
        return makeError<FileStat>(inodeResult.status(), inodeResult.message());
    }
    const Inode &inode = *inodeResult.value();
    return FileStat{inode.id,        inode.generation, inode.directory, inode.linkCount,
                    inode.openCount, inode.logicalSize, inode.durableSize, inode.mtimeNs,
                    inode.ctimeNs,   inode.logicalEpoch};
}

Result<FileStat> Namespace::stat(std::string_view path) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto resolved = resolveEntry(path);
    if (!resolved.ok()) {
        return makeError<FileStat>(resolved.status(), resolved.message());
    }
    const Inode &inode = inodes_[resolved.value().inode];
    return FileStat{inode.id,        inode.generation, inode.directory, inode.linkCount,
                    inode.openCount, inode.logicalSize, inode.durableSize, inode.mtimeNs,
                    inode.ctimeNs,   inode.logicalEpoch};
}

Result<void> Namespace::close(FileHandle handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto inodeResult = lookupInode(handle);
    if (!inodeResult.ok()) {
        return makeError(inodeResult.status(), inodeResult.message());
    }
    Inode &inode = *inodeResult.value();
    if (inode.openCount == 0) {
        return makeError(Status::InvalidArgument, "handle is already closed");
    }
    --inode.openCount;
    if (inode.id != kRootInodeId && inode.linkCount == 0 && inode.openCount == 0) {
        freeInode(inode.id);
        requireCheckpointLocked();
    }
    return Result<void>(Status::Ok);
}

void Namespace::releaseDescriptor(const ExtentDescriptor &descriptor) noexcept {
    if (descriptor.deviceId >= allocators_.size() ||
        descriptor.deviceId >= config_.deviceStartExtents.size()) {
        return;
    }
    const std::uint64_t regionStart = config_.deviceStartExtents[descriptor.deviceId];
    if (descriptor.physicalExtent < regionStart) {
        return;
    }
    allocators_[descriptor.deviceId]->release(descriptor.physicalExtent - regionStart,
                                              descriptor.count);
    stats_.freedExtents += descriptor.count;
}

Result<std::vector<ExtentAllocation>> Namespace::reserveExtents(FileHandle handle,
                                                                std::uint64_t offset,
                                                                std::uint64_t length) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto inodeResult = lookupInode(handle);
    if (!inodeResult.ok()) {
        return makeError<std::vector<ExtentAllocation>>(inodeResult.status(),
                                                        inodeResult.message());
    }
    if (config_.deviceExtents.empty()) {
        return makeError<std::vector<ExtentAllocation>>(Status::NotSupported,
                                                        "no data devices configured");
    }
    if (length == 0) {
        return makeError<std::vector<ExtentAllocation>>(Status::InvalidArgument,
                                                        "zero-length allocation");
    }
    if (offset > std::numeric_limits<std::uint64_t>::max() - length) {
        return makeError<std::vector<ExtentAllocation>>(Status::InvalidArgument,
                                                        "allocation range overflows");
    }
    Inode &inode = *inodeResult.value();
    const std::uint64_t extentBytes = config_.extentBytes;
    const std::uint64_t logicalStart = offset / extentBytes;
    const std::uint64_t end = offset + length;
    const std::uint64_t logicalEnd = end / extentBytes + (end % extentBytes != 0 ? 1 : 0);
    std::vector<ExtentAllocation> allocations;
    std::vector<std::uint64_t> freeCounts(allocators_.size(), 0);

    auto rollback = [&]() {
        for (const ExtentAllocation &allocation : allocations) {
            allocators_[allocation.run.deviceId]->release(allocation.run.start,
                                                          allocation.run.count);
        }
        allocations.clear();
    };

    /* Binary-search the first mapped descriptor that can overlap the range and
     * walk only descriptors inside it (O(log E + k), no full-map copy). */
    const std::size_t descriptorCount = inode.extents.size();
    std::size_t index = inode.extents.firstOverlapping(logicalStart);
    std::uint64_t cursor = logicalStart;
    while (cursor < logicalEnd) {
        while (index < descriptorCount) {
            const ExtentDescriptor *mapped = inode.extents.descriptorAt(index);
            if (mapped != nullptr &&
                mapped->logicalExtent + mapped->count <= cursor) {
                ++index;
                continue;
            }
            break;
        }
        const ExtentDescriptor *next = inode.extents.descriptorAt(index);
        if (next != nullptr && next->logicalExtent <= cursor) {
            cursor = next->logicalExtent + next->count;
            continue;
        }
        std::uint64_t runEnd = logicalEnd;
        if (next != nullptr && next->logicalExtent < runEnd) {
            runEnd = next->logicalExtent;
        }
        std::uint64_t remaining = runEnd - cursor;
        std::uint64_t logical = cursor;
        while (remaining > 0) {
            std::uint64_t device = 0;
            std::uint64_t take = 0;
            for (std::size_t attempt = 0; attempt < allocators_.size(); ++attempt) {
                for (std::size_t i = 0; i < allocators_.size(); ++i) {
                    freeCounts[i] = allocators_[i]->allocatableExtents();
                }
                device = picker_.pick(freeCounts);
                take = std::min(remaining, freeCounts[device]);
                if (take > 0) {
                    break;
                }
            }
            if (take == 0) {
                rollback();
                return makeError<std::vector<ExtentAllocation>>(
                    Status::NoSpace, "no allocatable extents across data devices");
            }
            auto runs = allocators_[device]->reserveScattered(take);
            if (!runs.ok()) {
                rollback();
                return makeError<std::vector<ExtentAllocation>>(runs.status(),
                                                                 runs.message());
            }
            for (const ExtentRun &run : runs.value()) {
                allocations.push_back(ExtentAllocation{run, logical});
                logical += run.count;
            }
            remaining -= take;
        }
        cursor = runEnd;
    }
    return allocations;
}

Result<void> Namespace::commitExtents(FileHandle handle,
                                      std::span<const ExtentAllocation> allocations) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto inodeResult = lookupInode(handle);
    if (!inodeResult.ok()) {
        return makeError(inodeResult.status(), inodeResult.message());
    }
    if (allocations.empty()) {
        return Result<void>(Status::Ok);
    }
    Inode &inode = *inodeResult.value();
    std::vector<ExtentDescriptor> descriptors;
    descriptors.reserve(allocations.size());

    /* Validate the complete publish set before changing allocator state. The
     * extent map is checked in place (no full-map copy). */
    for (const ExtentAllocation &allocation : allocations) {
        const std::uint64_t device = allocation.run.deviceId;
        if (allocation.run.count == 0 || device >= allocators_.size() ||
            device >= config_.deviceStartExtents.size() ||
            allocation.run.start > allocators_[device]->totalExtents() -
                                       std::min(allocation.run.count,
                                                allocators_[device]->totalExtents())) {
            return makeError(Status::InvalidArgument, "allocation device out of range");
        }
        if (allocation.run.start + allocation.run.count >
            allocators_[device]->totalExtents()) {
            return makeError(Status::InvalidArgument, "allocation range out of bounds");
        }
        for (std::uint64_t i = 0; i < allocation.run.count; ++i) {
            if (allocators_[device]->stateAt(allocation.run.start + i) !=
                ExtentState::Reserved) {
                return makeError(Status::InvalidState,
                                 "allocation is not an outstanding reservation");
            }
        }
        ExtentDescriptor descriptor;
        descriptor.logicalExtent = allocation.logicalExtent;
        descriptor.physicalExtent = config_.deviceStartExtents[device] + allocation.run.start;
        descriptor.count = allocation.run.count;
        descriptor.deviceId = static_cast<std::uint32_t>(device);
        descriptors.push_back(descriptor);
    }
    auto validated = inode.extents.validateInsert(descriptors);
    if (!validated.ok()) {
        return validated;
    }

    std::size_t committedRuns = 0;
    for (const ExtentAllocation &allocation : allocations) {
        const std::uint64_t device = allocation.run.deviceId;
        auto initialized =
            allocators_[device]->markInitialized(allocation.run.start, allocation.run.count);
        auto committed = initialized.ok()
                             ? allocators_[device]->commit(allocation.run.start,
                                                           allocation.run.count)
                             : initialized;
        if (!committed.ok()) {
            /* Restore the allocator to FREE; the inode map is still untouched. */
            for (std::size_t i = 0; i < committedRuns; ++i) {
                const auto &done = allocations[i];
                allocators_[done.run.deviceId]->release(done.run.start, done.run.count);
            }
            return committed;
        }
        ++committedRuns;
    }
    /* The map was validated above; commit it in place. On an unexpected insert
     * failure, return the allocator runs to FREE so state stays conserved. */
    auto added = inode.extents.addExtents(descriptors);
    if (!added.ok()) {
        for (const ExtentAllocation &allocation : allocations) {
            allocators_[allocation.run.deviceId]->release(allocation.run.start,
                                                          allocation.run.count);
        }
        return added;
    }
    for (const ExtentAllocation &allocation : allocations) {
        stats_.allocatedExtents += allocation.run.count;
    }
    /* Extent-map changes invalidate readers even without a size change. */
    publishDataVersionLocked(inode);
    touch(inode, nowNs(), false);
    for (const ExtentAllocation &allocation : allocations) {
        const std::uint64_t device = allocation.run.deviceId;
        if (!runFitsRecord(device, allocation.run.start, allocation.run.count)) {
            requireCheckpointLocked();
            break;
        }
        stageRecord(JournalRecord{
            JournalRecordType::Alloc, inode.id, allocation.logicalExtent,
            packRun(device, allocation.run.start, allocation.run.count)});
    }
    return Result<void>(Status::Ok);
}

Result<void> Namespace::releaseExtents(FileHandle handle,
                                       std::span<const ExtentAllocation> allocations) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto inodeResult = lookupInode(handle);
    if (!inodeResult.ok()) {
        return makeError(inodeResult.status(), inodeResult.message());
    }
    for (const ExtentAllocation &allocation : allocations) {
        if (allocation.run.count == 0 || allocation.run.deviceId >= allocators_.size()) {
            return makeError(Status::InvalidArgument, "allocation device out of range");
        }
        auto *allocator = allocators_[allocation.run.deviceId].get();
        if (allocation.run.start > allocator->totalExtents() -
                                       std::min(allocation.run.count,
                                                allocator->totalExtents()) ||
            allocation.run.start + allocation.run.count > allocator->totalExtents()) {
            return makeError(Status::InvalidArgument, "allocation range out of bounds");
        }
        for (std::uint64_t i = 0; i < allocation.run.count; ++i) {
            if (allocator->stateAt(allocation.run.start + i) != ExtentState::Reserved) {
                return makeError(Status::InvalidState,
                                 "allocation is not an outstanding reservation");
            }
        }
    }
    for (const ExtentAllocation &allocation : allocations) {
        auto released = allocators_[allocation.run.deviceId]->release(allocation.run.start,
                                                                      allocation.run.count);
        if (!released.ok()) {
            return released;
        }
    }
    for (const ExtentAllocation &allocation : allocations) {
        const std::uint64_t device = allocation.run.deviceId;
        if (!runFitsRecord(device, allocation.run.start, allocation.run.count)) {
            requireCheckpointLocked();
            break;
        }
        stageRecord(JournalRecord{
            JournalRecordType::Free, inodeResult.value()->id, 0,
            packRun(device, allocation.run.start, allocation.run.count)});
    }
    return Result<void>(Status::Ok);
}

Result<std::vector<ExtentDescriptor>> Namespace::extents(FileHandle handle) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto inodeResult = lookupInode(handle);
    if (!inodeResult.ok()) {
        return makeError<std::vector<ExtentDescriptor>>(inodeResult.status(),
                                                        inodeResult.message());
    }
    return inodeResult.value()->extents.snapshot();
}

/* Shared by truncateFile and journal replay; mutex_ is held by the caller. */
Result<void> Namespace::applyTruncateLocked(Inode &inode, std::uint64_t size) {
    const std::uint64_t extentBytes = config_.extentBytes;
    const std::uint64_t keep = size / extentBytes + (size % extentBytes != 0 ? 1 : 0);
    for (const ExtentDescriptor &descriptor : inode.extents.snapshot()) {
        if (descriptor.logicalExtent + descriptor.count <= keep) {
            continue;
        }
        if (descriptor.logicalExtent >= keep) {
            releaseDescriptor(descriptor);
            continue;
        }
        ExtentDescriptor tail = descriptor;
        const std::uint64_t keepCount = keep - descriptor.logicalExtent;
        tail.logicalExtent = keep;
        tail.physicalExtent = descriptor.physicalExtent + keepCount;
        tail.count = descriptor.count - keepCount;
        releaseDescriptor(tail);
    }
    inode.extents.removeFrom(keep);
    inode.logicalSize = size;
    touch(inode, nowNs(), true);
    return Result<void>(Status::Ok);
}

Result<void> Namespace::truncateFile(FileHandle handle, std::uint64_t size) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto inodeResult = lookupInode(handle);
    if (!inodeResult.ok()) {
        return makeError(inodeResult.status(), inodeResult.message());
    }
    Inode &inode = *inodeResult.value();
    auto truncated = applyTruncateLocked(inode, size);
    if (!truncated.ok()) {
        return truncated;
    }
    /* Replay recomputes the released runs from the inode extent map. */
    stageRecord(JournalRecord{JournalRecordType::Truncate, inode.id, size, 0});
    return Result<void>(Status::Ok);
}

Result<void> Namespace::setSize(FileHandle handle, std::uint64_t size) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto inodeResult = lookupInode(handle);
    if (!inodeResult.ok()) {
        return makeError(inodeResult.status(), inodeResult.message());
    }
    Inode &inode = *inodeResult.value();
    if (size > inode.logicalSize) {
        inode.logicalSize = size;
        touch(inode, nowNs(), true);
        stageRecord(JournalRecord{JournalRecordType::SetSize, inode.id, size, 0});
    }
    return Result<void>(Status::Ok);
}

/* Barrier-side accessors: the barrier runs on another thread. */
bool Namespace::checkpointRequired() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return checkpointRequired_;
}

void Namespace::clearPendingRecords() {
    std::lock_guard<std::mutex> lock(mutex_);
    pendingRecords_.clear();
    pendingEpoch_ = 0;
    checkpointRequired_ = false;
}

void Namespace::requireCheckpoint() {
    std::lock_guard<std::mutex> lock(mutex_);
    requireCheckpointLocked();
}

Namespace::StagedBatch Namespace::takeStagedRecords() {
    std::lock_guard<std::mutex> lock(mutex_);
    StagedBatch batch;
    batch.records = std::move(pendingRecords_);
    batch.epoch = pendingEpoch_;
    pendingRecords_.clear();
    pendingEpoch_ = 0;
    return batch;
}

/* Stages one journal record for the next durable barrier (§9.3). */
void Namespace::stageRecord(JournalRecord record) {
    if (checkpointRequired_) {
        return; /* the checkpoint snapshot covers every staged mutation */
    }
    pendingRecords_.push_back(record);
    pendingEpoch_ = std::max(pendingEpoch_, logicalEpoch_);
}

Inode *Namespace::inodeById(std::uint64_t inodeId) noexcept {
    if (inodeId >= inodes_.size() || inodes_[inodeId].id != inodeId) {
        return nullptr;
    }
    return &inodes_[inodeId];
}

/* Applies one replayed journal record to the resident state. Replay starts
 * after the checkpoint sequence, so records are never re-applied to a state
 * that already contains their effect; Alloc/Free tolerate a committed or
 * released run because Journal::append may checkpoint before writing. */
Result<void> Namespace::applyJournalRecord(const JournalRecord &record) {
    std::lock_guard<std::mutex> lock(mutex_);
    switch (record.type) {
    case JournalRecordType::Checkpoint:
        return Result<void>(Status::Ok);
    case JournalRecordType::SetSize: {
        Inode *inode = inodeById(record.objectId);
        if (inode == nullptr) {
            return makeError(Status::NotFound, "replayed SetSize inode is missing");
        }
        if (record.arg0 > inode->logicalSize) {
            inode->logicalSize = record.arg0;
        }
        touch(*inode, nowNs(), true);
        return Result<void>(Status::Ok);
    }
    case JournalRecordType::SetTimes: {
        Inode *inode = inodeById(record.objectId);
        if (inode == nullptr) {
            return makeError(Status::NotFound, "replayed SetTimes inode is missing");
        }
        inode->mtimeNs = record.arg0;
        inode->ctimeNs = record.arg1;
        touch(*inode, nowNs(), false);
        return Result<void>(Status::Ok);
    }
    case JournalRecordType::Alloc: {
        Inode *inode = inodeById(record.objectId);
        if (inode == nullptr) {
            return makeError(Status::NotFound, "replayed Alloc inode is missing");
        }
        std::uint64_t device = 0;
        std::uint64_t start = 0;
        std::uint64_t count = 0;
        if (!unpackRun(record.arg1, &device, &start, &count) ||
            device >= allocators_.size() || device >= config_.deviceStartExtents.size()) {
            return makeError(Status::Corrupt, "replayed Alloc run is invalid");
        }
        Allocator *allocator = allocators_[device].get();
        /* The checkpoint may predate the reservation (Free) or the zero-fill
         * (Reserved/Initialized); walk the run up to COMMITTED. */
        ExtentState state = allocator->stateAt(start);
        if (state == ExtentState::Free) {
            auto reserved = allocator->markReserved(start, count);
            if (!reserved.ok()) {
                return reserved;
            }
            state = ExtentState::Reserved;
        }
        if (state != ExtentState::Committed) {
            auto initialized = allocator->markInitialized(start, count);
            if (!initialized.ok()) {
                return initialized;
            }
            auto committed = allocator->commit(start, count);
            if (!committed.ok()) {
                return committed;
            }
        }
        ExtentDescriptor descriptor;
        descriptor.logicalExtent = record.arg0;
        descriptor.physicalExtent =
            deviceStartExtent(static_cast<std::uint32_t>(device)) + start;
        descriptor.count = count;
        descriptor.deviceId = static_cast<std::uint32_t>(device);
        auto added =
            inode->extents.addExtents(std::span<const ExtentDescriptor>(&descriptor, 1));
        if (!added.ok()) {
            return added;
        }
        publishDataVersionLocked(*inode);
        touch(*inode, nowNs(), false);
        return Result<void>(Status::Ok);
    }
    case JournalRecordType::Free: {
        std::uint64_t device = 0;
        std::uint64_t start = 0;
        std::uint64_t count = 0;
        if (!unpackRun(record.arg1, &device, &start, &count) ||
            device >= allocators_.size()) {
            return makeError(Status::Corrupt, "replayed Free run is invalid");
        }
        Allocator *allocator = allocators_[device].get();
        if (allocator->stateAt(start) == ExtentState::Free) {
            return Result<void>(Status::Ok); /* already released */
        }
        return allocator->release(start, count);
    }
    case JournalRecordType::Truncate: {
        Inode *inode = inodeById(record.objectId);
        if (inode == nullptr) {
            return makeError(Status::NotFound, "replayed Truncate inode is missing");
        }
        return applyTruncateLocked(*inode, record.arg0);
    }
    default:
        return makeError(Status::NotSupported,
                         "journal record type requires the checkpoint path");
    }
}

std::uint64_t Namespace::logicalEpoch() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return logicalEpoch_;
}

/* Frontend caches use this acquire load only as an invalidation key. */
std::uint64_t Namespace::logicalEpochFast() const noexcept {
    return logicalEpochPublished_.load(std::memory_order_acquire);
}

std::uint64_t Namespace::durableEpoch() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return durableEpoch_;
}

Result<void> Namespace::publishDurableEpoch(std::uint64_t epoch) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (epoch > logicalEpoch_) {
        return makeError(Status::InvalidArgument,
                         "durable epoch is ahead of logical epoch");
    }
    if (epoch < durableEpoch_) {
        return makeError(Status::InvalidArgument,
                         "durable epoch cannot move backwards");
    }
    durableEpoch_ = epoch;
    /* Only inodes whose size changed since the last publish are visited; all
     * other inodes already carry a durable size at or beyond this epoch. */
    std::size_t keep = 0;
    for (std::size_t index = 0; index < dirtyInodes_.size(); ++index) {
        const std::uint64_t inodeId = dirtyInodes_[index];
        Inode &inode = inodes_[inodeId];
        if (inode.id != 0 && inode.logicalEpoch <= epoch) {
            inode.durableSize = inode.logicalSize;
            inode.sizeDirty = false;
        } else {
            dirtyInodes_[keep++] = inodeId;
        }
    }
    dirtyInodes_.resize(keep);
    preparedDurableSizes_.clear();
    return Result<void>(Status::Ok);
}

Result<void> Namespace::prepareDurableEpoch(std::uint64_t epoch) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (epoch > logicalEpoch_) {
        return makeError(Status::InvalidArgument,
                         "durable epoch is ahead of logical epoch");
    }
    if (epoch < durableEpoch_) {
        return makeError(Status::InvalidArgument,
                         "durable epoch cannot move backwards");
    }
    if (!preparedDurableSizes_.empty()) {
        return makeError(Status::Busy, "another durable epoch is being prepared");
    }
    preparedDurableSizes_.reserve(dirtyInodes_.size());
    for (const std::uint64_t inodeId : dirtyInodes_) {
        const Inode &inode = inodes_[inodeId];
        if (inode.id != 0) {
            preparedDurableSizes_.push_back(PreparedDurableSize{inodeId, inode.durableSize});
        }
    }
    /* Stage file sizes before checkpoint; the epoch itself publishes afterward. */
    for (const std::uint64_t inodeId : dirtyInodes_) {
        Inode &inode = inodes_[inodeId];
        if (inode.id != 0 && inode.logicalEpoch <= epoch) {
            inode.durableSize = inode.logicalSize;
        }
    }
    return Result<void>(Status::Ok);
}

/* Restores staged durable sizes after a failed checkpoint. */
void Namespace::rollbackPreparedDurableEpoch() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const PreparedDurableSize &prepared : preparedDurableSizes_) {
        if (prepared.inodeId < inodes_.size()) {
            inodes_[prepared.inodeId].durableSize = prepared.durableSize;
        }
    }
    preparedDurableSizes_.clear();
}

Result<void> Namespace::saveSnapshot(std::uint64_t txid, std::vector<std::byte> &out) {
    std::lock_guard<std::mutex> lock(mutex_);
    out.clear();
    appendSnapshotField(out, kNamespaceSnapshotMagic);
    appendSnapshotField(out, kNamespaceSnapshotVersion);
    appendSnapshotField(out, txid);
    appendSnapshotField(out, static_cast<std::uint64_t>(inodes_.size()));
    appendSnapshotField(out, static_cast<std::uint64_t>(freeSlots_.size()));
    appendSnapshotField(out, namespaceBytes_);
    appendSnapshotField(out, logicalEpoch_);
    appendSnapshotField(out, durableEpoch_);
    for (const Inode &inode : inodes_) {
        appendSnapshotField(out, inode.id);
        appendSnapshotField(out, inode.generation);
        appendSnapshotField(out, static_cast<std::uint8_t>(inode.directory ? 1 : 0));
        appendSnapshotField(out, inode.linkCount);
        appendSnapshotField(out, inode.logicalSize);
        appendSnapshotField(out, inode.durableSize);
        appendSnapshotField(out, inode.logicalEpoch);
        appendSnapshotField(out, inode.mtimeNs);
        appendSnapshotField(out, inode.ctimeNs);
        appendSnapshotField(out, inode.parent);
        appendSnapshotField(out, inode.childCount);
        const std::uint64_t entryCount =
            inode.directory ? static_cast<std::uint64_t>(inode.index->ordered.size()) : 0;
        appendSnapshotField(out, entryCount);
        if (inode.directory) {
            for (const auto &[name, child] : inode.index->ordered) {
                if (!appendSnapshotString(out, name)) {
                    return makeError(Status::NoMemory, "directory name is too large");
                }
                appendSnapshotField(out, child);
            }
        }
        const auto descriptors = inode.extents.snapshot();
        appendSnapshotField(out, static_cast<std::uint64_t>(descriptors.size()));
        for (const ExtentDescriptor &descriptor : descriptors) {
            appendSnapshotField(out, descriptor.logicalExtent);
            appendSnapshotField(out, descriptor.physicalExtent);
            appendSnapshotField(out, descriptor.count);
            appendSnapshotField(out, descriptor.deviceId);
        }
    }
    for (const std::uint64_t slot : freeSlots_) {
        appendSnapshotField(out, slot);
    }
    appendSnapshotField(out, static_cast<std::uint64_t>(allocators_.size()));
    for (const auto &allocator : allocators_) {
        const auto encoded = allocator->serialize();
        appendSnapshotField(out, static_cast<std::uint64_t>(encoded.size()));
        out.insert(out.end(), encoded.begin(), encoded.end());
    }
    snapshotTxid_ = txid;
    return Result<void>(Status::Ok);
}

Result<void> Namespace::loadSnapshot(std::span<const std::byte> data) {
    std::size_t cursor = 0;
    std::uint64_t magic = 0;
    std::uint32_t version = 0;
    std::uint64_t txid = 0;
    std::uint64_t inodeCount = 0;
    std::uint64_t freeCount = 0;
    std::uint64_t savedNamespaceBytes = 0;
    std::uint64_t savedLogicalEpoch = 0;
    std::uint64_t savedDurableEpoch = 0;
    if (!readSnapshotField(data, cursor, &magic) ||
        !readSnapshotField(data, cursor, &version) ||
        !readSnapshotField(data, cursor, &txid) ||
        !readSnapshotField(data, cursor, &inodeCount) ||
        !readSnapshotField(data, cursor, &freeCount) ||
        !readSnapshotField(data, cursor, &savedNamespaceBytes) ||
        !readSnapshotField(data, cursor, &savedLogicalEpoch) ||
        !readSnapshotField(data, cursor, &savedDurableEpoch) ||
        magic != kNamespaceSnapshotMagic || version != kNamespaceSnapshotVersion ||
        inodeCount <= kRootInodeId || inodeCount > config_.maxInodes + 1 ||
        freeCount > inodeCount) {
        return makeError(Status::Corrupt, "invalid namespace snapshot header");
    }

    std::vector<Inode> restored(inodeCount);
    for (std::uint64_t index = 0; index < inodeCount; ++index) {
        Inode &inode = restored[index];
        std::uint8_t directory = 0;
        std::uint64_t entryCount = 0;
        std::uint64_t extentCount = 0;
        if (!readSnapshotField(data, cursor, &inode.id) ||
            !readSnapshotField(data, cursor, &inode.generation) ||
            !readSnapshotField(data, cursor, &directory) ||
            !readSnapshotField(data, cursor, &inode.linkCount) ||
            !readSnapshotField(data, cursor, &inode.logicalSize) ||
            !readSnapshotField(data, cursor, &inode.durableSize) ||
            !readSnapshotField(data, cursor, &inode.logicalEpoch) ||
            !readSnapshotField(data, cursor, &inode.mtimeNs) ||
            !readSnapshotField(data, cursor, &inode.ctimeNs) ||
            !readSnapshotField(data, cursor, &inode.parent) ||
            !readSnapshotField(data, cursor, &inode.childCount) ||
            !readSnapshotField(data, cursor, &entryCount) ||
            directory > 1 || entryCount > config_.maxInodes) {
            return makeError(Status::Corrupt, "truncated inode snapshot");
        }
        inode.directory = directory != 0;
        inode.openCount = 0;
        inode.extents.configure(config_.extentBytes);
        if (inode.directory) {
            inode.index = std::make_shared<DirectoryIndex>();
            for (std::uint64_t entry = 0; entry < entryCount; ++entry) {
                std::string name;
                std::uint64_t child = 0;
                if (!readSnapshotString(data, cursor, &name) || name.empty() ||
                    name.size() > kMaxNameBytes || !readSnapshotField(data, cursor, &child) ||
                    child == 0 || child >= inodeCount ||
                    !inode.index->byName.emplace(name, child).second ||
                    !inode.index->ordered.emplace(name, child).second) {
                    return makeError(Status::Corrupt, "invalid directory snapshot");
                }
            }
        } else if (entryCount != 0) {
            return makeError(Status::Corrupt, "regular inode has directory entries");
        }
        if (!readSnapshotField(data, cursor, &extentCount) || extentCount > config_.maxInodes) {
            return makeError(Status::Corrupt, "invalid extent snapshot count");
        }
        std::vector<ExtentDescriptor> descriptors;
        descriptors.reserve(static_cast<std::size_t>(extentCount));
        for (std::uint64_t extent = 0; extent < extentCount; ++extent) {
            ExtentDescriptor descriptor;
            if (!readSnapshotField(data, cursor, &descriptor.logicalExtent) ||
                !readSnapshotField(data, cursor, &descriptor.physicalExtent) ||
                !readSnapshotField(data, cursor, &descriptor.count) ||
                !readSnapshotField(data, cursor, &descriptor.deviceId) ||
                descriptor.count == 0) {
                return makeError(Status::Corrupt, "truncated extent snapshot");
            }
            descriptors.push_back(descriptor);
        }
        auto restoredMap = inode.extents.restore(descriptors);
        if (!restoredMap.ok()) {
            return makeError(Status::Corrupt, "invalid inode extent map snapshot");
        }
        if (index == 0 && inode.id != 0) {
            return makeError(Status::Corrupt, "snapshot slot zero is occupied");
        }
        if (index != 0 && inode.id != 0 && inode.id != index) {
            return makeError(Status::Corrupt, "inode id does not match snapshot slot");
        }
    }

    std::vector<std::uint64_t> restoredFreeSlots;
    restoredFreeSlots.reserve(static_cast<std::size_t>(freeCount));
    std::unordered_set<std::uint64_t> freeSet;
    for (std::uint64_t i = 0; i < freeCount; ++i) {
        std::uint64_t slot = 0;
        if (!readSnapshotField(data, cursor, &slot) || slot == 0 || slot >= inodeCount ||
            restored[slot].id != 0 || !freeSet.insert(slot).second) {
            return makeError(Status::Corrupt, "invalid free inode slot snapshot");
        }
        restoredFreeSlots.push_back(slot);
    }

    std::uint64_t allocatorCount = 0;
    if (!readSnapshotField(data, cursor, &allocatorCount) ||
        allocatorCount != allocators_.size()) {
        return makeError(Status::Corrupt, "allocator count mismatch in namespace snapshot");
    }
    std::vector<std::unique_ptr<Allocator>> restoredAllocators;
    restoredAllocators.reserve(allocatorCount);
    for (std::size_t i = 0; i < allocatorCount; ++i) {
        std::uint64_t length = 0;
        if (!readSnapshotField(data, cursor, &length) || length > data.size() - cursor) {
            return makeError(Status::Corrupt, "truncated allocator snapshot");
        }
        const auto current = allocators_[i]->stats();
        AllocatorConfig allocatorConfig;
        allocatorConfig.deviceId = allocators_[i]->deviceId();
        allocatorConfig.totalExtents = current.total;
        allocatorConfig.capacityExtents = current.capacity;
        allocatorConfig.maxReserveExtents = config_.maxReserveExtents;
        auto allocator = Allocator::create(allocatorConfig);
        if (!allocator.ok() || !allocator.value()->deserialize(
                                  data.subspan(cursor, static_cast<std::size_t>(length))).ok()) {
            return makeError(Status::Corrupt, "invalid allocator snapshot");
        }
        restoredAllocators.push_back(std::move(allocator).value());
        cursor += static_cast<std::size_t>(length);
    }
    if (cursor != data.size() || restored[kRootInodeId].id != kRootInodeId ||
        !restored[kRootInodeId].directory) {
        return makeError(Status::Corrupt, "namespace snapshot has trailing or invalid root data");
    }

    std::lock_guard<std::mutex> lock(mutex_);
    inodes_ = std::move(restored);
    freeSlots_ = std::move(restoredFreeSlots);
    allocators_ = std::move(restoredAllocators);
    namespaceBytes_ = savedNamespaceBytes;
    logicalEpoch_ = savedLogicalEpoch;
    logicalEpochPublished_.store(logicalEpoch_, std::memory_order_release);
    durableEpoch_ = savedDurableEpoch;
    snapshotTxid_ = txid;
    preparedDurableSizes_.clear();
    dirtyInodes_.clear();
    /* Snapshots are mount-time only; refresh every per-inode data version so
     * no stale frontend cache can survive the restore. */
    for (Inode &inode : inodes_) {
        inode.sizeDirty = false;
        if (inode.id != 0) {
            publishDataVersionLocked(inode);
        }
    }
    stats_ = {};
    for (const Inode &inode : inodes_) {
        if (inode.id != 0) {
            ++stats_.inodes;
        }
    }
    stats_.namespaceBytes = namespaceBytes_;
    return Result<void>(Status::Ok);
}

std::uint64_t Namespace::snapshotTxid() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return snapshotTxid_;
}

std::uint64_t Namespace::deviceStartExtent(std::uint32_t deviceId) const noexcept {
    if (deviceId >= config_.deviceStartExtents.size()) {
        return 0;
    }
    return config_.deviceStartExtents[deviceId];
}

/* Sums allocator capacity without exposing metadata or reserved extents. */
std::uint64_t Namespace::freeDataBytes() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    std::uint64_t freeBytes = 0;
    for (const auto &allocator : allocators_) {
        const std::uint64_t freeExtents = allocator->allocatableExtents();
        if (freeExtents > std::numeric_limits<std::uint64_t>::max() /
                              config_.extentBytes) {
            return std::numeric_limits<std::uint64_t>::max();
        }
        const std::uint64_t bytes = freeExtents * config_.extentBytes;
        if (freeBytes > std::numeric_limits<std::uint64_t>::max() - bytes) {
            return std::numeric_limits<std::uint64_t>::max();
        }
        freeBytes += bytes;
    }
    return freeBytes;
}

Namespace::Stats Namespace::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    Stats stats = stats_;
    stats.namespaceBytes = namespaceBytes_;
    stats.orphanInodes = 0;
    for (const Inode &inode : inodes_) {
        if (inode.id != 0 && inode.id != kRootInodeId && inode.orphan()) {
            ++stats.orphanInodes;
        }
    }
    return stats;
}

Namespace::ExtentPoolStats Namespace::extentPoolStats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    ExtentPoolStats result;
    for (const auto &allocator : allocators_) {
        const Allocator::Stats stats = allocator->stats();
        result.total += stats.total;
        result.capacity += stats.capacity;
        result.free += stats.free;
        result.reserved += stats.reserved;
        result.initialized += stats.initialized;
        result.committed += stats.committed;
        result.freeRuns += stats.freeRuns;
    }
    return result;
}

} // namespace hfs::metadata
