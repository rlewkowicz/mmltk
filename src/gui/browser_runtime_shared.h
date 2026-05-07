#pragma once

#include "browser/host_api.h"
#include "gui/browser_host_helpers.h"
#include "mmltk_logging.h"

#include <algorithm>
#include <chrono>
#include <concepts>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace mmltk::gui::browser_runtime_shared {

inline constexpr std::string_view kCallbacksReadyMessageType = "host.callbacks.ready";
inline constexpr std::string_view kRuntimeCapabilitiesMessageType = "host.runtime.capabilities";
inline constexpr std::string_view kDefaultFrameMimeType = "application/octet-stream";

struct ReadyWatcherScriptOptions {
    std::string_view post_message_expression;
    bool wrap_deliver_snapshot = false;
    bool send_ready_once = false;
};

[[nodiscard]] inline std::string make_ready_watcher_script(const ReadyWatcherScriptOptions& options) {
    std::string script = R"JS(
(() => {
  const root =
    typeof globalThis === "object" && globalThis !== null ? globalThis : this;
  const getOwnValue = (name) => {
    const descriptor = Object.getOwnPropertyDescriptor(root, name);
    return descriptor && Object.prototype.hasOwnProperty.call(descriptor, "value")
      ? descriptor.value
      : undefined;
  };
  const setOwnValue = (name, value) => {
    Object.defineProperty(root, name, {
      configurable: true,
      enumerable: false,
      writable: true,
      value,
    });
    return value;
  };
  let lastNavigatorGpuAvailable = null;
  let lastWorkspaceGpuBridgeCapabilitySignature = null;
)JS";
    if (options.send_ready_once) {
        script += "  let readyCallbacksSent = false;\n";
    }
    script += R"JS(

  const ensureBridge = () => {
    const existing = getOwnValue("__MMLTK_NATIVE_BRIDGE__");
    if (existing !== null && typeof existing === "object") {
      if (typeof existing.postMessage !== "function") {
        Object.defineProperty(existing, "postMessage", {
          configurable: true,
          enumerable: false,
          value(messageText) {
)JS";
    script += "            " + std::string(options.post_message_expression) + ";\n";
    script += R"JS(
          },
        });
      }
      return existing;
    }

    const bridge = {};
    Object.defineProperty(bridge, "postMessage", {
      configurable: true,
      enumerable: false,
      value(messageText) {
)JS";
    script += "        " + std::string(options.post_message_expression) + ";\n";
    script += R"JS(
      },
    });
    setOwnValue("__MMLTK_NATIVE_BRIDGE__", bridge);
    return bridge;
  };

  const readyWatcherBridge = ensureBridge();
  const readyWatcherInstalledDescriptor = Object.getOwnPropertyDescriptor(
    readyWatcherBridge,
    "__mmltkReadyWatcherInstalled",
  );
  if (
    readyWatcherInstalledDescriptor &&
    Object.prototype.hasOwnProperty.call(readyWatcherInstalledDescriptor, "value") &&
    readyWatcherInstalledDescriptor.value === true
  ) {
    return;
  }
  Object.defineProperty(readyWatcherBridge, "__mmltkReadyWatcherInstalled", {
    configurable: true,
    enumerable: false,
    writable: true,
    value: true,
  });

  const notifyReady = () => {
    const bridge = ensureBridge();
    if (typeof bridge.deliverSnapshot !== "function") {
      return;
    }
    if (typeof bridge.setBridgeState !== "function") {
      return;
    }
    if (typeof bridge.reportError !== "function") {
      return;
    }
)JS";
    if (options.send_ready_once) {
        script += R"JS(
    if (!readyCallbacksSent) {
      readyCallbacksSent = true;
      try {
        bridge.postMessage(
          JSON.stringify({ type: "host.callbacks.ready" }),
        );
      } catch (_) {}
    }
)JS";
    } else {
        script += R"JS(
    try {
      bridge.postMessage(
        JSON.stringify({ type: "host.callbacks.ready" }),
      );
    } catch (_) {}
)JS";
    }
    script += R"JS(
    const navigatorGpuAvailable =
      typeof navigator === "object" &&
      navigator !== null &&
      typeof navigator.gpu === "object" &&
      navigator.gpu !== null;
    const workspaceGpuBridge =
      typeof root.__MMLTK_WORKSPACE_GPU_BRIDGE__ === "object" &&
      root.__MMLTK_WORKSPACE_GPU_BRIDGE__ !== null
        ? root.__MMLTK_WORKSPACE_GPU_BRIDGE__
        : null;
    let workspaceGpuBridgeStatus = "unavailable";
    let workspaceGpuBridgeDetail =
      "__MMLTK_WORKSPACE_GPU_BRIDGE__ is not installed";
    if (workspaceGpuBridge !== null) {
      if (typeof workspaceGpuBridge.acquireCurrentSurface !== "function") {
        workspaceGpuBridgeDetail =
          "__MMLTK_WORKSPACE_GPU_BRIDGE__.acquireCurrentSurface is absent";
      } else if (typeof workspaceGpuBridge.importTexture !== "function") {
        workspaceGpuBridgeDetail =
          "__MMLTK_WORKSPACE_GPU_BRIDGE__.importTexture is absent";
      } else if (typeof workspaceGpuBridge.releaseSurface !== "function") {
        workspaceGpuBridgeDetail =
          "__MMLTK_WORKSPACE_GPU_BRIDGE__.releaseSurface is absent";
      } else if (typeof workspaceGpuBridge.releaseRendererTexture !== "function") {
        workspaceGpuBridgeDetail =
          "__MMLTK_WORKSPACE_GPU_BRIDGE__.releaseRendererTexture is absent";
      } else {
        workspaceGpuBridgeStatus = "unknown";
        workspaceGpuBridgeDetail =
          "Workspace GPU bridge functions are installed; native helper capability is pending.";
      }
    }
    const workspaceGpuBridgeCapabilitySignature =
      `${workspaceGpuBridgeStatus}|${workspaceGpuBridgeDetail}`;
    if (
      lastNavigatorGpuAvailable === navigatorGpuAvailable &&
      lastWorkspaceGpuBridgeCapabilitySignature ===
        workspaceGpuBridgeCapabilitySignature
    ) {
      return;
    }
    lastNavigatorGpuAvailable = navigatorGpuAvailable;
    lastWorkspaceGpuBridgeCapabilitySignature =
      workspaceGpuBridgeCapabilitySignature;
    const capabilityMessage = {
      type: "host.runtime.capabilities",
      navigator_gpu: navigatorGpuAvailable,
      workspace_surface_bridge: workspaceGpuBridgeStatus,
      workspace_surface_zero_copy: "unknown",
      capabilities: {
        workspace_surface_bridge: {
          status:
            workspaceGpuBridgeStatus === "available"
              ? "ready"
              : workspaceGpuBridgeStatus === "unavailable"
                ? "blocked"
                : "pending",
          summary:
            workspaceGpuBridgeStatus === "available"
              ? "workspace surface bridge ready"
              : workspaceGpuBridgeStatus === "unavailable"
                ? "workspace surface bridge unavailable"
                : "workspace surface bridge pending",
          detail: workspaceGpuBridgeDetail,
        },
        workspace_surface_zero_copy: {
          status: "pending",
          summary: "workspace zero-copy pending",
          detail: "No live workspace surface publication has been exported yet.",
        },
      },
    };
    try {
      bridge.postMessage(
        JSON.stringify(capabilityMessage),
      );
    } catch (_) {}
  };

  const watchBridgeProperty = (name) => {
    const bridge = ensureBridge();
    let value = bridge[name];
    Object.defineProperty(bridge, name, {
      configurable: true,
      enumerable: false,
      get() {
        return value;
      },
      set(nextValue) {
        value = nextValue;
        notifyReady();
      },
    });
  };

  const watchRootProperty = (name) => {
    const descriptor = Object.getOwnPropertyDescriptor(root, name);
    if (descriptor && descriptor.configurable === false) {
      return;
    }
    let value =
      descriptor && Object.prototype.hasOwnProperty.call(descriptor, "value")
        ? descriptor.value
        : root[name];
    try {
      Object.defineProperty(root, name, {
        configurable: true,
        enumerable: descriptor ? descriptor.enumerable : false,
        get() {
          return value;
        },
        set(nextValue) {
          value = nextValue;
          notifyReady();
        },
      });
    } catch (_) {}
  };
)JS";
    if (options.wrap_deliver_snapshot) {
        script += R"JS(

  const installDeliverSnapshotWatcher = () => {
    const bridge = ensureBridge();
    let value = bridge.deliverSnapshot;
    Object.defineProperty(bridge, "deliverSnapshot", {
      configurable: true,
      enumerable: false,
      get() {
        if (typeof value !== "function") {
          return value;
        }
        return function deliverSnapshotWrapped(jsonText) {
          return value.call(bridge, jsonText);
        };
      },
      set(nextValue) {
        value = nextValue;
        notifyReady();
      },
    });
  };

  installDeliverSnapshotWatcher();
)JS";
    } else {
        script += "\n  watchBridgeProperty(\"deliverSnapshot\");\n";
    }
    script += R"JS(
  const nodeWithinRoot = (root, node) =>
    root instanceof Node &&
    node instanceof Node &&
    (node === root || root.contains(node));

  const textOffsetWithinRoot = (root, node, offset) => {
    if (!nodeWithinRoot(root, node)) {
      return 0;
    }
    try {
      const range = document.createRange();
      range.selectNodeContents(root);
      range.setEnd(node, offset);
      return range.toString().length;
    } catch (_) {
      return 0;
    }
  };

  const resolveImeContext = () => {
    const active = document.activeElement;
    if (!(active instanceof HTMLElement)) {
      return {
        surrounding_text: "",
        cursor_position: 0,
        anchor_position: 0,
      };
    }

    if (
      active instanceof HTMLInputElement ||
      active instanceof HTMLTextAreaElement
    ) {
      const surroundingText =
        typeof active.value === "string" ? active.value : "";
      const selectionStart =
        typeof active.selectionStart === "number" ? active.selectionStart : 0;
      const selectionEnd =
        typeof active.selectionEnd === "number"
          ? active.selectionEnd
          : selectionStart;
      const hasSelection = selectionStart !== selectionEnd;
      const selectionDirection =
        typeof active.selectionDirection === "string"
          ? active.selectionDirection
          : "none";
      const backwardSelection =
        hasSelection && selectionDirection === "backward";
      const anchorPosition = backwardSelection ? selectionEnd : selectionStart;
      const cursorPosition = backwardSelection ? selectionStart : selectionEnd;
      return {
        surrounding_text: surroundingText,
        cursor_position: cursorPosition,
        anchor_position: anchorPosition,
      };
    }

    if (active.isContentEditable) {
      const selection = window.getSelection();
      const surroundingText = active.textContent ?? "";
      const anchorPosition =
        selection !== null
          ? textOffsetWithinRoot(active, selection.anchorNode, selection.anchorOffset)
          : 0;
      const cursorPosition =
        selection !== null
          ? textOffsetWithinRoot(active, selection.focusNode, selection.focusOffset)
          : anchorPosition;
      return {
        surrounding_text: surroundingText,
        cursor_position: cursorPosition,
        anchor_position: anchorPosition,
      };
    }

    return {
      surrounding_text: "",
      cursor_position: 0,
      anchor_position: 0,
    };
  };

  let imeContextQueued = false;
  const notifyImeContext = () => {
    const bridge = ensureBridge();
    const context = resolveImeContext();
    try {
      bridge.postMessage(
        JSON.stringify({
          type: "host.ime.context",
          surrounding_text: context.surrounding_text,
          cursor_position: context.cursor_position,
          anchor_position: context.anchor_position,
        }),
      );
    } catch (_) {}
  };

  const queueImeContext = () => {
    if (imeContextQueued) {
      return;
    }
    imeContextQueued = true;
    queueMicrotask(() => {
      imeContextQueued = false;
      notifyImeContext();
    });
  };

  document.addEventListener("selectionchange", queueImeContext, true);
  document.addEventListener("focusin", queueImeContext, true);
  document.addEventListener("focusout", queueImeContext, true);
  document.addEventListener("input", queueImeContext, true);
  document.addEventListener("keyup", queueImeContext, true);
  document.addEventListener("mouseup", queueImeContext, true);

  watchBridgeProperty("setBridgeState");
  watchBridgeProperty("reportError");
  watchRootProperty("__MMLTK_WORKSPACE_GPU_BRIDGE__");
  queueImeContext();
  notifyReady();
})();
)JS";
    return script;
}

[[nodiscard]] inline mmltk::browser::BrowserRuntimeCapabilities configured_runtime_capabilities(
    const mmltk::browser::BrowserHostBackend backend) {
    mmltk::browser::BrowserRuntimeCapabilities capabilities;
    capabilities.host_backend = backend;
    capabilities.workspace_surface_bridge = mmltk::browser::BrowserRuntimeCapabilityStatus::Unknown;
    capabilities.workspace_surface_zero_copy = mmltk::browser::BrowserRuntimeCapabilityStatus::Unknown;
    return capabilities;
}

[[nodiscard]] inline std::optional<std::string> runtime_capability_error_message(
    const mmltk::browser::BrowserRuntimeCapabilities&) {
    return std::nullopt;
}

template <typename T>
constexpr bool kAlwaysFalse = false;

template <typename T>
struct IsOptional : std::false_type {};
template <typename T>
struct IsOptional<std::optional<T>> : std::true_type {};

[[nodiscard]] inline nlohmann::json bridge_state_to_json(const mmltk::browser::BrowserBridgeState& state) {
    const mmltk::browser::BrowserRuntimeCapabilities& runtime_capabilities = state.runtime_capabilities;
    nlohmann::json json = {
        {"phase", mmltk::browser::browser_bridge_phase_name(state.phase)},
        {"connected", state.connected},
        {"lastError", state.last_error},
        {"lastSuccessRevision", state.last_success_revision.has_value() ? nlohmann::json(*state.last_success_revision)
                                                                        : nlohmann::json(nullptr)},
        {"runtimeCapabilities",
         {{"hostBackend", mmltk::browser::browser_host_backend_name(runtime_capabilities.host_backend)},
          {"navigatorGpu", mmltk::browser::browser_runtime_capability_status_name(runtime_capabilities.navigator_gpu)},
          {"workspaceSurfaceBridge",
           mmltk::browser::browser_runtime_capability_status_name(runtime_capabilities.workspace_surface_bridge)},
          {"workspaceSurfaceZeroCopy",
           mmltk::browser::browser_runtime_capability_status_name(runtime_capabilities.workspace_surface_zero_copy)}}},
    };
    if (state.capabilities.is_object() && !state.capabilities.empty()) {
        json["capabilities"] = state.capabilities;
    }
    return json;
}

template <typename Buffer>
[[nodiscard]] std::vector<std::uint8_t> copy_byte_buffer(const Buffer& buffer) {
    if constexpr (IsOptional<std::decay_t<Buffer>>::value) {
        if (!buffer.has_value()) {
            return {};
        }
        return copy_byte_buffer(*buffer);
    } else if constexpr (std::same_as<std::decay_t<Buffer>, std::vector<std::uint8_t>>) {
        return buffer;
    } else if constexpr (std::same_as<std::decay_t<Buffer>, std::string>) {
        return std::vector<std::uint8_t>(buffer.begin(), buffer.end());
    } else if constexpr (requires {
                             buffer.data();
                             buffer.size();
                         }) {
        using Element = std::remove_cv_t<std::remove_pointer_t<decltype(buffer.data())>>;
        static_assert(sizeof(Element) == 1U, "buffer elements must be byte-sized");
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(buffer.data());
        return {bytes, bytes + static_cast<std::size_t>(buffer.size())};
    } else if constexpr (requires {
                             buffer.begin();
                             buffer.end();
                         }) {
        std::vector<std::uint8_t> bytes;
        bytes.reserve(static_cast<std::size_t>(std::distance(buffer.begin(), buffer.end())));
        for (const auto value : buffer) {
            bytes.push_back(static_cast<std::uint8_t>(value));
        }
        return bytes;
    } else {
        static_assert(kAlwaysFalse<Buffer>, "unsupported frame byte buffer type");
    }
}

template <typename AppType, typename SnapshotCache, typename RefreshSnapshotRuntimeCapabilitiesFn>
inline void refresh_snapshot(AppType& app, SnapshotCache& snapshot_cache,
                             RefreshSnapshotRuntimeCapabilitiesFn&& refresh_snapshot_runtime_capabilities,
                             std::string& current_snapshot_json, std::uint64_t& current_snapshot_revision,
                             bool& snapshot_dirty, const std::string& delivered_snapshot_json) {
    mmltk::browser::StateSnapshot snapshot = browser_state_snapshot(app);
    std::forward<RefreshSnapshotRuntimeCapabilitiesFn>(refresh_snapshot_runtime_capabilities)(snapshot);
    current_snapshot_json = snapshot_cache.encode([snapshot = std::move(snapshot)]() mutable { return snapshot; });
    current_snapshot_revision = snapshot_cache.last_revision();
    if (current_snapshot_json != delivered_snapshot_json) {
        snapshot_dirty = true;
    }
}

inline void queue_bridge_state(bool& bridge_state_dirty) noexcept {
    bridge_state_dirty = true;
}

inline void update_bridge_phase(mmltk::browser::BrowserBridgeState& bridge_state,
                                const mmltk::browser::BrowserBridgePhase phase, bool& bridge_state_dirty) {
    if (bridge_state.phase == phase) {
        return;
    }
    bridge_state.phase = phase;
    queue_bridge_state(bridge_state_dirty);
}

inline void set_bridge_error(mmltk::browser::BrowserBridgeState& bridge_state, std::string message,
                             const bool report_to_page, bool& bridge_state_dirty, std::string& pending_error_text,
                             bool& error_dirty) {
    if (message == bridge_state.last_error && !report_to_page) {
        return;
    }
    bridge_state.last_error = std::move(message);
    queue_bridge_state(bridge_state_dirty);
    if (report_to_page && !bridge_state.last_error.empty()) {
        pending_error_text = bridge_state.last_error;
        error_dirty = true;
    }
}

template <typename ResetRuntimeCapabilityProbeFn>
inline void clear_delivery_state(bool& page_callbacks_ready, std::string& delivered_snapshot_json,
                                 std::string& delivered_bridge_state_json, std::string& pending_error_text,
                                 bool& error_dirty, bool& dispatch_pending_idle,
                                 const std::string& current_snapshot_json, bool& snapshot_dirty,
                                 mmltk::browser::BrowserBridgeState& bridge_state,
                                 ResetRuntimeCapabilityProbeFn&& reset_runtime_capability_probe,
                                 bool& bridge_state_dirty) {
    page_callbacks_ready = false;
    delivered_snapshot_json.clear();
    delivered_bridge_state_json.clear();
    pending_error_text.clear();
    error_dirty = false;
    dispatch_pending_idle = false;
    if (!current_snapshot_json.empty()) {
        snapshot_dirty = true;
    }
    bridge_state.connected = false;
    bridge_state.last_success_revision.reset();
    bridge_state.last_error.clear();
    bridge_state.phase = mmltk::browser::BrowserBridgePhase::Polling;
    bridge_state.capabilities = nlohmann::json::object();
    std::forward<ResetRuntimeCapabilityProbeFn>(reset_runtime_capability_probe)();
    queue_bridge_state(bridge_state_dirty);
}

template <typename ConfigureRuntimeCapabilitiesFn, typename ReportRuntimeCapabilitiesFn>
inline void reset_runtime_capability_probe(
    mmltk::browser::BrowserBridgeState& bridge_state, bool& runtime_capabilities_reported,
    bool& runtime_capability_gate_passed,
    std::optional<std::chrono::steady_clock::time_point>& runtime_capability_deadline,
    ConfigureRuntimeCapabilitiesFn&& configured_runtime_capabilities,
    ReportRuntimeCapabilitiesFn&& report_runtime_capabilities) {
    mmltk::browser::BrowserRuntimeCapabilities capabilities =
        std::forward<ConfigureRuntimeCapabilitiesFn>(configured_runtime_capabilities)();
    capabilities.navigator_gpu = mmltk::browser::BrowserRuntimeCapabilityStatus::Unknown;
    std::forward<ReportRuntimeCapabilitiesFn>(report_runtime_capabilities)(capabilities);
    bridge_state.runtime_capabilities = capabilities;
    runtime_capabilities_reported = false;
    runtime_capability_gate_passed = false;
    runtime_capability_deadline.reset();
}

template <typename Clock = std::chrono::steady_clock>
inline void arm_runtime_capability_probe_deadline(std::optional<typename Clock::time_point>& deadline,
                                                  const typename Clock::duration timeout) {
    deadline = Clock::now() + timeout;
}

template <typename Clock = std::chrono::steady_clock>
[[nodiscard]] inline bool runtime_capability_probe_expired(const bool page_callbacks_ready,
                                                           const bool runtime_capabilities_reported,
                                                           const bool runtime_capability_gate_passed,
                                                           const std::optional<typename Clock::time_point>& deadline) {
    if (!page_callbacks_ready || runtime_capabilities_reported || runtime_capability_gate_passed ||
        !deadline.has_value()) {
        return false;
    }
    return Clock::now() >= *deadline;
}

template <typename FatalErrorFn>
inline void fail_if_runtime_capability_probe_expired(
    const bool page_callbacks_ready, const bool runtime_capabilities_reported,
    const bool runtime_capability_gate_passed, const std::optional<std::chrono::steady_clock::time_point>& deadline,
    FatalErrorFn&& fatal_error) {
    if (!runtime_capability_probe_expired(page_callbacks_ready, runtime_capabilities_reported,
                                          runtime_capability_gate_passed, deadline)) {
        return;
    }
    std::forward<FatalErrorFn>(fatal_error)(
        "embedded browser runtime did not report startup capabilities before "
        "the probe deadline");
}

template <typename FatalErrorFn>
inline void verify_runtime_capabilities_or_abort(const mmltk::browser::BrowserRuntimeCapabilities& capabilities,
                                                 FatalErrorFn&& fatal_error) {
    const std::optional<std::string> error_message = runtime_capability_error_message(capabilities);
    if (!error_message.has_value()) {
        return;
    }
    std::forward<FatalErrorFn>(fatal_error)(*error_message);
}

template <typename VerifyRuntimeCapabilitiesFn, typename QueueBridgeStateFn, typename RefreshSnapshotFn,
          typename FlushPendingMessagesFn>
inline void maybe_complete_runtime_capability_gate(
    const bool page_callbacks_ready, const bool runtime_capabilities_reported, bool& runtime_capability_gate_passed,
    const bool quit_requested, std::optional<std::chrono::steady_clock::time_point>& runtime_capability_deadline,
    mmltk::browser::BrowserBridgeState& bridge_state,
    VerifyRuntimeCapabilitiesFn&& verify_runtime_capabilities_or_abort, QueueBridgeStateFn&& queue_bridge_state_fn,
    RefreshSnapshotFn&& refresh_snapshot, FlushPendingMessagesFn&& flush_pending_messages) {
    if (!page_callbacks_ready || !runtime_capabilities_reported || runtime_capability_gate_passed) {
        return;
    }

    std::forward<VerifyRuntimeCapabilitiesFn>(verify_runtime_capabilities_or_abort)();
    if (quit_requested) {
        return;
    }

    runtime_capability_gate_passed = true;
    runtime_capability_deadline.reset();
    bridge_state.connected = true;
    bridge_state.last_error.clear();
    bridge_state.phase = mmltk::browser::BrowserBridgePhase::Idle;
    std::forward<QueueBridgeStateFn>(queue_bridge_state_fn)();
    std::forward<RefreshSnapshotFn>(refresh_snapshot)();
    std::forward<FlushPendingMessagesFn>(flush_pending_messages)();
}

inline void mark_snapshot_delivery_revision(mmltk::browser::BrowserBridgeState& bridge_state, const bool snapshot_dirty,
                                            const std::string& current_snapshot_json,
                                            const std::uint64_t current_snapshot_revision, bool& bridge_state_dirty) {
    if (!bridge_state.connected || !snapshot_dirty || current_snapshot_json.empty()) {
        return;
    }
    bridge_state.last_success_revision = current_snapshot_revision;
    queue_bridge_state(bridge_state_dirty);
}

struct PendingBridgeDispatch {
    std::string bridge_json;
    bool send_bridge = false;
    bool send_snapshot = false;
    bool send_error = false;
};

[[nodiscard]] inline std::optional<PendingBridgeDispatch> build_pending_bridge_dispatch(
    const mmltk::browser::BrowserBridgeState& bridge_state, const bool bridge_state_dirty,
    const bool runtime_capability_gate_passed, const bool snapshot_dirty, const std::string& current_snapshot_json,
    const bool error_dirty, const std::string& pending_error_text, const std::string& delivered_bridge_state_json) {
    PendingBridgeDispatch dispatch;
    dispatch.bridge_json = bridge_state_to_json(bridge_state).dump();
    dispatch.send_bridge = bridge_state_dirty || dispatch.bridge_json != delivered_bridge_state_json;
    dispatch.send_snapshot = runtime_capability_gate_passed && snapshot_dirty && !current_snapshot_json.empty();
    dispatch.send_error = error_dirty && !pending_error_text.empty();
    if (!dispatch.send_bridge && !dispatch.send_snapshot && !dispatch.send_error) {
        return std::nullopt;
    }
    return dispatch;
}

inline void complete_pending_bridge_dispatch(const PendingBridgeDispatch& dispatch,
                                             const std::string& current_snapshot_json,
                                             std::string& delivered_bridge_state_json, bool& bridge_state_dirty,
                                             std::string& delivered_snapshot_json, bool& snapshot_dirty,
                                             std::string& pending_error_text, bool& error_dirty) {
    if (dispatch.send_bridge) {
        delivered_bridge_state_json = dispatch.bridge_json;
        bridge_state_dirty = false;
    }
    if (dispatch.send_snapshot) {
        delivered_snapshot_json = current_snapshot_json;
        snapshot_dirty = false;
    }
    if (dispatch.send_error) {
        pending_error_text.clear();
        error_dirty = false;
    }
}

[[nodiscard]] inline std::string bridge_dispatch_script(const std::string* bridge_json,
                                                        const std::string* snapshot_json,
                                                        const std::string* error_text,
                                                        const std::string* surface_ready_json = nullptr);

template <typename ExecuteScriptFn>
inline void flush_pending_bridge_messages(mmltk::browser::BrowserBridgeState& bridge_state, bool& bridge_state_dirty,
                                          const bool runtime_capability_gate_passed, bool& snapshot_dirty,
                                          const std::string& current_snapshot_json,
                                          const std::uint64_t current_snapshot_revision, bool& error_dirty,
                                          std::string& pending_error_text, std::string& delivered_bridge_state_json,
                                          std::string& delivered_snapshot_json, ExecuteScriptFn&& execute_script) {
    mark_snapshot_delivery_revision(bridge_state, snapshot_dirty, current_snapshot_json, current_snapshot_revision,
                                    bridge_state_dirty);
    const std::optional<PendingBridgeDispatch> dispatch = build_pending_bridge_dispatch(
        bridge_state, bridge_state_dirty, runtime_capability_gate_passed, snapshot_dirty, current_snapshot_json,
        error_dirty, pending_error_text, delivered_bridge_state_json);
    if (!dispatch.has_value()) {
        return;
    }

    std::forward<ExecuteScriptFn>(execute_script)(
        bridge_dispatch_script(dispatch->send_bridge ? &dispatch->bridge_json : nullptr,
                               dispatch->send_snapshot ? &current_snapshot_json : nullptr,
                               dispatch->send_error ? &pending_error_text : nullptr, nullptr));
    complete_pending_bridge_dispatch(*dispatch, current_snapshot_json, delivered_bridge_state_json, bridge_state_dirty,
                                     delivered_snapshot_json, snapshot_dirty, pending_error_text, error_dirty);
}

inline void publish_runtime_capabilities(const mmltk::browser::BrowserRuntimeCapabilities& capabilities,
                                         mmltk::browser::StateSnapshot& snapshot,
                                         mmltk::browser::BrowserBridgeState& bridge_state, bool& bridge_state_dirty) {
    snapshot.runtime_capabilities = capabilities;
    if (snapshot.workflow_state.is_object()) {
        if (auto live_runtime_it = snapshot.workflow_state.find("live_runtime");
            live_runtime_it != snapshot.workflow_state.end() && live_runtime_it->is_object()) {
            (*live_runtime_it)["runtime_capabilities"] = capabilities;
        }
    }

    if (!(bridge_state.runtime_capabilities == capabilities)) {
        bridge_state.runtime_capabilities = capabilities;
        queue_bridge_state(bridge_state_dirty);
    }
}

[[nodiscard]] inline std::string lower_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](const unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

[[nodiscard]] inline std::string mime_type_for_path(const std::filesystem::path& path) {
    const std::string extension = lower_ascii(path.extension().string());
    if (extension == ".html" || extension == ".htm") {
        return "text/html";
    }
    if (extension == ".css") {
        return "text/css";
    }
    if (extension == ".js" || extension == ".mjs") {
        return "application/javascript";
    }
    if (extension == ".json" || extension == ".map") {
        return "application/json";
    }
    if (extension == ".svg") {
        return "image/svg+xml";
    }
    if (extension == ".png") {
        return "image/png";
    }
    if (extension == ".jpg" || extension == ".jpeg") {
        return "image/jpeg";
    }
    if (extension == ".webp") {
        return "image/webp";
    }
    if (extension == ".ico") {
        return "image/x-icon";
    }
    if (extension == ".wasm") {
        return "application/wasm";
    }
    if (extension == ".woff") {
        return "font/woff";
    }
    if (extension == ".woff2") {
        return "font/woff2";
    }
    if (extension == ".ttf") {
        return "font/ttf";
    }
    if (extension == ".txt") {
        return "text/plain";
    }
    return std::string(kDefaultFrameMimeType);
}

[[nodiscard]] inline std::vector<std::uint8_t> read_binary_file(const std::filesystem::path& path) {
    const auto file_size = std::filesystem::file_size(path);
    if (file_size > static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max())) {
        throw std::runtime_error("asset is too large to load: " + path.string());
    }

    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        throw std::runtime_error("failed to open asset: " + path.string());
    }

    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(file_size));
    if (!bytes.empty()) {
        stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    if (!stream && !stream.eof()) {
        throw std::runtime_error("failed to read asset: " + path.string());
    }
    return bytes;
}

[[nodiscard]] inline bool path_is_within_root(const std::filesystem::path& root, const std::filesystem::path& path) {
    const std::string root_text = root.lexically_normal().generic_string();
    const std::string path_text = path.lexically_normal().generic_string();
    if (path_text == root_text) {
        return true;
    }
    std::string normalized_root = root_text;
    if (!normalized_root.empty() && normalized_root.back() != '/') {
        normalized_root.push_back('/');
    }
    return path_text.starts_with(normalized_root);
}

[[nodiscard]] inline std::string percent_encode(const std::string_view text, const bool preserve_slash) {
    constexpr std::string_view kHex = "0123456789ABCDEF";
    std::string encoded;
    encoded.reserve(text.size() + 8U);
    for (const unsigned char ch : text) {
        const bool unreserved = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
                                ch == '-' || ch == '_' || ch == '.' || ch == '~' || (preserve_slash && ch == '/');
        if (unreserved) {
            encoded.push_back(static_cast<char>(ch));
            continue;
        }
        encoded.push_back('%');
        encoded.push_back(kHex[(ch >> 4U) & 0x0FU]);
        encoded.push_back(kHex[ch & 0x0FU]);
    }
    return encoded;
}

[[nodiscard]] inline std::string percent_decode(const std::string_view text) {
    std::string decoded;
    decoded.reserve(text.size());
    for (std::size_t index = 0; index < text.size(); ++index) {
        const char ch = text[index];
        if (ch == '%' && index + 2U < text.size()) {
            const auto hex_value = [](const char value) -> int {
                if (value >= '0' && value <= '9') {
                    return value - '0';
                }
                if (value >= 'a' && value <= 'f') {
                    return 10 + (value - 'a');
                }
                if (value >= 'A' && value <= 'F') {
                    return 10 + (value - 'A');
                }
                return -1;
            };
            const int hi = hex_value(text[index + 1U]);
            const int lo = hex_value(text[index + 2U]);
            if (hi >= 0 && lo >= 0) {
                decoded.push_back(static_cast<char>((hi << 4U) | lo));
                index += 2U;
                continue;
            }
        }
        decoded.push_back(ch);
    }
    return decoded;
}

[[nodiscard]] inline std::string app_origin(const std::string_view scheme, const std::string_view host) {
    return std::string(scheme) + "://" + std::string(host);
}

[[nodiscard]] inline std::string browser_entry_uri(const BrowserHostAssetPaths& assets, const std::string_view scheme,
                                                   const std::string_view host) {
    std::error_code error;
    const std::filesystem::path relative = std::filesystem::relative(assets.index_html, assets.bundle_root, error);
    if (error) {
        throw std::runtime_error("failed to resolve browser app entry URI: " + error.message());
    }
    return app_origin(scheme, host) + "/" + percent_encode(relative.generic_string(), true);
}

[[nodiscard]] inline bool is_message_type(const nlohmann::json& json, const std::string_view type) {
    const auto it = json.find("type");
    return it != json.end() && it->is_string() && it->get_ref<const std::string&>() == type;
}

[[nodiscard]] inline bool is_release_all_message(const nlohmann::json& json) {
    return is_message_type(json, "frame.release_all") || is_message_type(json, "browser.frame.release_all") ||
           is_message_type(json, "host.frame.release_all") || is_message_type(json, "bridge.disconnect") ||
           is_message_type(json, "page.unload");
}

[[nodiscard]] inline std::string bridge_dispatch_script(const std::string* bridge_json,
                                                        const std::string* snapshot_json,
                                                        const std::string* error_text,
                                                        const std::string* surface_ready_json) {
    std::string script =
        "(() => {const root=globalThis;"
        "const bridgeDescriptor=Object.getOwnPropertyDescriptor(root,"
        "'__MMLTK_NATIVE_BRIDGE__');"
        "const bridge=bridgeDescriptor&&Object.prototype.hasOwnProperty.call("
        "bridgeDescriptor,'value')?bridgeDescriptor.value:null;";
    if (bridge_json != nullptr) {
        script += "const setBridgeState=bridge&&bridge.setBridgeState;";
        script += "if(typeof setBridgeState==='function'){setBridgeState(";
        script += nlohmann::json(*bridge_json).dump();
        script += ");}";
    }
    if (snapshot_json != nullptr) {
        script += "const deliverSnapshot=bridge&&bridge.deliverSnapshot;";
        script += "if(typeof deliverSnapshot==='function'){deliverSnapshot(";
        script += nlohmann::json(*snapshot_json).dump();
        script += ");}";
    }
    if (error_text != nullptr) {
        script += "const reportError=bridge&&bridge.reportError;";
        script += "if(typeof reportError==='function'){reportError(";
        script += nlohmann::json(*error_text).dump();
        script += ");}";
    }
    if (surface_ready_json != nullptr) {
        script += "const deliverSurfaceReady=bridge&&bridge.deliverSurfaceReady;";
        script += "if(typeof deliverSurfaceReady==='function'){deliverSurfaceReady(";
        script += nlohmann::json(*surface_ready_json).dump();
        script += ");}";
    }
    script += "})();";
    return script;
}

[[nodiscard]] inline std::optional<mmltk::browser::BrowserRuntimeCapabilityStatus> extract_navigator_gpu_capability(
    const nlohmann::json& message) {
    const auto navigator_gpu_it = message.find("navigator_gpu");
    if (navigator_gpu_it == message.end()) {
        return std::nullopt;
    }
    if (!navigator_gpu_it->is_boolean()) {
        throw std::runtime_error("browser runtime capabilities navigator_gpu must be a boolean");
    }
    return navigator_gpu_it->get<bool>() ? mmltk::browser::BrowserRuntimeCapabilityStatus::Available
                                         : mmltk::browser::BrowserRuntimeCapabilityStatus::Unavailable;
}

[[nodiscard]] inline std::optional<mmltk::browser::BrowserRuntimeCapabilityStatus> extract_runtime_capability_status(
    const nlohmann::json& message, const std::string_view snake_case_key, const std::string_view camel_case_key) {
    const auto it = message.find(std::string(snake_case_key));
    const auto camel_case_it = message.find(std::string(camel_case_key));
    const auto* value = it != message.end() ? &(*it) : camel_case_it != message.end() ? &(*camel_case_it) : nullptr;
    if (value == nullptr) {
        return std::nullopt;
    }
    if (!value->is_string()) {
        throw std::runtime_error("browser runtime capability status fields must be strings");
    }
    return mmltk::browser::browser_runtime_capability_status_from_name(value->get<std::string>());
}

[[nodiscard]] inline bool runtime_capabilities_complete(
    const mmltk::browser::BrowserRuntimeCapabilities& capabilities) noexcept {
    return capabilities.navigator_gpu != mmltk::browser::BrowserRuntimeCapabilityStatus::Unknown &&
           capabilities.workspace_surface_bridge != mmltk::browser::BrowserRuntimeCapabilityStatus::Unknown;
}

[[nodiscard]] inline bool apply_capability_contracts_from_message(
    const nlohmann::json& message, mmltk::browser::BrowserBridgeState& bridge_state) {
    const auto capabilities_it = message.find("capabilities");
    if (capabilities_it == message.end()) {
        return false;
    }
    if (!capabilities_it->is_object()) {
        throw std::runtime_error("browser runtime capabilities contracts must be an object");
    }
    if (bridge_state.capabilities == *capabilities_it) {
        return false;
    }
    bridge_state.capabilities = *capabilities_it;
    return true;
}

template <typename RefreshBeforeVerifyFn, typename OnCapabilityChangedFn, typename VerifyRuntimeCapabilitiesFn,
          typename MaybeCompleteRuntimeCapabilityGateFn>
inline void handle_runtime_capabilities_message(
    const nlohmann::json& message, bool& runtime_capabilities_reported, const bool runtime_capability_gate_passed,
    const bool& quit_requested, mmltk::browser::BrowserRuntimeCapabilities& reported_runtime_capabilities,
    mmltk::browser::BrowserBridgeState& bridge_state,
    RefreshBeforeVerifyFn&& refresh_before_verify, OnCapabilityChangedFn&& on_capability_changed,
    VerifyRuntimeCapabilitiesFn&& verify_runtime_capabilities_or_abort,
    MaybeCompleteRuntimeCapabilityGateFn&& maybe_complete_runtime_capability_gate) {
    const auto navigator_gpu = extract_navigator_gpu_capability(message);
    const auto workspace_surface_bridge =
        extract_runtime_capability_status(message, "workspace_surface_bridge", "workspaceSurfaceBridge");
    const auto workspace_surface_zero_copy =
        extract_runtime_capability_status(message, "workspace_surface_zero_copy", "workspaceSurfaceZeroCopy");
    const mmltk::browser::BrowserRuntimeCapabilities previous_capabilities = reported_runtime_capabilities;

    if (navigator_gpu.has_value()) {
        reported_runtime_capabilities.navigator_gpu = *navigator_gpu;
    }
    if (workspace_surface_bridge.has_value()) {
        reported_runtime_capabilities.workspace_surface_bridge = *workspace_surface_bridge;
    }
    if (workspace_surface_zero_copy.has_value()) {
        reported_runtime_capabilities.workspace_surface_zero_copy = *workspace_surface_zero_copy;
    }
    const bool capability_contract_changed = apply_capability_contracts_from_message(message, bridge_state);
    runtime_capabilities_reported = runtime_capabilities_complete(reported_runtime_capabilities);
    const bool capability_changed =
        capability_contract_changed || !runtime_capabilities_reported ||
        !(previous_capabilities == reported_runtime_capabilities);
    std::forward<RefreshBeforeVerifyFn>(refresh_before_verify)();
    if (runtime_capability_gate_passed) {
        std::forward<VerifyRuntimeCapabilitiesFn>(verify_runtime_capabilities_or_abort)();
        if (quit_requested) {
            return;
        }
    }
    if (capability_changed) {
        std::forward<OnCapabilityChangedFn>(on_capability_changed)();
    }
    if (!runtime_capability_gate_passed) {
        std::forward<MaybeCompleteRuntimeCapabilityGateFn>(maybe_complete_runtime_capability_gate)();
    }
}

template <typename UpdateBridgePhaseFn, typename ArmDeadlineFn, typename QueueBridgeStateFn,
          typename FlushPendingMessagesFn, typename MaybeCompleteRuntimeCapabilityGateFn>
inline void handle_callbacks_ready_message(
    bool& page_callbacks_ready, mmltk::browser::BrowserBridgeState& bridge_state,
    UpdateBridgePhaseFn&& update_bridge_phase, ArmDeadlineFn&& arm_runtime_capability_probe_deadline,
    QueueBridgeStateFn&& queue_bridge_state_fn, FlushPendingMessagesFn&& flush_pending_messages,
    MaybeCompleteRuntimeCapabilityGateFn&& maybe_complete_runtime_capability_gate) {
    page_callbacks_ready = true;
    bridge_state.last_error.clear();
    bridge_state.connected = false;
    std::forward<UpdateBridgePhaseFn>(update_bridge_phase)(mmltk::browser::BrowserBridgePhase::Polling);
    std::forward<ArmDeadlineFn>(arm_runtime_capability_probe_deadline)();
    std::forward<QueueBridgeStateFn>(queue_bridge_state_fn)();
    std::forward<FlushPendingMessagesFn>(flush_pending_messages)();
    std::forward<MaybeCompleteRuntimeCapabilityGateFn>(maybe_complete_runtime_capability_gate)();
}

template <typename AppType, typename SetBridgeErrorFn, typename UpdateBridgePhaseFn>
inline void handle_intent_message(AppType& app, const nlohmann::json& message,
                                  const bool runtime_capability_gate_passed, SetBridgeErrorFn&& set_bridge_error_fn,
                                  UpdateBridgePhaseFn&& update_bridge_phase, bool& dispatch_pending_idle) {
    if (!runtime_capability_gate_passed) {
        throw std::runtime_error(
            "browser bridge received an intent before the runtime "
            "capability gate passed");
    }
    apply_browser_intent(app, message.get<mmltk::browser::IntentMessage>());
    std::forward<SetBridgeErrorFn>(set_bridge_error_fn)({}, false);
    std::forward<UpdateBridgePhaseFn>(update_bridge_phase)(mmltk::browser::BrowserBridgePhase::Dispatch);
    dispatch_pending_idle = true;
}

template <typename HandleCallbacksReadyFn, typename HandleRuntimeCapabilitiesMessageFn, typename ReleaseAllFramesFn,
          typename HandleExtraMessageFn, typename HandleIntentMessageFn>
inline void dispatch_bridge_message(const nlohmann::json& message, HandleCallbacksReadyFn&& handle_callbacks_ready,
                                    HandleRuntimeCapabilitiesMessageFn&& handle_runtime_capabilities_message,
                                    ReleaseAllFramesFn&& release_all_frames,
                                    HandleExtraMessageFn&& handle_extra_message,
                                    HandleIntentMessageFn&& handle_intent_message) {
    if (!message.is_object()) {
        throw std::runtime_error("browser bridge JSON payload must be an object");
    }
    if (is_message_type(message, kCallbacksReadyMessageType)) {
        std::forward<HandleCallbacksReadyFn>(handle_callbacks_ready)();
        return;
    }
    if (is_message_type(message, kRuntimeCapabilitiesMessageType)) {
        std::forward<HandleRuntimeCapabilitiesMessageFn>(handle_runtime_capabilities_message)(message);
        return;
    }
    if (is_release_all_message(message)) {
        std::forward<ReleaseAllFramesFn>(release_all_frames)();
        return;
    }
    if (std::forward<HandleExtraMessageFn>(handle_extra_message)(message)) {
        return;
    }
    if (is_message_type(message, mmltk::browser::kIntentMessageType)) {
        std::forward<HandleIntentMessageFn>(handle_intent_message)(message);
        return;
    }
    throw std::runtime_error("unsupported browser bridge message type");
}

template <typename AppType, typename RefreshBeforeVerifyFn, typename RefreshOnCapabilityChangedFn,
          typename VerifyRuntimeCapabilitiesFn, typename MaybeCompleteRuntimeCapabilityGateFn,
          typename UpdateBridgePhaseFn, typename ArmDeadlineFn, typename QueueBridgeStateFn,
          typename FlushPendingMessagesFn, typename SetBridgeErrorFn, typename ReleaseAllFramesFn,
          typename HandleExtraMessageFn>
inline void handle_bridge_message_payload(
    AppType& app, const std::string& payload, bool& page_callbacks_ready, bool& runtime_capabilities_reported,
    const bool runtime_capability_gate_passed, const bool& quit_requested,
    mmltk::browser::BrowserBridgeState& bridge_state,
    mmltk::browser::BrowserRuntimeCapabilities& reported_runtime_capabilities,
    RefreshBeforeVerifyFn&& refresh_before_verify, RefreshOnCapabilityChangedFn&& refresh_on_capability_changed,
    VerifyRuntimeCapabilitiesFn&& verify_runtime_capabilities_or_abort,
    MaybeCompleteRuntimeCapabilityGateFn&& maybe_complete_runtime_capability_gate,
    UpdateBridgePhaseFn&& update_bridge_phase, ArmDeadlineFn&& arm_runtime_capability_probe_deadline,
    QueueBridgeStateFn&& queue_bridge_state_fn, FlushPendingMessagesFn&& flush_pending_messages,
    SetBridgeErrorFn&& set_bridge_error_fn, ReleaseAllFramesFn&& release_all_frames,
    HandleExtraMessageFn&& handle_extra_message, bool& dispatch_pending_idle) {
    if (payload.empty()) {
        return;
    }

    auto refresh_before_verify_fn = std::forward<RefreshBeforeVerifyFn>(refresh_before_verify);
    auto refresh_on_capability_changed_fn = std::forward<RefreshOnCapabilityChangedFn>(refresh_on_capability_changed);
    auto verify_runtime_capabilities_fn =
        std::forward<VerifyRuntimeCapabilitiesFn>(verify_runtime_capabilities_or_abort);
    auto maybe_complete_runtime_capability_gate_fn =
        std::forward<MaybeCompleteRuntimeCapabilityGateFn>(maybe_complete_runtime_capability_gate);
    auto update_bridge_phase_fn = std::forward<UpdateBridgePhaseFn>(update_bridge_phase);
    auto arm_runtime_capability_probe_deadline_fn = std::forward<ArmDeadlineFn>(arm_runtime_capability_probe_deadline);
    auto queue_bridge_state_helper_fn = std::forward<QueueBridgeStateFn>(queue_bridge_state_fn);
    auto flush_pending_messages_fn = std::forward<FlushPendingMessagesFn>(flush_pending_messages);
    auto set_bridge_error_helper_fn = std::forward<SetBridgeErrorFn>(set_bridge_error_fn);
    auto release_all_frames_fn = std::forward<ReleaseAllFramesFn>(release_all_frames);
    auto handle_extra_message_fn = std::forward<HandleExtraMessageFn>(handle_extra_message);

    const nlohmann::json message = nlohmann::json::parse(payload);
    dispatch_bridge_message(
        message,
        [&] {
            handle_callbacks_ready_message(page_callbacks_ready, bridge_state, update_bridge_phase_fn,
                                           arm_runtime_capability_probe_deadline_fn, queue_bridge_state_helper_fn,
                                           flush_pending_messages_fn, maybe_complete_runtime_capability_gate_fn);
        },
        [&](const nlohmann::json& runtime_capabilities_message) {
            handle_runtime_capabilities_message(
                runtime_capabilities_message, runtime_capabilities_reported, runtime_capability_gate_passed,
                quit_requested, reported_runtime_capabilities, bridge_state, refresh_before_verify_fn,
                [&] {
                    queue_bridge_state_helper_fn();
                    refresh_on_capability_changed_fn();
                    flush_pending_messages_fn();
                },
                verify_runtime_capabilities_fn, maybe_complete_runtime_capability_gate_fn);
        },
        release_all_frames_fn, handle_extra_message_fn,
        [&](const nlohmann::json& intent_message) {
            handle_intent_message(app, intent_message, runtime_capability_gate_passed, set_bridge_error_helper_fn,
                                  update_bridge_phase_fn, dispatch_pending_idle);
        });
}

}  // namespace mmltk::gui::browser_runtime_shared
