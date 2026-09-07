

#ifndef CATCH_REPORTER_EVENT_LISTENER_HPP_INCLUDED
#define CATCH_REPORTER_EVENT_LISTENER_HPP_INCLUDED

#include <catch2/interfaces/catch_interfaces_reporter.hpp>
#include <catch2/reporters/catch_reporter_event_list.hpp>

namespace Catch {

class EventListenerBase : public IEventListener {
   public:
    using IEventListener::IEventListener;

    CATCH_REPORTER_ALL_EVENTS(CATCH_REPORTER_DECLARE_EVENT1, CATCH_REPORTER_DECLARE_EVENT2)
};

}

#endif  // CATCH_REPORTER_EVENT_LISTENER_HPP_INCLUDED
