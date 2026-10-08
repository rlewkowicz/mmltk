module;
#include <string>
#include <string_view>
#include <vector>

export module mmltk.tools.container_commands;
import mmltk.tools.tool_command;

export {
namespace mmltk::container_commands {

inline std::string tool(tools::command::Command selected) {
 return tools::command::shell_invocation(selected, {}, {}, "/workspace/build/tools/mmltk");
}
using tools::command::Command;

// The packaged acceptance child has two process roles: enumeration enters the
// executable directly; execution may enter the owned private compositor.
inline std::vector<std::string> workspace_test_command(bool headless, const std::string& executable,
        const std::vector<std::string>& arguments, const std::string& runtime_image,
        const std::string& validation_image) {
    std::vector<std::string> result;
    if (headless && (arguments.empty() || arguments.front() != "--list-tests")) {
        result = {"--ulimit", "core=1:1", "--entrypoint", "/workspace/build/tools/mmltk", validation_image};
        auto command = tools::command::self_invocation(Command::headless_compositor,
            {"--", executable}, {}, "/workspace/build/tools/mmltk");
        result.insert(result.end(), command.begin() + 1, command.end());
    } else result = {"--entrypoint", executable, runtime_image};
    result.insert(result.end(), arguments.begin(), arguments.end());
    return result;
}

inline const std::string configure_release = R"MMLTK(         )MMLTK" + tool(Command::configure_cmake_graph) + R"MMLTK( \
             release "${MMLTK_CONTAINER_CACHE_ROOT}/cmake/release" \
             -DBUILD_MMLTK_BROWSER_HOST=ON \
             -DBUILD_MMLTK_FIREFOX_RUNTIME=ON \
             -DMMLTK_BUILD_JOBS="${MMLTK_JOBS}" \
             -DMMLTK_FIREFOX_CACHE_ROOT="${MMLTK_FIREFOX_CACHE_ROOT}"
)MMLTK";

inline const std::string release_ninja = R"MMLTK(set -euo pipefail
         capture_compiler_cache_stats() {
             if [[ -n "${MMLTK_CCACHE_STATS_FILE:-}" ]]; then
                 ccache --show-stats >"${MMLTK_CCACHE_STATS_FILE}" 2>&1 || true
             fi
             if [[ -n "${MMLTK_FIREFOX_SCCACHE_STATS_FILE:-}" ]]; then
                 sccache_bin="${MOZBUILD_STATE_PATH}/sccache/sccache"
                 if [[ -x "${sccache_bin}" ]]; then
                     "${sccache_bin}" --show-stats \
                         >"${MMLTK_FIREFOX_SCCACHE_STATS_FILE}" 2>&1 || true
                 fi
             fi
         }
         trap capture_compiler_cache_stats EXIT
         if [[ -n "${MMLTK_CCACHE_STATS_FILE:-}" ]]; then
             ccache --zero-stats
         fi
         sccache_bin="${MOZBUILD_STATE_PATH}/sccache/sccache"
         if [[ -x "${sccache_bin}" && -n "${MMLTK_FIREFOX_SCCACHE_STATS_FILE:-}" ]]; then
             "${sccache_bin}" --zero-stats || true
         fi
         )MMLTK" + tool(Command::run_traced_command) + R"MMLTK( cargo_fmt_iced_aw \
             cargo fmt --manifest-path /workspace/third_party/iced_aw/Cargo.toml
         )MMLTK" + tool(Command::run_traced_command) + R"MMLTK( cargo_fmt_frontend_iced \
             cargo fmt --manifest-path /workspace/src/frontend/iced/Cargo.toml --package mmltk-browser-app
)MMLTK" + configure_release + R"MMLTK(         )MMLTK" + tool(Command::run_traced_command) + R"MMLTK( ninja_build \
             cmake --build "${MMLTK_CONTAINER_CACHE_ROOT}/cmake/release" \
             --parallel "${MMLTK_JOBS}"
         )MMLTK" + tool(Command::check_toolchain_invariants) + R"MMLTK( \
             --repo-root /workspace --build-dir "${MMLTK_CONTAINER_CACHE_ROOT}/cmake/release")MMLTK";

inline const std::string protocol = R"MMLTK(set -euo pipefail
         cargo fetch --locked \
             --manifest-path /workspace/src/frontend/iced/Cargo.toml \
             --target x86_64-unknown-linux-gnu
)MMLTK" + configure_release + R"MMLTK(         cmake --build "${MMLTK_CONTAINER_CACHE_ROOT}/cmake/release" \
             --parallel "${MMLTK_JOBS}" \
             --target mmltk_protocol_v17_generation
         )MMLTK" + tool(Command::check_toolchain_invariants) + R"MMLTK( \
             --repo-root /workspace --build-dir "${MMLTK_CONTAINER_CACHE_ROOT}/cmake/release")MMLTK";

inline const std::string bindings = R"MMLTK(set -euo pipefail
         cargo fetch --locked \
             --manifest-path /workspace/src/frontend/iced/Cargo.toml \
             --target x86_64-unknown-linux-gnu
         )MMLTK" + tool(Command::configure_cmake_graph) + R"MMLTK( \
             release "${MMLTK_CONTAINER_CACHE_ROOT}/cmake/application-bindings-release" \
             -DBUILD_MMLTK_BROWSER_HOST=OFF \
             -DBUILD_MMLTK_FIREFOX_RUNTIME=OFF \
             -DMMLTK_BUILD_JOBS="${MMLTK_JOBS}" \
             -DMMLTK_FIREFOX_CACHE_ROOT="${MMLTK_FIREFOX_CACHE_ROOT}"
         cmake --build "${MMLTK_CONTAINER_CACHE_ROOT}/cmake/application-bindings-release" \
             --parallel "${MMLTK_JOBS}" \
             --target mmltk_protocol_v17_generation
         )MMLTK" + tool(Command::check_toolchain_invariants) + R"MMLTK( \
             --repo-root /workspace --build-dir "${MMLTK_CONTAINER_CACHE_ROOT}/cmake/application-bindings-release")MMLTK";

inline const std::string shiftlut = R"MMLTK(set -euo pipefail
         build="${MMLTK_CONTAINER_CACHE_ROOT}/cmake/shiftlut-export"
         if [[ "${MMLTK_SHIFTLUT_MODE}" == "--export-only" ]]; then
             )MMLTK" + tool(Command::configure_cmake_graph) + R"MMLTK( release "${build}" \
                 -DBUILD_MMLTK_BROWSER_HOST=OFF -DBUILD_MMLTK_FIREFOX_RUNTIME=OFF \
                 -DMMLTK_BUILD_JOBS="${MMLTK_JOBS}"
             cmake --build "${build}" --parallel "${MMLTK_JOBS}" \
                 --target mmltk_shiftlut_model_generator
         fi
         if [[ "${MMLTK_SHIFTLUT_MODE}" == "--verify-only" ]]; then
             build="${MMLTK_CONTAINER_CACHE_ROOT}/cmake/release"
         fi
         python3 /workspace/src/backend/imaging/upscale/export_shiftlut_lut_onnx.py \
             --source /shiftlut-source \
             --generator "${build}/mmltk_shiftlut_model_generator" \
             --output /workspace/src/backend/imaging/upscale/assets/ShiftLUT_fp32.onnx \
             --validation /workspace/build/validation/atlas-viewer-residency/shiftlut \
             "${MMLTK_SHIFTLUT_MODE}")MMLTK";

inline const std::string firefox_format = R"MMLTK(set -euo pipefail
         )MMLTK" + tool(Command::run_traced_command) + R"MMLTK( rustfmt_firefox_wgpu \
             rustfmt --edition 2021 \
             /workspace/third_party/firefox/gfx/wgpu_bindings/src/server.rs \
             /workspace/third_party/firefox/gfx/wgpu_bindings/src/mmltk_workspace_channel.rs)MMLTK";

inline const std::string gui_build = R"MMLTK(set -euo pipefail
         cargo fetch --locked \
             --manifest-path /workspace/src/frontend/iced/Cargo.toml \
             --target wasm32-unknown-unknown
         )MMLTK" + tool(Command::configure_cmake_graph) + R"MMLTK( \
             gui "${MMLTK_CONTAINER_CACHE_ROOT}/cmake/gui"
         )MMLTK" + tool(Command::run_traced_command) + R"MMLTK( cargo_fmt_iced_aw \
             cargo fmt --manifest-path /workspace/third_party/iced_aw/Cargo.toml
         )MMLTK" + tool(Command::run_traced_command) + R"MMLTK( cargo_fmt_frontend_iced \
             cargo fmt --manifest-path /workspace/src/frontend/iced/Cargo.toml --package mmltk-browser-app
         cmake --build "${MMLTK_CONTAINER_CACHE_ROOT}/cmake/gui" \
             --parallel "${MMLTK_JOBS}" --target mmltk_frontend_iced_check
         cmake --build "${MMLTK_CONTAINER_CACHE_ROOT}/cmake/gui" \
             --parallel "${MMLTK_JOBS}" --target mmltk_frontend_iced_bundle
         )MMLTK" + tool(Command::check_toolchain_invariants) + R"MMLTK( \
             --repo-root /workspace --build-dir "${MMLTK_CONTAINER_CACHE_ROOT}/cmake/gui")MMLTK";

inline const std::string package = R"MMLTK(set -euo pipefail
         next_stage="${MMLTK_CONTAINER_RELEASE_STAGE_ROOT}.next"
         rm -rf -- "${next_stage}"
         cmake --install "${MMLTK_CONTAINER_CACHE_ROOT}/cmake/release" \
             --prefix "${next_stage}"
         printf "%s\n" "${MMLTK_PACKAGE_FINGERPRINT}" >"${next_stage}/.mmltk-package-fingerprint")MMLTK";

inline const std::string shutdown = R"MMLTK(milliseconds_duration() { printf '%s.%03ds' "$(( $1 / 1000 ))" "$(( $1 % 1000 ))"; }

        signal_name="$1"
        grace_ticks="$2"
        runtime_pid="$(pgrep -o -f "[/]opt/mmltk/bin/mmltk-browser-host" 2>/dev/null || true)"
        if [ -z "${runtime_pid}" ]; then
            exit 0
        fi

        signal_runtime() {
            signal_name="$1"
            kill "-${signal_name}" "${runtime_pid}" 2>/dev/null || true
        }

        wait_runtime() {
            wait_ticks="$1"
            wait_milliseconds=$((wait_ticks * 100))
            if ! pgrep -f "[/]opt/mmltk/bin/mmltk-browser-host" >/dev/null 2>&1; then
                return 0
            fi
            if timeout --foreground --signal=KILL "$(milliseconds_duration "${wait_milliseconds}")" \
                    pidwait -o -f "[/]opt/mmltk/bin/mmltk-browser-host"; then
                return 0
            fi
            ! pgrep -f "[/]opt/mmltk/bin/mmltk-browser-host" >/dev/null 2>&1
        }

        signal_runtime "${signal_name}"
        if wait_runtime "${grace_ticks}"; then
            exit 0
        fi
        signal_runtime TERM
        if wait_runtime 10; then
            exit 0
        fi
        signal_runtime KILL
        wait_runtime 10 || true
        exit 1
    )MMLTK";

inline const std::string gpu_compile = R"MMLTK(
            torch_flags=()
            if [[ "${MMLTK_DIAGNOSTIC_TORCH}" == 1 ]]; then
                torch_root=$(python3 -c "import torch; print(torch.__path__[0])") || exit
                torch_abi=$(python3 -c "import torch; print(int(torch._C._GLIBCXX_USE_CXX11_ABI))") || exit
                torch_flags=(-isystem "${torch_root}/include" -isystem "${torch_root}/include/torch/csrc/api/include"
                    -D_GLIBCXX_USE_CXX11_ABI="${torch_abi}" -L"${torch_root}/lib"
                    -L"${CUDA_HOME}/lib64" -ltorch -ltorch_cpu -ltorch_cuda -lc10 -lc10_cuda -lcudart)
            fi
            exec /opt/gcc-16.2/bin/g++ -std=c++26 -freflection -O2 -g -Wall -Wextra \
                -Wno-missing-field-initializers \
                -I"${CUDA_HOME}/include" "/diagnostic/source/${MMLTK_DIAGNOSTIC_SOURCE}" \
                -L"${CUDA_HOME}/lib64/stubs" -lcuda -lvulkan \
                "${torch_flags[@]}" \
                -o /diagnostic/output/probe
        )MMLTK";

inline const std::string contract = R"MMLTK(
        test -x /opt/mmltk/bin/mmltk-browser-host &&
        test -x /opt/mmltk/bin/mmltk-browser-bundle-contract &&
        test -x /opt/mmltk/lib/mmltk/firefox/firefox &&
        test -r "$1/index.html" &&
        test -s "$1/browser_protocol.marker"
    )MMLTK";

inline const std::string firefox_lock = R"MMLTK(set -euo pipefail
             cd /workspace/third_party/firefox
             cargo --config .cargo/config.toml.in metadata \
                 --manifest-path Cargo.toml --offline \
                 --format-version 1 >/dev/null)MMLTK";

inline const std::string gui_lock = R"MMLTK(set -euo pipefail
             cargo fetch \
                 --manifest-path /workspace/src/frontend/iced/Cargo.toml \
                 --target x86_64-unknown-linux-gnu)MMLTK";

inline const std::string test_help = R"MMLTK(Test suites
  application-systems  Independent application system suites
  application-contracts  Isolated application domain and service contract suite
  transport  Isolated browser physical transport and attachment suite
  browser-runtime  Isolated Firefox process-owner and shell shutdown suite
  workspace-wayland  Opt-in packaged Wayland/NVIDIA browser hardware suite
  cuda-vulkan     Standalone CUDA/Vulkan FD, timeline, pixel and process-exit diagnostic (no browser)
  headless-compositor  Verify the NVIDIA headless Wayland compositor and protocols
  headless-compositor-tool  Headless session ownership and failure-handling fixture suite
  browser-app  Iced client protocol, state, and transport unit suite
  cleanup-tool  Cleanup, declaration classification, and formatter fixture suite
  log-query-tool  Runtime log parser, query, correlation, and output fixture suite
  core    Core dataset/runtime/compiler Catch2 suite
  gui     Unavailable in the native Catch2 suite
  tsan    Unavailable in the native Catch2 suite
  rfdetr  RF-DETR native/model Catch2 suite
  rfdetr-profile  Opt-in RF-DETR training profile runner
  data-profile    Opt-in CPU compile/resample and CUDA loader profile runner
  build   Build every available native release test target without running tests
  all     Build native targets; run ordinary native, browser-app, cleanup-tool, log-query, and compositor suites

Use: ./mmltk --test <suite> [test options] [-- <Catch2 args...>]
For browser-app, arguments after -- are Cargo test arguments.
  Prefix --js-test-name-pattern <pattern> to select JavaScript cases, or
  --rust-package <package> to run one Rust package without JavaScript.
For cleanup-tool, use -- --test-name-pattern PATTERN to rerun selected native cases.
For log-query-tool, arguments after -- select Class.test_method cases.
For headless-compositor, arguments after -- run a command in the private compositor.
For cuda-vulkan, arguments after -- are width height allocation-pairs transfers validation context-mode memory-owner process-mode; use -- --help.
Test options:
  --executable <target>       Build and run one executable owned by the suite
  --config <release|dev>      Select the containerized CMake graph
  --env <NAME[=VALUE]>        Forward an additional environment variable
  --headless-compositor      Run workspace-wayland in a private NVIDIA GL Weston session
  --gdb                       Run the selected executable through containerized GDB
  --gdb-command <command>     Add an ordered GDB command (repeatable)
)MMLTK";

inline const std::string oracle_help = R"MMLTK(Usage: ./mmltk --diagnose-rfdetr-upstream --upstream PATH --output PATH [--sdpa default|math]
       ./mmltk --diagnose-rfdetr-upstream --build-only
--build-only builds the isolated native oracle image without weights, upstream checkout, or numerical execution.
Requires clean upstream e9a138f70cc14e3fddca029a0683055e34ab8f35 and cached official Nano weights.
Builds a validation-only cached image from the pinned full NGC donor.
Executes upstream Nano/SegNano FP32 and supported AMP evidence, then core/training [upstream_oracle] cases.
--sdpa math selects ATen math attention on both sides; default retains accelerated backend selection.
Gates matched-state forward/objectives, complete derivative vectors and identical-gradient AdamW.
Reports per-tensor and independent-trajectory differences separately; success does not certify accuracy or identical training trajectories.
Exit zero requires both stages. References, comparison JSONL and wrapper.log survive comparison failures.
Output must be a fresh directory; retry comparisons with --test ... --env MMLTK_RFDETR_UPSTREAM_ORACLE_DIR=PATH.
The upstream mount is read-only. Producer deadline: two hours; signals use bounded wrapper cancellation.
)MMLTK";

inline const std::string gpu_environment_help = R"MMLTK(Usage: ./mmltk --diagnose-gpu-environment [runtime|wayland-validation|development]
Default: runtime (MMLTK_IMAGE). Other selections reuse existing wrapper images.
Print bounded JSON for the container, kernel, NVIDIA driver/GPU identities,
CUDA/Vulkan libraries and ICD manifests, and explicit virtualization evidence.
Read-only, no network, no image builds/pulls, no application or GPU workload.
)MMLTK";

inline const std::string process_help = R"MMLTK(Usage: ./mmltk --diagnose-processes [WRAPPER_MODE] [--backtrace EXECUTABLE | --terminate EXECUTABLE | --read-file EXECUTABLE --file CONTAINER_PATH] [--pid CONTAINER_PID]
Read-only process snapshots for running wrapper containers owned by this repository.
Shows process names, elapsed/CPU time, CPU/memory use, state and wait channel; omits arguments and environment.
Default snapshots do not alter containers; each daemon query has a 15-second deadline.
--backtrace briefly attaches GDB to one exact native executable, prints thread stacks, and detaches.
--read-file prints an escaped, bounded retained artifact from that process container; maximum 4 MiB.
--pid selects one container PID when the exact executable has several workers.
--terminate sends SIGTERM to that exact executable so a stuck test can fail and the suite can continue.
)MMLTK";

inline const std::string symbols_help = R"MMLTK(Usage: ./mmltk --diagnose-native-symbols [--match REGEX] [--limit N] [--address HEX ...] ARTIFACT...
Inspect repository-relative or installed /opt dependency .a/.o files with the existing development image GCC nm.
Repeat --address HEX with one ELF artifact to resolve crash addresses using addr2line.
Read-only JSONL, no builds, pulls, network, or GPU execution; 120-second deadline.
)MMLTK";

inline const std::string link_help = R"MMLTK(Usage: ./mmltk --diagnose-native-link [--graph release|analysis] [--no-save-temps] [--linker-directory DIR] [--trace-symbol SYMBOL] TARGET
Repeat one cached native link with saved LTO intermediates and a linker map (default: release).
Uses the existing development image; repository remains read-only.
Artifacts go under build/diagnostics; no image builds, pulls, or network; 120-second deadline.
)MMLTK";

inline const std::string gpu_program_help = R"MMLTK(Usage: ./mmltk --diagnose-gpu-program [--torch] SOURCE.cpp [PROGRAM_ARGS...]
Build one standalone C++26 CUDA-driver/Vulkan diagnostic in the existing development image.
--torch adds the pinned PyTorch headers, ABI setting, and native libraries.
Run it in the existing Wayland validation image, without starting the application or compositor.
Artifacts go under build/diagnostics; no image builds, pulls, or network.
Compilation and execution each have a 120-second deadline.
)MMLTK";

} // namespace mmltk::container_commands

}
