#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
namespace mmltk::frameworks::gpu::detail {
struct GpuInventoryEntry final {
    std::int32_t device_id = 0;
    bool online = false;
    [[nodiscard]] constexpr bool operator==(const GpuInventoryEntry&) const noexcept = default;
};
struct GpuInventoryView final {
    std::uint64_t revision = 0U;
    std::size_t count = 0U;
    std::span<const GpuInventoryEntry> devices{};
};
[[nodiscard]] inline bool valid_gpu_inventory(const GpuInventoryView inventory) noexcept {
    if (!inventory.revision || inventory.count > inventory.devices.size()) { return false; }
    for (std::size_t index = 0U; index < inventory.count; ++index) {
        if (inventory.devices[index].device_id < 0 || !inventory.devices[index].online) { return false; }
        for (std::size_t prior = 0U; prior < index; ++prior) {
            if (inventory.devices[prior].device_id == inventory.devices[index].device_id) { return false; }
        }
    }
    return std::all_of(inventory.devices.begin() + inventory.count, inventory.devices.end(),
                       [](const GpuInventoryEntry entry) { return entry == GpuInventoryEntry{}; });
}
[[nodiscard]] inline bool gpu_inventory_contains(const GpuInventoryView inventory, const std::int32_t device_id) noexcept {
    return device_id >= 0 && std::any_of(inventory.devices.begin(), inventory.devices.begin() + inventory.count,
                                         [device_id](const GpuInventoryEntry entry) { return entry.online && entry.device_id == device_id; });
}
}  // namespace mmltk::frameworks::gpu::detail
