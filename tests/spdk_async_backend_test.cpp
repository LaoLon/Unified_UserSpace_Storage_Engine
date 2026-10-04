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
uuse::AsyncDataBackend::Request makeRequest(std::byte *buffer, std::size_t bufferBytes) {
    uuse::AsyncDataBackend::Request request;
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
    uuse::SpdkAsyncDataBackend::NativeRequest native;
    auto request = makeRequest(buffer.data(), buffer.size());

    UUSE_TEST_CHECK(uuse::SpdkAsyncDataBackend::convertPlannerRequest(
        request, 4096, 512, 1U << 20, native));
    UUSE_TEST_CHECK(native.offsetBlocks == 24);
    UUSE_TEST_CHECK(native.blocks == 8);
    UUSE_TEST_CHECK(native.spanBytes == 4096);

    UUSE_TEST_CHECK(uuse::SpdkAsyncDataBackend::convertPlannerRequest(
        request, 4096, 4096, 1U << 20, native));
    UUSE_TEST_CHECK(native.offsetBlocks == 3);
    UUSE_TEST_CHECK(native.blocks == 1);

    request.spanBytes = 8192;
    request.bytes = 4096;
    request.bufferBytes = buffer.size();
    UUSE_TEST_CHECK(uuse::SpdkAsyncDataBackend::convertPlannerRequest(
        request, 4096, 4096, 1U << 20, native));
    UUSE_TEST_CHECK(native.offsetBlocks == 3);
    UUSE_TEST_CHECK(native.blocks == 2);

    /* Native-sector reads carry an explicit byte address and may be 512-byte
     * aligned without being a multiple of the 4 KiB planner block. */
    request = makeRequest(buffer.data(), buffer.size());
    request.physicalByteValid = true;
    request.physicalByte = 4608;
    request.bytes = 4608;
    request.spanBytes = 4608;
    UUSE_TEST_CHECK(uuse::SpdkAsyncDataBackend::convertPlannerRequest(
        request, 4096, 512, 1U << 20, native));
    UUSE_TEST_CHECK(native.offsetBlocks == 9);
    UUSE_TEST_CHECK(native.blocks == 9);

    request.physicalByte = 4609;
    UUSE_TEST_CHECK(!uuse::SpdkAsyncDataBackend::convertPlannerRequest(
        request, 4096, 512, 1U << 20, native));

    request = makeRequest(buffer.data(), buffer.size());
    request.spanBytes = 4096;
    request.bytes = 1024;
    request.spanOffset = 2048;
    UUSE_TEST_CHECK(uuse::SpdkAsyncDataBackend::convertPlannerRequest(
        request, 4096, 4096, 1U << 20, native));
    request.spanOffset = 4097;
    UUSE_TEST_CHECK(!uuse::SpdkAsyncDataBackend::convertPlannerRequest(
        request, 4096, 4096, 1U << 20, native));

    request = makeRequest(buffer.data(), buffer.size());
    request.physicalBlock = 256;
    UUSE_TEST_CHECK(!uuse::SpdkAsyncDataBackend::convertPlannerRequest(
        request, 4096, 4096, 1U << 20, native));
    UUSE_TEST_CHECK(!uuse::SpdkAsyncDataBackend::convertPlannerRequest(
        request, 4096, 8192, 1U << 20, native));
    request.bufferBytes = 1024;
    UUSE_TEST_CHECK(!uuse::SpdkAsyncDataBackend::convertPlannerRequest(
        request, 4096, 512, 1U << 20, native));

    uuse::SpdkRuntime &runtime = uuse::SpdkRuntime::instance();
    uuse::SpdkAsyncDataBackend backend(runtime, 0, 4096, 2);
    UUSE_TEST_CHECK(backend.capacity() == 2);
    UUSE_TEST_CHECK(backend.inFlight() == 0);
    /* Without an active SPDK device set the backend stays invalid and keeps no
     * free list; with a live device every slot must be free at construction. */
    UUSE_TEST_CHECK(backend.freeSlots() == (backend.valid() ? 2 : 0));

    /* Without an active SPDK reactor, submission must reject without leasing
     * a slot; on a live system this also exercises the DMA validation edge. */
    request = makeRequest(buffer.data(), buffer.size());
    const auto rejected = backend.submitRead(request, nullptr, nullptr);
    UUSE_TEST_CHECK(rejected.disposition == uuse::AsyncDataBackend::SubmitDisposition::Rejected);
    UUSE_TEST_CHECK(backend.inFlight() == 0);
    UUSE_TEST_CHECK(backend.freeSlots() == (backend.valid() ? 2 : 0));

    uuse::DataIoRequest ioRequest;
    auto wrongThread = runtime.submitDataReadOnReactor(
        0, 0, ioRequest, buffer.data(), 0, 1, nullptr, nullptr);
    UUSE_TEST_CHECK(!wrongThread.ok());
    UUSE_TEST_CHECK(wrongThread.status() == uuse::Status::NotSupported ||
                   wrongThread.status() == uuse::Status::InvalidState ||
                   wrongThread.status() == uuse::Status::Busy);

    std::printf("spdk_async_backend_test: ok\n");
    return 0;
}
