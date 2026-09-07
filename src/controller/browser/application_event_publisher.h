#pragma once

#include <functional>
#include <utility>
#include <variant>

#include "src/controller/browser/application_materializer.h"

namespace mmltk::controller::browser {

template <auto Member, class Composition = ApplicationSystems>
class ApplicationEventPublisher final {
   public:
    using Sink = std::function<void(SystemEvent)>;
    using ContinuitySink = std::function<void()>;

    ApplicationEventPublisher(Sink& sink, ContinuitySink continuity)
        : sink_(sink), continuity_(std::move(continuity)) {}

    template <class Variant>
    void operator()(const Variant& event) const noexcept {
        if (!sink_) return;
        std::visit([this]<class Event>(const Event& value) noexcept {
            using Descriptor = ApplicationEventDescriptor<Composition, Member, Event>;
            try {
                sink_(encode_system_event<Member, Event, Composition>(value));
            } catch (...) {
                if constexpr (Descriptor::delivery != contracts::reflection::EventDelivery::Transient) {
                    try {
                        if (continuity_) continuity_();
                    } catch (...) {}
                }
            }
        }, event);
    }

   private:
    Sink& sink_;
    ContinuitySink continuity_;
};

}  // namespace mmltk::controller::browser
