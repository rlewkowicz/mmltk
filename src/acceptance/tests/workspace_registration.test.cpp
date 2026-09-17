#include "third_party/firefox/dom/webgpu/ipc/WorkspaceSlotReleaseRegistration.h"
#include <catch2/catch_test_macros.hpp>
namespace {
using mozilla::webgpu::WorkspaceSlotReleaseRegistrationOwner;
void test_workspace_slot_release_registration_shares_one_window_listener_until_last_device() {
    WorkspaceSlotReleaseRegistrationOwner registrations;
    const auto first = registrations.Register(1U, 11U);
    REQUIRE((first.attach_window == 11U));
    REQUIRE((!first.detach_window));
    const auto second = registrations.Register(2U, 11U);
    REQUIRE((!second.attach_window));
    REQUIRE((!second.detach_window));
    REQUIRE((registrations.DeviceCount(11U) == 2U));
    REQUIRE((registrations.WindowCount() == 1U));
    REQUIRE((!registrations.Unregister(1U)));
    REQUIRE((registrations.DeviceCount(11U) == 1U));
    REQUIRE((registrations.Unregister(2U) == 11U));
    REQUIRE((registrations.DeviceCount() == 0U));
    REQUIRE((registrations.WindowCount() == 0U));
}
void test_workspace_slot_release_registration_tracks_windows_independently() {
    WorkspaceSlotReleaseRegistrationOwner registrations;
    REQUIRE((registrations.Register(1U, 11U).attach_window == 11U));
    REQUIRE((registrations.Register(2U, 22U).attach_window == 22U));
    REQUIRE((registrations.WindowCount() == 2U));
    REQUIRE((registrations.Unregister(1U) == 11U));
    REQUIRE((registrations.WindowCount() == 1U));
    REQUIRE((registrations.DeviceCount(22U) == 1U));
    REQUIRE((registrations.Unregister(2U) == 22U));
}
void test_workspace_slot_release_registration_tolerates_duplicate_and_reordered_unregister() {
    WorkspaceSlotReleaseRegistrationOwner registrations;
    REQUIRE((registrations.Register(1U, 11U).attach_window == 11U));
    const auto duplicate = registrations.Register(1U, 11U);
    REQUIRE((!duplicate.attach_window));
    REQUIRE((!duplicate.detach_window));
    REQUIRE((registrations.DeviceCount(11U) == 1U));
    const auto moved = registrations.Register(1U, 22U);
    REQUIRE((moved.detach_window == 11U));
    REQUIRE((moved.attach_window == 22U));
    REQUIRE((registrations.DeviceCount(11U) == 0U));
    REQUIRE((registrations.DeviceCount(22U) == 1U));
    REQUIRE((registrations.Unregister(1U) == 22U));
    REQUIRE((!registrations.Unregister(1U)));
}
void test_workspace_slot_release_registration_handles_window_death_and_system_teardown() {
    WorkspaceSlotReleaseRegistrationOwner registrations;
    REQUIRE((registrations.Register(1U, 11U).attach_window == 11U));
    REQUIRE((!registrations.Register(2U, 11U).attach_window));
    REQUIRE((registrations.Register(3U, 22U).attach_window == 22U));
    REQUIRE((registrations.RemoveWindow(11U)));
    REQUIRE((!registrations.RemoveWindow(11U)));
    REQUIRE((!registrations.Unregister(1U)));
    REQUIRE((!registrations.Unregister(2U)));
    REQUIRE((registrations.DeviceCount() == 1U));
    REQUIRE((registrations.WindowCount() == 1U));
    registrations.Clear();
    registrations.Clear();
    REQUIRE((!registrations.Unregister(3U)));
    REQUIRE((registrations.DeviceCount() == 0U));
    REQUIRE((registrations.WindowCount() == 0U));
}
}
TEST_CASE("test_workspace_slot_release_registration_shares_one_window_listener_until_last_device", "[core][vendored]") { test_workspace_slot_release_registration_shares_one_window_listener_until_last_device(); }
TEST_CASE("test_workspace_slot_release_registration_tracks_windows_independently", "[core][vendored]") { test_workspace_slot_release_registration_tracks_windows_independently(); }
TEST_CASE("test_workspace_slot_release_registration_tolerates_duplicate_and_reordered_unregister", "[core][vendored]") { test_workspace_slot_release_registration_tolerates_duplicate_and_reordered_unregister(); }
TEST_CASE("test_workspace_slot_release_registration_handles_window_death_and_system_teardown", "[core][vendored]") { test_workspace_slot_release_registration_handles_window_death_and_system_teardown(); }
