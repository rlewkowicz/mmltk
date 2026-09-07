#pragma once

#include <cstdint>
#include <memory>
#include <string_view>
#include <utility>

#include "src/controller/contracts/diagnostic_context.h"
#include "src/controller/services/diagnostics_client.h"
#include "src/frameworks/serialization/cbor_wire.h"

namespace mmltk::controller::services {

enum class RuntimeDiagnosticOwner : std::uint8_t {
    BrowserRuntime,
    BrowserServer,
    FirefoxProcess,
    Explore,
    Annotation,
    Upscale,
    Live,
    Presentation,
    AnnotationResource,
};

struct RuntimeDiagnosticFact final {
    RuntimeDiagnosticOwner owner = RuntimeDiagnosticOwner::BrowserRuntime;
    std::string_view event;
    std::string_view participant{};
    std::uint64_t sequence = 0U;
    std::uint64_t value = 0U;
    std::uint64_t detail = 0U;
    std::int64_t device = -1;
    contracts::DiagnosticContext context{};
    std::string_view message{};
};

class RuntimeDiagnostics;

// An optional, effect-only trace capability. Its shared private state owns the
// producer lifetime used by every system and worker copy; callers cannot install
// borrowed contexts or alternate callback vocabularies.
class RuntimeDiagnosticTarget final {
   public:
    RuntimeDiagnosticTarget() noexcept = default;

    [[nodiscard]] bool valid() const noexcept;
    void write(RuntimeDiagnosticFact fact) const noexcept;
    void write_browser_event(std::string_view event, const mmltk::frameworks::serialization::wire::Value& fields) const noexcept;
    [[nodiscard]] bool benchmark_trace_enabled() const noexcept;
    void write_benchmark_trace(std::string_view event, std::string_view json_fields) const noexcept;

   private:
    struct State;
    explicit RuntimeDiagnosticTarget(std::shared_ptr<State> state) noexcept : state_(std::move(state)) {}
    std::shared_ptr<State> state_;
    friend class RuntimeDiagnostics;
};

// Formats the shared runtime JSONL vocabulary into the application-owned,
// bounded DiagnosticsClient. A disabled client yields an empty target, so
// transport and process paths perform no trace collection or formatting.
class RuntimeDiagnostics final {
   public:
    explicit RuntimeDiagnostics(DiagnosticsProducer producer) noexcept;

    [[nodiscard]] RuntimeDiagnosticTarget target() noexcept;
    void write(RuntimeDiagnosticFact fact) noexcept;
    void write_browser_event(std::string_view event, const mmltk::frameworks::serialization::wire::Value& fields) noexcept;
    void write_benchmark_trace(std::string_view event, std::string_view json_fields) noexcept;

   private:
    static void Submit(void* context, RuntimeDiagnosticFact fact) noexcept;
    static void SubmitBrowserEvent(void* context, std::string_view event,
                                   const mmltk::frameworks::serialization::wire::Value& fields) noexcept;
    static void SubmitBenchmarkTrace(void* context, std::string_view event, std::string_view json_fields) noexcept;

    std::shared_ptr<RuntimeDiagnosticTarget::State> state_;
};

}  // namespace mmltk::controller::services
