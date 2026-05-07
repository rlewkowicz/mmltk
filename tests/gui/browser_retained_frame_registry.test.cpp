#include "gui/browser_retained_frame_registry.h"
#include "gui/browser_dmabuf_fd.h"
#include "gui/browser_workspace_surface_bridge.h"
#include "support/catch2_compat.hpp"

#include <cstdint>
#include <cerrno>
#include <optional>
#include <stdexcept>
#include <string>

#include <fcntl.h>
#include <unistd.h>

namespace {

using namespace mmltk::browser;
using namespace mmltk::gui;

constexpr std::uint32_t kTestDrmFormatAbgr8888 = 0x34324241U;

template <typename T>
[[nodiscard]] const T& require_optional(const std::optional<T>& value) {
    assert(value.has_value());
    if (!value.has_value()) {
        throw std::runtime_error("expected optional value");
    }
    return *value;
}

[[nodiscard]] bool fd_is_open(const int fd) noexcept {
    errno = 0;
    return ::fcntl(fd, F_GETFD) >= 0 || errno != EBADF;
}

[[nodiscard]] FrameSlotDescriptor make_imported_slot(std::string imported_image_handle, std::string release_token,
                                                     const std::uint32_t slot_index = 5U) {
    FrameSlotDescriptor descriptor;
    descriptor.slot_name = slot_index == 5U ? "workspace" : "source";
    descriptor.transport = FrameTransportKind::CudaDeviceBuffer;
    descriptor.pixel_format = FramePixelFormat::Rgba8;
    descriptor.slot_index = slot_index;
    descriptor.frame_id = mmltk::live::LiveFrameId{101U + slot_index, 202U + slot_index};
    descriptor.capture_region = CaptureRegion{10U, 12U, 8U, 6U};
    descriptor.width = 8U;
    descriptor.height = 6U;
    descriptor.row_stride_bytes = 32U;
    descriptor.byte_length = frame_byte_length(descriptor.row_stride_bytes, descriptor.height);
    descriptor.ready_ns = 9001U + slot_index;
    descriptor.lifecycle = FrameSlotLifecycle::ExplicitRelease;
    descriptor.ownership = FrameSlotOwnership::NativeHost;
    apply_linux_importable_frame_slot_contract(descriptor,
                                               LinuxImportableFrameSlotContract{
                                                   LinuxImportableImageHandle{
                                                       LinuxImportableImageHandleKind::OpaqueToken,
                                                       std::string(kLinuxImportableImageHandleTypeNativeToken),
                                                       std::move(imported_image_handle),
                                                   },
                                                   FrameSlotReadySync{
                                                       FrameSlotSyncKind::TimelinePoint,
                                                       std::string("browser-frame:ready:") + std::to_string(slot_index),
                                                       descriptor.ready_ns,
                                                   },
                                                   std::move(release_token),
                                               });
    return descriptor;
}

[[nodiscard]] FrameSlotDescriptor make_workspace_surface_descriptor() {
    FrameSlotDescriptor descriptor =
        make_imported_slot("browser-frame:workspace:5:101:202:image", "browser-frame:workspace:5:101:202:release");
    LinuxImportableFrameSlotContract contract{};
    contract.ready_sync.kind = FrameSlotSyncKind::TimelinePoint;
    contract.ready_sync.value = descriptor.ready_ns;
    contract.metadata.emplace();
    contract.metadata->release_behavior = FrameSlotNativeImportReleaseBehavior::ContractRelease;
    apply_linux_importable_frame_slot_contract(descriptor, std::move(contract));
    return descriptor;
}

[[nodiscard]] FrameSlotDescriptor with_workspace_revision(FrameSlotDescriptor descriptor,
                                                          const std::uint64_t revision) {
    descriptor.frame_id.sequence = revision;
    descriptor.ready_ns = 10000U + revision;
    descriptor.ready_sync.value = descriptor.ready_ns;
    if (descriptor.linux_import.has_value()) {
        descriptor.linux_import->ready_sync.value = descriptor.ready_ns;
    }
    return descriptor;
}

[[nodiscard]] LinuxImportedFrameSourceContract make_imported_source_contract(
    const FrameSlotDescriptor& descriptor, const std::uint32_t image_fd, const int cuda_device_index,
    const std::uint32_t ready_handle, const std::uint32_t producer_stream_handle) {
    return LinuxImportedFrameSourceContract{
        LinuxImportedImageSource{
            LinuxImportedImageSourceKind::DmaBufRgba,
            image_fd,
            cuda_device_index,
            descriptor.width,
            descriptor.height,
            descriptor.row_stride_bytes,
            0U,
            descriptor.byte_length,
            kTestDrmFormatAbgr8888,
            0U,
            LinuxDmaBufModifierMode::Explicit,
        },
        LinuxImportedSyncSource{
            LinuxImportedSyncHandleKind::CudaEvent,
            ready_handle,
            descriptor.ready_sync.value,
        },
        LinuxImportedSyncSource{
            LinuxImportedSyncHandleKind::CudaStream,
            producer_stream_handle,
            0U,
        },
    };
}

[[nodiscard]] LinuxImportedFrameSourceContract make_imported_source_contract(const FrameSlotDescriptor& descriptor) {
    return make_imported_source_contract(descriptor, 0xCAFEU + descriptor.slot_index, 3,
                                         0xBEEFU + descriptor.slot_index, 0xDEADU + descriptor.slot_index);
}

[[nodiscard]] RetainedBrowserImportedFrameSource make_imported_source(const FrameSlotDescriptor& descriptor) {
    RetainedBrowserImportedFrameSource source;
    source.descriptor = descriptor;
    apply_linux_imported_frame_source_contract(source, make_imported_source_contract(descriptor));
    return source;
}

[[nodiscard]] mmltk::browser::WorkspaceSurfaceInfo make_workspace_surface_info(const FrameSlotDescriptor& descriptor) {
    WorkspaceSurfaceInfo surface_info;
    surface_info.surface_id = "workspace";
    surface_info.revision = std::to_string(descriptor.frame_id.sequence);
    surface_info.width = descriptor.width;
    surface_info.height = descriptor.height;
    surface_info.texture_format = "rgba8unorm";
    surface_info.opaque = true;
    surface_info.upright = true;
    return surface_info;
}

void assert_linux_imported_source_contract(const RetainedBrowserImportedFrameSource& source,
                                           const FrameSlotDescriptor& descriptor) {
    const auto& linux_import = require_optional(source.linux_import);
    assert(linux_import.image.kind == LinuxImportedImageSourceKind::DmaBufRgba);
    assert(linux_import.image.fd == 0xCAFEU + descriptor.slot_index);
    assert(linux_import.image.cuda_device_index == 3);
    assert(linux_import.image.width == descriptor.width);
    assert(linux_import.image.height == descriptor.height);
    assert(linux_import.image.stride_bytes == descriptor.row_stride_bytes);
    assert(linux_import.image.allocation_size == descriptor.byte_length);
    assert(linux_import.image.drm_format == kTestDrmFormatAbgr8888);
    assert(linux_import.image.modifier_mode == LinuxDmaBufModifierMode::Explicit);
    assert(linux_import.ready_sync.kind == LinuxImportedSyncHandleKind::CudaEvent);
    assert(linux_import.ready_sync.handle == 0xBEEFU + descriptor.slot_index);
    assert(linux_import.ready_sync.value == descriptor.ready_sync.value);
    assert(linux_import.producer_stream.kind == LinuxImportedSyncHandleKind::CudaStream);
    assert(linux_import.producer_stream.handle == 0xDEADU + descriptor.slot_index);
}

void assert_workspace_surface_descriptor_contract(const FrameSlotDescriptor& descriptor) {
    const auto& linux_import = require_optional(descriptor.linux_import);
    const auto metadata_value = frame_slot_native_import_metadata(descriptor);
    const auto& metadata = require_optional(metadata_value);
    assert(linux_import.image.handle.empty());
    assert(linux_import.image.handle_type.empty());
    assert(linux_import.release_token.empty());
    assert(linux_import.ready_sync.kind == FrameSlotSyncKind::TimelinePoint);
    assert(linux_import.ready_sync.handle.empty());
    assert(descriptor.ready_sync.kind == FrameSlotSyncKind::TimelinePoint);
    assert(descriptor.ready_sync.handle.empty());
    assert(metadata.release_behavior == FrameSlotNativeImportReleaseBehavior::ContractRelease);
    assert(metadata.texture_format == "rgba8unorm");
    assert(!frame_slot_supports_imported_acquire(descriptor));
}

void assert_linux_imported_lifecycle_contract(const RetainedBrowserImportedFrameSource& source) {
    const auto& linux_lifecycle = require_optional(source.linux_lifecycle);
    assert(linux_lifecycle.release_sync.kind == LinuxImportedSyncHandleKind::CudaEvent);
    assert(linux_lifecycle.release_sync.handle == 0xF00DU);
    assert(linux_lifecycle.release_sync.value == 77U);
}

class RetainedWorkspaceSurfaceFixture {
   public:
    RetainedWorkspaceSurfaceFixture() {
        registry.retain_workspace_surface(surface_info, make_imported_source(descriptor),
                                          [this]() { ++release_count; });
    }

    void assert_imported_source_contract() {
        const auto imported_surface =
            registry.imported_frame_source_for_surface(surface_info.surface_id, surface_info.revision);
        const auto& imported_source = require_optional(imported_surface);
        assert_workspace_surface_descriptor_contract(imported_source.descriptor);
        assert_linux_imported_source_contract(imported_source, descriptor);
    }

    RetainedBrowserFrameRegistry registry;
    FrameSlotDescriptor descriptor = make_workspace_surface_descriptor();
    WorkspaceSurfaceInfo surface_info = make_workspace_surface_info(descriptor);
    int release_count = 0;
};

void test_workspace_surface_release_runs_once_after_acquire() {
    RetainedWorkspaceSurfaceFixture fixture;
    fixture.assert_imported_source_contract();

    assert(fixture.release_count == 0);
    assert(fixture.registry.release_workspace_surface(fixture.surface_info.surface_id, fixture.surface_info.revision));
    assert(fixture.release_count == 1);
    const auto released_source = fixture.registry.imported_frame_source_for_surface(fixture.surface_info.surface_id,
                                                                                    fixture.surface_info.revision);
    assert(!released_source.has_value());
    assert(!fixture.registry.release_workspace_surface(fixture.surface_info.surface_id, fixture.surface_info.revision));

    fixture.registry.release_all();
    assert(fixture.release_count == 1);
}

void test_workspace_surface_contract_only_source_is_retained() {
    RetainedBrowserFrameRegistry registry;
    const FrameSlotDescriptor descriptor = make_workspace_surface_descriptor();
    const mmltk::browser::WorkspaceSurfaceInfo surface_info = make_workspace_surface_info(descriptor);

    RetainedBrowserImportedFrameSource source;
    source.descriptor = descriptor;
    source.linux_import = make_imported_source_contract(descriptor);

    registry.retain_workspace_surface(surface_info, source, []() {});

    const auto imported_surface =
        registry.imported_frame_source_for_surface(surface_info.surface_id, surface_info.revision);
    const auto& imported_source = require_optional(imported_surface);
    assert_workspace_surface_descriptor_contract(imported_source.descriptor);
    assert_linux_imported_source_contract(imported_source, descriptor);
}

void test_workspace_surface_explicit_source_contract_overrides_previous_contract() {
    RetainedBrowserFrameRegistry registry;
    const FrameSlotDescriptor descriptor = make_workspace_surface_descriptor();
    const mmltk::browser::WorkspaceSurfaceInfo surface_info = make_workspace_surface_info(descriptor);

    RetainedBrowserImportedFrameSource source = make_imported_source(descriptor);
    apply_linux_imported_frame_source_contract(source,
                                               make_imported_source_contract(descriptor, 0xABCDU, 5, 0x1234U, 0x5678U));

    registry.retain_workspace_surface(surface_info, source, []() {});

    const auto imported_surface =
        registry.imported_frame_source_for_surface(surface_info.surface_id, surface_info.revision);
    const auto& imported_source = require_optional(imported_surface);
    assert_workspace_surface_descriptor_contract(imported_source.descriptor);
    const auto& linux_import = require_optional(imported_source.linux_import);
    assert(linux_import.image.fd == 0xABCDU);
    assert(linux_import.image.cuda_device_index == 5);
    assert(linux_import.ready_sync.handle == 0x1234U);
    assert(linux_import.producer_stream.handle == 0x5678U);
}

void test_discard_unacquired_workspace_surface_releases_immediately() {
    RetainedBrowserFrameRegistry registry;
    const FrameSlotDescriptor descriptor = make_workspace_surface_descriptor();
    const WorkspaceSurfaceInfo surface_info = make_workspace_surface_info(descriptor);

    int release_count = 0;
    registry.retain_workspace_surface(surface_info, make_imported_source(descriptor),
                                      [&release_count]() { ++release_count; });

    assert(registry.discard_workspace_surface(surface_info));
    assert(release_count == 1);
    assert(!registry.discard_workspace_surface(surface_info));
}

void test_discard_acquired_workspace_surface_keeps_source_until_explicit_release() {
    RetainedBrowserFrameRegistry registry;
    const FrameSlotDescriptor descriptor = make_workspace_surface_descriptor();
    const mmltk::browser::WorkspaceSurfaceInfo surface_info = make_workspace_surface_info(descriptor);

    int release_count = 0;
    registry.retain_workspace_surface(surface_info, make_imported_source(descriptor),
                                      [&release_count]() { ++release_count; });

    const auto acquired_before_discard =
        registry.imported_frame_source_for_surface(surface_info.surface_id, surface_info.revision);
    assert(acquired_before_discard.has_value());
    assert(registry.discard_workspace_surface(surface_info));
    assert(release_count == 0);

    const auto acquired_after_discard =
        registry.imported_frame_source_for_surface(surface_info.surface_id, surface_info.revision);
    const auto& retained_source = require_optional(acquired_after_discard);
    assert_workspace_surface_descriptor_contract(retained_source.descriptor);
    assert_linux_imported_source_contract(retained_source, descriptor);

    assert(registry.release_workspace_surface(surface_info.surface_id, surface_info.revision));
    assert(release_count == 1);
    assert(!registry.imported_frame_source_for_surface(surface_info.surface_id, surface_info.revision).has_value());
}

void test_workspace_surface_preserves_lifecycle_contract() {
    RetainedBrowserFrameRegistry registry;
    const FrameSlotDescriptor descriptor = make_workspace_surface_descriptor();
    const mmltk::browser::WorkspaceSurfaceInfo surface_info = make_workspace_surface_info(descriptor);

    RetainedBrowserImportedFrameSource source = make_imported_source(descriptor);
    apply_linux_imported_frame_lifecycle_contract(source, LinuxImportedFrameLifecycleContract{
                                                              LinuxImportedSyncSource{
                                                                  LinuxImportedSyncHandleKind::CudaEvent,
                                                                  0xF00DU,
                                                                  77U,
                                                              },
                                                          });

    registry.retain_workspace_surface(surface_info, source, []() {});

    const auto imported_surface =
        registry.imported_frame_source_for_surface(surface_info.surface_id, surface_info.revision);
    const auto& imported_source = require_optional(imported_surface);
    assert_workspace_surface_descriptor_contract(imported_source.descriptor);
    assert_linux_imported_source_contract(imported_source, descriptor);
    assert_linux_imported_lifecycle_contract(imported_source);
}

void test_workspace_surface_release_by_revision_runs_once() {
    RetainedWorkspaceSurfaceFixture fixture;
    fixture.assert_imported_source_contract();

    assert(fixture.registry.release_workspace_surface(fixture.surface_info.surface_id, fixture.surface_info.revision));
    assert(fixture.release_count == 1);
    const auto released_source = fixture.registry.imported_frame_source_for_surface(fixture.surface_info.surface_id,
                                                                                    fixture.surface_info.revision);
    assert(!released_source.has_value());
}

void test_workspace_surface_rejects_invalid_lifecycle_contract() {
    RetainedBrowserFrameRegistry registry;
    const FrameSlotDescriptor descriptor = make_workspace_surface_descriptor();
    const WorkspaceSurfaceInfo surface_info = make_workspace_surface_info(descriptor);

    RetainedBrowserImportedFrameSource source = make_imported_source(descriptor);
    apply_linux_imported_frame_lifecycle_contract(source, LinuxImportedFrameLifecycleContract{
                                                              LinuxImportedSyncSource{
                                                                  LinuxImportedSyncHandleKind::None,
                                                                  0xF00DU,
                                                                  0U,
                                                              },
                                                          });

    bool lifecycle_threw = false;
    try {
        registry.retain_workspace_surface(surface_info, source, []() {});
    } catch (const std::runtime_error&) {
        lifecycle_threw = true;
    }
    assert(lifecycle_threw);
}

void test_workspace_surface_rejects_legacy_cuda_pointer_metadata() {
    RetainedBrowserFrameRegistry registry;
    const FrameSlotDescriptor descriptor = make_workspace_surface_descriptor();
    const WorkspaceSurfaceInfo surface_info = make_workspace_surface_info(descriptor);

    RetainedBrowserImportedFrameSource source = make_imported_source(descriptor);
    source.linux_import->image.kind = LinuxImportedImageSourceKind::CudaDevicePointer;

    bool rejected = false;
    try {
        registry.retain_workspace_surface(surface_info, source, []() {});
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    assert(rejected);
}

void test_workspace_surface_rejects_unknown_modifier_mode() {
    RetainedBrowserFrameRegistry registry;
    const FrameSlotDescriptor descriptor = make_workspace_surface_descriptor();
    const WorkspaceSurfaceInfo surface_info = make_workspace_surface_info(descriptor);

    RetainedBrowserImportedFrameSource source = make_imported_source(descriptor);
    source.linux_import->image.modifier_mode = LinuxDmaBufModifierMode::Unknown;

    bool rejected = false;
    try {
        registry.retain_workspace_surface(surface_info, source, []() {});
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    assert(rejected);
}

void test_workspace_surface_retains_owned_dmabuf_fd_until_release() {
    int pipe_fds[2] = {-1, -1};
    assert(::pipe(pipe_fds) == 0);

    const int retained_fd = pipe_fds[0];
    const int write_fd = pipe_fds[1];
    {
        RetainedBrowserFrameRegistry registry;
        const FrameSlotDescriptor descriptor = make_workspace_surface_descriptor();
        const WorkspaceSurfaceInfo surface_info = make_workspace_surface_info(descriptor);
        RetainedBrowserImportedFrameSource source = make_imported_source(descriptor);
        source.linux_import->image.fd = static_cast<std::uintptr_t>(retained_fd);
        source.owned_image_fd = std::make_shared<RetainedLinuxImageFd>(retained_fd);

        registry.retain_workspace_surface(surface_info, source, []() {});
        source.owned_image_fd.reset();
        assert(fd_is_open(retained_fd));

        assert(registry.release_workspace_surface(surface_info.surface_id, surface_info.revision));
        assert(!fd_is_open(retained_fd));
    }
    (void)::close(write_fd);
}

void test_workspace_surface_duplicate_fd_is_owned_independently() {
    int pipe_fds[2] = {-1, -1};
    assert(::pipe(pipe_fds) == 0);

    const int original_fd = pipe_fds[0];
    const int write_fd = pipe_fds[1];
    const int duplicate_fd =
        duplicate_workspace_dmabuf_fd(original_fd, "fcntl(F_DUPFD_CLOEXEC) for retained workspace test");
    assert(duplicate_fd != original_fd);
    const int duplicate_flags = ::fcntl(duplicate_fd, F_GETFD);
    assert(duplicate_flags >= 0 && (duplicate_flags & FD_CLOEXEC) != 0);
    assert(::close(original_fd) == 0);
    assert(!fd_is_open(original_fd));
    assert(fd_is_open(duplicate_fd));

    {
        RetainedBrowserFrameRegistry registry;
        const FrameSlotDescriptor descriptor = make_workspace_surface_descriptor();
        const WorkspaceSurfaceInfo surface_info = make_workspace_surface_info(descriptor);
        RetainedBrowserImportedFrameSource source = make_imported_source(descriptor);
        source.linux_import->image.fd = static_cast<std::uintptr_t>(duplicate_fd);
        source.owned_image_fd = std::make_shared<RetainedLinuxImageFd>(duplicate_fd);

        registry.retain_workspace_surface(surface_info, source, []() {});
        source.owned_image_fd.reset();
        assert(fd_is_open(duplicate_fd));

        assert(registry.release_workspace_surface(surface_info.surface_id, surface_info.revision));
        assert(!fd_is_open(duplicate_fd));
    }
    (void)::close(write_fd);
}

void test_retained_workspace_surface_source_helper_preserves_zero_copy_contract() {
    int pipe_fds[2] = {-1, -1};
    assert(::pipe(pipe_fds) == 0);

    const int original_fd = pipe_fds[0];
    const int write_fd = pipe_fds[1];
    const FrameSlotDescriptor descriptor = make_workspace_surface_descriptor();
    mmltk::live::WorkspaceOutputBundle bundle;
    bundle.slot_index = descriptor.slot_index;
    bundle.frame_id = descriptor.frame_id;
    bundle.dims = mmltk::live::WorkspaceDimensions{descriptor.width, descriptor.height, descriptor.row_stride_bytes};
    bundle.ready_event = reinterpret_cast<cudaEvent_t>(0xBEEFU);
    bundle.stream = reinterpret_cast<cudaStream_t>(0xDEADU);
    bundle.region = mmltk::live::LiveCaptureRegion{0U, 0U, descriptor.width, descriptor.height};
    bundle.ready_ns = descriptor.ready_sync.value;
    bundle.dmabuf_image = mmltk::live::WorkspaceDmaBufImage{
        original_fd,
        descriptor.width,
        descriptor.height,
        descriptor.row_stride_bytes,
        0U,
        descriptor.byte_length,
        kTestDrmFormatAbgr8888,
        0U,
        mmltk::live::DmaBufModifierMode::Explicit,
    };

    RetainedBrowserImportedFrameSource source = make_retained_workspace_surface_source(descriptor, bundle, 4);
    assert(source.owned_image_fd != nullptr);
    const auto& linux_import = require_optional(source.linux_import);
    assert(linux_import.image.kind == LinuxImportedImageSourceKind::DmaBufRgba);
    assert(linux_import.image.fd != static_cast<std::uintptr_t>(original_fd));
    assert(linux_import.image.cuda_device_index == 4);
    assert(linux_import.image.width == descriptor.width);
    assert(linux_import.image.height == descriptor.height);
    assert(linux_import.image.stride_bytes == descriptor.row_stride_bytes);
    assert(linux_import.image.drm_format == kTestDrmFormatAbgr8888);
    assert(linux_import.image.modifier_mode == LinuxDmaBufModifierMode::Explicit);
    assert(linux_import.ready_sync.kind == LinuxImportedSyncHandleKind::CudaEvent);
    assert(linux_import.ready_sync.handle == reinterpret_cast<std::uintptr_t>(bundle.ready_event));
    assert(linux_import.producer_stream.kind == LinuxImportedSyncHandleKind::CudaStream);
    assert(linux_import.producer_stream.handle == reinterpret_cast<std::uintptr_t>(bundle.stream));

    assert(::close(original_fd) == 0);
    assert(fd_is_open(static_cast<int>(linux_import.image.fd)));
    source.owned_image_fd.reset();
    assert(!fd_is_open(static_cast<int>(linux_import.image.fd)));
    (void)::close(write_fd);
}

void test_workspace_surface_bridge_live_revision_import_release_reuse() {
    BrowserWorkspaceSurfaceBridge bridge;
    FrameSlotDescriptor first_descriptor = with_workspace_revision(make_workspace_surface_descriptor(), 501U);
    const mmltk::live::LiveFrameId first_live_frame_id{91U, first_descriptor.frame_id.sequence};

    int release_count = 0;
    bridge.publish(
        BrowserWorkspaceSurfaceOwner::Live, make_imported_source(first_descriptor),
        [&release_count]() { ++release_count; }, first_live_frame_id);

    const auto first_surface = bridge.acquire_current_surface("workspace");
    const auto& first_surface_info = require_optional(first_surface);
    assert(first_surface_info.revision == "501");
    const auto first_imported_source =
        bridge.acquire_surface_source(first_surface_info.surface_id, first_surface_info.revision);
    const auto& first_imported = require_optional(first_imported_source);
    assert_linux_imported_source_contract(first_imported, first_descriptor);
    assert(bridge.preview().live_frame_id == first_live_frame_id);

    assert(bridge.release_surface(first_surface_info.surface_id, first_surface_info.revision));
    assert(release_count == 1);
    assert(!bridge.acquire_current_surface("workspace").has_value());
    assert(!bridge.acquire_surface_source(first_surface_info.surface_id, first_surface_info.revision).has_value());
    assert(bridge.preview().owner == BrowserWorkspaceSurfaceOwner::None);
    assert(bridge.preview().last_error.empty());

    FrameSlotDescriptor second_descriptor = with_workspace_revision(make_workspace_surface_descriptor(), 502U);
    const mmltk::live::LiveFrameId second_live_frame_id{91U, second_descriptor.frame_id.sequence};
    bridge.publish(
        BrowserWorkspaceSurfaceOwner::Live, make_imported_source(second_descriptor),
        [&release_count]() { ++release_count; }, second_live_frame_id);

    const auto second_surface = bridge.acquire_current_surface("workspace");
    const auto& second_surface_info = require_optional(second_surface);
    assert(second_surface_info.revision == "502");
    const auto second_imported_source =
        bridge.acquire_surface_source(second_surface_info.surface_id, second_surface_info.revision);
    const auto& second_imported = require_optional(second_imported_source);
    assert_linux_imported_source_contract(second_imported, second_descriptor);
    assert(bridge.preview().live_frame_id == second_live_frame_id);

    assert(bridge.release_surface(second_surface_info.surface_id, second_surface_info.revision));
    assert(release_count == 2);
    assert(!bridge.acquire_current_surface("workspace").has_value());
    assert(!bridge.acquire_surface_source(second_surface_info.surface_id, second_surface_info.revision).has_value());
    assert(bridge.preview().owner == BrowserWorkspaceSurfaceOwner::None);
    assert(bridge.preview().last_error.empty());
}

void test_release_all_releases_each_retained_surface_once() {
    RetainedBrowserFrameRegistry registry;
    const FrameSlotDescriptor workspace_descriptor = make_workspace_surface_descriptor();
    const WorkspaceSurfaceInfo workspace_surface_info = make_workspace_surface_info(workspace_descriptor);
    const FrameSlotDescriptor next_workspace_descriptor =
        with_workspace_revision(make_workspace_surface_descriptor(), 303U);
    const WorkspaceSurfaceInfo next_workspace_surface_info = make_workspace_surface_info(next_workspace_descriptor);

    int workspace_release_count = 0;
    int next_workspace_release_count = 0;
    registry.retain_workspace_surface(workspace_surface_info, make_imported_source(workspace_descriptor),
                                      [&workspace_release_count]() { ++workspace_release_count; });
    registry.retain_workspace_surface(next_workspace_surface_info, make_imported_source(next_workspace_descriptor),
                                      [&next_workspace_release_count]() { ++next_workspace_release_count; });

    registry.release_all();

    assert(workspace_release_count == 1);
    assert(next_workspace_release_count == 1);
    assert(
        !registry.imported_frame_source_for_surface(workspace_surface_info.surface_id, workspace_surface_info.revision)
             .has_value());
    assert(!registry
                .imported_frame_source_for_surface(next_workspace_surface_info.surface_id,
                                                   next_workspace_surface_info.revision)
                .has_value());

    registry.release_all();
    assert(workspace_release_count == 1);
    assert(next_workspace_release_count == 1);
}

void test_registry_destructor_releases_remaining_surfaces_once() {
    int workspace_release_count = 0;
    int next_workspace_release_count = 0;

    {
        RetainedBrowserFrameRegistry registry;
        const FrameSlotDescriptor workspace_descriptor = make_workspace_surface_descriptor();
        const WorkspaceSurfaceInfo workspace_surface_info = make_workspace_surface_info(workspace_descriptor);
        const FrameSlotDescriptor next_workspace_descriptor =
            with_workspace_revision(make_workspace_surface_descriptor(), 404U);
        const WorkspaceSurfaceInfo next_workspace_surface_info = make_workspace_surface_info(next_workspace_descriptor);

        registry.retain_workspace_surface(workspace_surface_info, make_imported_source(workspace_descriptor),
                                          [&workspace_release_count]() { ++workspace_release_count; });
        registry.retain_workspace_surface(next_workspace_surface_info, make_imported_source(next_workspace_descriptor),
                                          [&next_workspace_release_count]() { ++next_workspace_release_count; });
    }

    assert(workspace_release_count == 1);
    assert(next_workspace_release_count == 1);
}

void test_retain_rejects_workspace_surface_revision_collisions() {
    RetainedBrowserFrameRegistry registry;
    const FrameSlotDescriptor workspace_descriptor = make_workspace_surface_descriptor();
    const WorkspaceSurfaceInfo workspace_surface_info = make_workspace_surface_info(workspace_descriptor);
    FrameSlotDescriptor colliding_descriptor = workspace_descriptor;
    apply_linux_importable_frame_slot_contract(colliding_descriptor,
                                               LinuxImportableFrameSlotContract{
                                                   LinuxImportableImageHandle{
                                                       LinuxImportableImageHandleKind::OpaqueToken,
                                                       std::string(kLinuxImportableImageHandleTypeNativeToken),
                                                       "browser-frame:workspace:5:101:202:image:duplicate",
                                                   },
                                                   FrameSlotReadySync{
                                                       FrameSlotSyncKind::TimelinePoint,
                                                       "browser-frame:workspace:5:101:202:ready:duplicate",
                                                       colliding_descriptor.ready_ns,
                                                   },
                                                   "browser-frame:workspace:5:101:202:release:duplicate",
                                               });
    const WorkspaceSurfaceInfo colliding_surface_info = make_workspace_surface_info(colliding_descriptor);

    registry.retain_workspace_surface(workspace_surface_info, make_imported_source(workspace_descriptor), []() {});

    bool collision_threw = false;
    try {
        registry.retain_workspace_surface(colliding_surface_info, make_imported_source(colliding_descriptor), []() {});
    } catch (const std::runtime_error&) {
        collision_threw = true;
    }
    assert(collision_threw);
}

}  // namespace

MMLTK_REGISTER_TEST_CASE("[gui][browser_retained_frame_registry]",
                         test_workspace_surface_release_runs_once_after_acquire);
MMLTK_REGISTER_TEST_CASE("[gui][browser_retained_frame_registry]",
                         test_workspace_surface_contract_only_source_is_retained);
MMLTK_REGISTER_TEST_CASE("[gui][browser_retained_frame_registry]",
                         test_workspace_surface_explicit_source_contract_overrides_previous_contract);
MMLTK_REGISTER_TEST_CASE("[gui][browser_retained_frame_registry]",
                         test_discard_unacquired_workspace_surface_releases_immediately);
MMLTK_REGISTER_TEST_CASE("[gui][browser_retained_frame_registry]",
                         test_discard_acquired_workspace_surface_keeps_source_until_explicit_release);
MMLTK_REGISTER_TEST_CASE("[gui][browser_retained_frame_registry]", test_workspace_surface_preserves_lifecycle_contract);
MMLTK_REGISTER_TEST_CASE("[gui][browser_retained_frame_registry]",
                         test_workspace_surface_release_by_revision_runs_once);
MMLTK_REGISTER_TEST_CASE("[gui][browser_retained_frame_registry]",
                         test_workspace_surface_rejects_invalid_lifecycle_contract);
MMLTK_REGISTER_TEST_CASE("[gui][browser_retained_frame_registry]",
                         test_workspace_surface_rejects_legacy_cuda_pointer_metadata);
MMLTK_REGISTER_TEST_CASE("[gui][browser_retained_frame_registry]",
                         test_workspace_surface_rejects_unknown_modifier_mode);
MMLTK_REGISTER_TEST_CASE("[gui][browser_retained_frame_registry]",
                         test_workspace_surface_retains_owned_dmabuf_fd_until_release);
MMLTK_REGISTER_TEST_CASE("[gui][browser_retained_frame_registry]",
                         test_workspace_surface_duplicate_fd_is_owned_independently);
MMLTK_REGISTER_TEST_CASE("[gui][browser_retained_frame_registry]",
                         test_retained_workspace_surface_source_helper_preserves_zero_copy_contract);
MMLTK_REGISTER_TEST_CASE("[gui][browser_retained_frame_registry]",
                         test_workspace_surface_bridge_live_revision_import_release_reuse);
MMLTK_REGISTER_TEST_CASE("[gui][browser_retained_frame_registry]",
                         test_release_all_releases_each_retained_surface_once);
MMLTK_REGISTER_TEST_CASE("[gui][browser_retained_frame_registry]",
                         test_registry_destructor_releases_remaining_surfaces_once);
MMLTK_REGISTER_TEST_CASE("[gui][browser_retained_frame_registry]",
                         test_retain_rejects_workspace_surface_revision_collisions);
