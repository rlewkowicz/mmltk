#pragma once
#include "src/controller/contracts/application_systems.h"
#include <functional>
#include <type_traits>
#include <utility>
#include <variant>
#include "src/controller/browser/application_materializer.h"
#include "src/controller/presentation/visual_system_types.h"
namespace mmltk::controller::browser {
template <auto Member, class Composition = ApplicationSystems>
class ApplicationEventPublisher final {
public:
 using Sink = std::function<void(SystemEvent)>;
 using ContinuitySink = std::function<void()>;
 using SourceSink = std::function<void(PresentationSourceIdentity)>;
 ApplicationEventPublisher(Sink& sink, ContinuitySink continuity, SourceSink source = {})
     : sink_(sink), continuity_(std::move(continuity)), source_(std::move(source)) {}
 template <class Variant>
 void operator()(const Variant& event) const noexcept {
  std::visit(
   [this]<class Event>(const Event& value) noexcept {
    if constexpr (application_schema_detail::annotation_count<^^Event, contracts::reflection::Event>() != 0U) {
     using Descriptor = ApplicationEventDescriptor<Composition, Member, Event>;
     try {
      using System = std::remove_pointer_t<std::remove_cvref_t<decltype(std::declval<Composition>().*Member)>>;
      if constexpr (requires { typename System::visual_source; }) {
       using Projection = typename System::visual_source;
       if (source_) source_({Projection::kind, 1U});
      }
      if (sink_) sink_(encode_system_event<Member, Event, Composition>(value));
     } catch (...) {
      if constexpr (Descriptor::delivery != contracts::reflection::EventDelivery::Transient) {
       try {
        if (continuity_) continuity_();
       } catch (...) {}
      }
     }
    }
   },
   event);
 }

private:
 Sink& sink_;
 ContinuitySink continuity_;
 SourceSink source_;
};
}  // namespace mmltk::controller::browser
