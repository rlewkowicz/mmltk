#include "src/backend/data/benchmark/detail/benchmark_trace.h"
#include "src/backend/data/detail/compile_execution.h"
#include "src/backend/data/detail/compile_resources.h"
namespace mmltk::backend::data::benchmark_internal {
namespace reflection = mmltk::frameworks::reflection;
nlohmann::json benchmark_observation_fields(const detail::CompileAdmissionObservation& observation) {
 nlohmann::json result = nlohmann::json::object();
 reflection::visit_materialized_members<detail::CompileAdmissionObservation>([&]<class Declaration>(const auto& field) {
  const auto& value = observation.*Declaration::pointer;
  if constexpr (std::same_as<typename Declaration::member_type, detail::CompileResources>) {
   reflection::visit_materialized_members<detail::CompileResources>([&]<class Resource>(const auto&) {
    // The retained flat trace has three semantic aliases; every other key and
    // value follows the canonical request without a second field inventory.
    constexpr auto key = [] {
     constexpr auto name = reflection::materialized_member_name<Resource::pointer>();
     if constexpr (name == reflection::materialized_member_name<&detail::CompileResources::bytes>())
      return std::string_view{"requested_bytes"};
     else if constexpr (name == reflection::materialized_member_name<&detail::CompileResources::workspace_window>())
      return std::string_view{"window"};
     else if constexpr (name == reflection::materialized_member_name<&detail::CompileResources::workspace_class>())
      return std::string_view{"work_class"};
     else
      return name;
    }();
    result[key] = value.*Resource::pointer;
   });
  } else {
   result[field.member_name] = value;
  }
 });
 return result;
}
void trace_compile_execution(const BenchmarkTraceSink& sink, std::string_view event, const detail::CompileExecutionSnapshot& value) noexcept {
 trace_benchmark_event(sink, event, [&] {
  if (value.reservation) {
   const auto& request = *value.reservation;
   nlohmann::json result{
    {"file", request.location.file_name()}, {"line", request.location.line()}, {"function", request.location.function_name()}, {"requested_bytes", request.requested.bytes}, {"denial", request.denial},
    {"resource_epoch", request.resource_epoch}, {"records", value.records}, {"record_limit", value.record_limit}, {"largest_eligible_extent", request.largest_eligible_extent},
    {"costs", benchmark_observation_fields(value.costs)}, {"requested_descriptors", request.requested.descriptors}, {"requested_continuation", request.requested.continuation_descriptors},
    {"producer", request.requested.producer}, {"held_bytes", value.resources.bytes}, {"byte_target", value.byte_target}, {"held_descriptors", value.resources.descriptors},
    {"committed_descriptors", value.resources.continuation_descriptors}, {"descriptor_limit", value.descriptor_limit}, {"parents", nlohmann::json::array()}, {"owners", nlohmann::json::array()}
   };
   for (const auto& credit : request.parents)
    result["parents"].push_back(
     {{"bytes", credit.resources.bytes}, {"descriptors", credit.resources.descriptors}, {"available", credit.available}, {"borrowed", credit.borrowed},
      {"descriptors_retired", credit.descriptors_retired}, {"continuations_released", credit.continuations_released}});
   for (const auto& credit : request.owners)
    result["owners"].push_back(
     {{"id", credit.id}, {"parent", credit.parent}, {"file", credit.location.file_name()}, {"line", credit.location.line()}, {"bytes", credit.resources.bytes},
      {"descriptors", credit.resources.descriptors}, {"available", credit.available}, {"borrowed", credit.borrowed}});
   return result;
  }
  auto demands = nlohmann::json::array();
  for (const auto& demand : value.demands) demands.push_back(benchmark_observation_fields(demand));
  return nlohmann::json{
   {"arena", benchmark_observation_fields(value.arena)}, {"costs", benchmark_observation_fields(value.costs)}, {"demands", std::move(demands)}, {"task_body_ns", value.task_body_ns},
   {"task_cpu_ns", value.task_cpu_ns}, {"ready_to_start_ns", value.ready_to_start_ns}, {"live", value.live}, {"baseline", value.baseline}, {"elastic", value.elastic}, {"ready", value.ready},
   {"parked", value.parked}, {"running", value.running}, {"active", value.active}, {"external", value.external}, {"services", value.services}, {"bytes", value.resources.bytes},
   {"byte_target", value.byte_target}, {"normal_locked", value.normal_locked}, {"resident", value.resident}, {"leased", value.leased}, {"exceptional", value.exceptional},
   {"exceptional_peak", value.exceptional_peak}, {"opaque_library", value.opaque_library}, {"protected_workspace", value.protected_workspace}, {"shared_workspace", value.shared_workspace},
   {"compaction_workspace", value.compaction_workspace}, {"exceptional_reserved", value.exceptional_reserved}, {"workspace_reuses", value.workspace_reuses},
   {"physical_retirements", value.physical_retirements}, {"descriptors", value.resources.descriptors}, {"descriptor_limit", value.descriptor_limit},
   {"descriptor_promises", value.resources.continuation_descriptors}, {"records", value.records}, {"record_limit", value.record_limit}, {"admitted", value.admitted}, {"completed", value.completed},
   {"task_completed", value.task_completed}, {"task_duration_ns", value.task_duration_ns}, {"task_max_duration_ns", value.task_max_duration_ns}
  };
 });
}
}  // namespace mmltk::backend::data::benchmark_internal
