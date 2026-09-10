#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <utility>

#include "src/controller/contracts/diagnostic_context.h"
#include "src/controller/services/diagnostics_client.h"
#include "src/frameworks/serialization/cbor_wire.h"
#include "src/frameworks/reflection/reflected_field_policy.h"

namespace mmltk::controller::services {

struct RuntimeDiagnosticFact final {
    contracts::DiagnosticOwner owner = contracts::DiagnosticOwner::BrowserRuntime;
    std::string_view event;
    std::string_view participant{};
    std::uint64_t sequence = 0U;
    std::uint64_t value = 0U;
    std::uint64_t detail = 0U;
    std::int64_t device = -1;
    contracts::DiagnosticContext context{};
    std::string_view message{};
};
MMLTK_REFLECT_FIELDS(RuntimeDiagnosticFact)

enum class RuntimeDiagnosticDelivery : std::uint8_t { BestEffort, Complete };

class RuntimeDiagnostics;

// An optional, effect-only trace capability. Its shared private state owns the
// producer lifetime used by every system and worker copy; callers cannot install
// borrowed contexts or alternate callback vocabularies.
class RuntimeDiagnosticTarget final {
   public:
    RuntimeDiagnosticTarget() noexcept = default;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] bool pixel_probes_enabled() const noexcept;
    void operator()(RuntimeDiagnosticFact fact) const noexcept { write(fact); }
    template <class Factory>
    void Emit(Factory&& factory) const noexcept {
        auto make_fact = [&]() -> RuntimeDiagnosticFact { return std::forward<Factory>(factory)(); };
        emit(&make_fact, [](void* context) { return (*static_cast<decltype(make_fact)*>(context))(); });
    }
    void write(RuntimeDiagnosticFact fact) const noexcept;
    void write_batch(std::span<const RuntimeDiagnosticFact> facts) const noexcept;
    // Seals diagnostic ingress and reserves one final owner fact without
    // waiting for queue capacity, immediately before the owner flushes.
    void write_required(RuntimeDiagnosticFact fact) const noexcept;
    void write_browser_event(std::string_view event, const mmltk::frameworks::serialization::wire::Value& fields) const noexcept;
    [[nodiscard]] bool benchmark_trace_enabled() const noexcept;
    void write_benchmark_trace(std::string_view event, std::string_view json_fields) const noexcept;

   private:
    using FactFactory = RuntimeDiagnosticFact (*)(void*);
    void emit(void* context, FactFactory factory) const noexcept;
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
    explicit RuntimeDiagnostics(DiagnosticsProducer producer, bool pixel_probes = false,
                                RuntimeDiagnosticDelivery delivery = RuntimeDiagnosticDelivery::BestEffort);

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
