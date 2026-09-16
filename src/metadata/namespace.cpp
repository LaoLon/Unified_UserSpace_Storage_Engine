#include "metadata/namespace.hpp"

#include <algorithm>
#include <chrono>

namespace hfs::metadata {

namespace {

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
    inode.id = 0; /* slot is free; generation is kept for the next reuse */
    inode.directory = false;
    inode.linkCount = 0;
    inode.openCount = 0;
    inode.logicalSize = 0;
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
                    inode.openCount, inode.logicalSize, inode.mtimeNs,    inode.ctimeNs};
}

Result<FileStat> Namespace::stat(std::string_view path) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto resolved = resolveEntry(path);
    if (!resolved.ok()) {
        return makeError<FileStat>(resolved.status(), resolved.message());
    }
    const Inode &inode = inodes_[resolved.value().inode];
    return FileStat{inode.id,        inode.generation, inode.directory, inode.linkCount,
                    inode.openCount, inode.logicalSize, inode.mtimeNs,    inode.ctimeNs};
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
    Inode &inode = *inodeResult.value();
    const std::uint64_t extentBytes = config_.extentBytes;
    const std::uint64_t logicalStart = offset / extentBytes;
    const std::uint64_t logicalEnd = (offset + length + extentBytes - 1) / extentBytes;
    const std::vector<ExtentDescriptor> descriptors = inode.extents.snapshot();
    std::vector<ExtentAllocation> allocations;
    std::vector<std::uint64_t> freeCounts(allocators_.size(), 0);

    auto rollback = [&]() {
        for (const ExtentAllocation &allocation : allocations) {
            allocators_[allocation.run.deviceId]->release(allocation.run.start,
                                                          allocation.run.count);
        }
        allocations.clear();
    };

    std::uint64_t cursor = logicalStart;
    std::size_t index = 0;
    while (cursor < logicalEnd) {
        while (index < descriptors.size() &&
               descriptors[index].logicalExtent + descriptors[index].count <= cursor) {
            ++index;
        }
        if (index < descriptors.size() && descriptors[index].logicalExtent <= cursor) {
            cursor = descriptors[index].logicalExtent + descriptors[index].count;
            continue;
        }
        std::uint64_t runEnd = logicalEnd;
        if (index < descriptors.size() && descriptors[index].logicalExtent < runEnd) {
            runEnd = descriptors[index].logicalExtent;
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
    for (const ExtentAllocation &allocation : allocations) {
        const std::uint64_t device = allocation.run.deviceId;
        if (device >= allocators_.size() || device >= config_.deviceStartExtents.size()) {
            return makeError(Status::InvalidArgument, "allocation device out of range");
        }
        auto initialized =
            allocators_[device]->markInitialized(allocation.run.start, allocation.run.count);
        if (!initialized.ok()) {
            return initialized;
        }
        auto committed = allocators_[device]->commit(allocation.run.start,
                                                     allocation.run.count);
        if (!committed.ok()) {
            return committed;
        }
        ExtentDescriptor descriptor;
        descriptor.logicalExtent = allocation.logicalExtent;
        descriptor.physicalExtent = config_.deviceStartExtents[device] + allocation.run.start;
        descriptor.count = allocation.run.count;
        descriptor.deviceId = static_cast<std::uint32_t>(device);
        descriptors.push_back(descriptor);
        stats_.allocatedExtents += allocation.run.count;
    }
    auto added = inode.extents.addExtents(descriptors);
    if (!added.ok()) {
        return added;
    }
    touch(inode, nowNs(), false);
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
        if (allocation.run.deviceId >= allocators_.size()) {
            return makeError(Status::InvalidArgument, "allocation device out of range");
        }
        auto released = allocators_[allocation.run.deviceId]->release(allocation.run.start,
                                                                      allocation.run.count);
        if (!released.ok()) {
            return released;
        }
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

Result<void> Namespace::truncateFile(FileHandle handle, std::uint64_t size) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto inodeResult = lookupInode(handle);
    if (!inodeResult.ok()) {
        return makeError(inodeResult.status(), inodeResult.message());
    }
    Inode &inode = *inodeResult.value();
    const std::uint64_t extentBytes = config_.extentBytes;
    const std::uint64_t keep = (size + extentBytes - 1) / extentBytes;
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
    }
    return Result<void>(Status::Ok);
}

std::uint64_t Namespace::deviceStartExtent(std::uint32_t deviceId) const noexcept {
    if (deviceId >= config_.deviceStartExtents.size()) {
        return 0;
    }
    return config_.deviceStartExtents[deviceId];
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

} // namespace hfs::metadata
