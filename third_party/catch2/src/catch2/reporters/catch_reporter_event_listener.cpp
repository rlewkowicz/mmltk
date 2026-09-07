

#include <catch2/reporters/catch_reporter_event_listener.hpp>

namespace Catch {

#define CATCH_EVENT_LISTENER_NOOP1(name, Arg1Type, arg1) \
    void EventListenerBase::name(Arg1Type) {}
#define CATCH_EVENT_LISTENER_NOOP2(name, Arg1Type, arg1, Arg2Type, arg2) \
    void EventListenerBase::name(Arg1Type, Arg2Type) {}

CATCH_REPORTER_ALL_EVENTS(CATCH_EVENT_LISTENER_NOOP1, CATCH_EVENT_LISTENER_NOOP2)

#undef CATCH_EVENT_LISTENER_NOOP1
#undef CATCH_EVENT_LISTENER_NOOP2

}
