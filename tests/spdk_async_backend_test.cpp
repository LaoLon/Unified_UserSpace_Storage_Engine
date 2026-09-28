#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "core/spdk_async_backend.hpp"
#include "runtime/spdk_runtime.hpp"
#include "test_support.hpp"

namespace {

/* Builds the planner-shaped request used by the conversion checks. */
hfs::AsyncDataBackend::Request makeRequest(std::byte *buffer, std::size_t bufferBytes) {
    hfs::AsyncDataBackend::Request request;
    request.physicalBlock = 3;
    request.deviceId = 0;
    request.bytes = 4096;
    request.spanBytes = 4096;
    request.buffer = buffer;
    request.bufferBytes = bufferBytes;
    return request;
}

} // namespace

int main() {
    std::array<std::byte, 16384> buffer{};
    hfs::SpdkAsyncDataBackend::NativeRequest native;
    auto request = makeRequest(buffer.data(), buffer.size());

    HFS_TEST_CHECK(hfs::SpdkAsyncDataBackend::convertPlannerRequest(
        request, 4096, 512, 1U << 20, native));
    HFS_TEST_CHECK(native.offsetBlocks == 24);
    HFS_TEST_CHECK(native.blocks == 8);
    HFS_TEST_CHECK(native.spanBytes == 4096);

    HFS_TEST_CHECK(hfs::SpdkAsyncDataBackend::convertPlannerRequest(
        request, 4096, 4096, 1U << 20, native));
    HFS_TEST_CHECK(native.offsetBlocks == 3);
    HFS_TEST_CHECK(native.blocks == 1);

    request.spanBytes = 8192;
    request.bytes = 4096;
    request.bufferBytes = buffer.size();
    HFS_TEST_CHECK(hfs::SpdkAsyncDataBackend::convertPlannerRequest(
        request, 4096, 4096, 1U << 20, native));
    HFS_TEST_CHECK(native.offsetBlocks == 3);
    HFS_TEST_CHECK(native.blocks == 2);

    /* Native-sector reads carry an explicit byte address and may be 512-byte
     * aligned without being a multiple of the 4 KiB planner block. */
    request = makeRequest(buffer.data(), buffer.size());
    request.physicalByteValid = true;
    request.physicalByte = 4608;
    request.bytes = 4608;
    request.spanBytes = 4608;
    HFS_TEST_CHECK(hfs::SpdkAsyncDataBackend::convertPlannerRequest(
        request, 4096, 512, 1U << 20, native));
    HFS_TEST_CHECK(native.offsetBlocks == 9);
    HFS_TEST_CHECK(native.blocks == 9);

    request.physicalByte = 4609;
    HFS_TEST_CHECK(!hfs::SpdkAsyncDataBackend::convertPlannerRequest(
        request, 4096, 512, 1U << 20, native));

    request = makeRequest(buffer.data(), buffer.size());
    request.spanBytes = 4096;
    request.bytes = 1024;
    request.spanOffset = 2048;
    HFS_TEST_CHECK(hfs::SpdkAsyncDataBackend::convertPlannerRequest(
        request, 4096, 4096, 1U << 20, native));
    request.spanOffset = 4097;
    HFS_TEST_CHECK(!hfs::SpdkAsyncDataBackend::convertPlannerRequest(
        request, 4096, 4096, 1U << 20, native));

    request = makeRequest(buffer.data(), buffer.size());
    request.physicalBlock = 256;
    HFS_TEST_CHECK(!hfs::SpdkAsyncDataBackend::convertPlannerRequest(
        request, 4096, 4096, 1U << 20, native));
    HFS_TEST_CHECK(!hfs::SpdkAsyncDataBackend::convertPlannerRequest(
        request, 4096, 8192, 1U << 20, native));
    request.bufferBytes = 1024;
    HFS_TEST_CHECK(!hfs::SpdkAsyncDataBackend::convertPlannerRequest(
        request, 4096, 512, 1U << 20, native));

    hfs::SpdkRuntime &runtime = hfs::SpdkRuntime::instance();
    hfs::SpdkAsyncDataBackend backend(runtime, 0, 4096, 2);
    HFS_TEST_CHECK(backend.capacity() == 2);
    HFS_TEST_CHECK(backend.inFlight() == 0);
    /* Without an active SPDK device set the backend stays invalid and keeps no
     * free list; with a live device every slot must be free at construction. */
    HFS_TEST_CHECK(backend.freeSlots() == (backend.valid() ? 2 : 0));

    /* Without an active SPDK reactor, submission must reject without leasing
     * a slot; on a live system this also exercises the DMA validation edge. */
    request = makeRequest(buffer.data(), buffer.size());
    const auto rejected = backend.submitRead(request, nullptr, nullptr);
    HFS_TEST_CHECK(rejected.disposition == hfs::AsyncDataBackend::SubmitDisposition::Rejected);
    HFS_TEST_CHECK(backend.inFlight() == 0);
    HFS_TEST_CHECK(backend.freeSlots() == (backend.valid() ? 2 : 0));

    hfs::DataIoRequest ioRequest;
    auto wrongThread = runtime.submitDataReadOnReactor(
        0, 0, ioRequest, buffer.data(), 0, 1, nullptr, nullptr);
    HFS_TEST_CHECK(!wrongThread.ok());
    HFS_TEST_CHECK(wrongThread.status() == hfs::Status::NotSupported ||
                   wrongThread.status() == hfs::Status::InvalidState ||
                   wrongThread.status() == hfs::Status::Busy);

    std::printf("spdk_async_backend_test: ok\n");
    return 0;
}
