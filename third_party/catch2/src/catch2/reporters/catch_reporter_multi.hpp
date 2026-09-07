

#ifndef CATCH_REPORTER_MULTI_HPP_INCLUDED
#define CATCH_REPORTER_MULTI_HPP_INCLUDED

#include <catch2/interfaces/catch_interfaces_reporter.hpp>
#include <catch2/reporters/catch_reporter_event_list.hpp>

namespace Catch {

class MultiReporter final : public IEventListener {
    std::vector<IEventListenerPtr> m_reporterLikes;
    bool m_haveNoncapturingReporters = false;

    std::vector<IEventListenerPtr>::difference_type m_insertedListeners = 0;

    void updatePreferences(IEventListener const& reporterish);

   public:
    MultiReporter(IConfig const* config) : IEventListener(config) {
        m_preferences.shouldReportAllAssertionStarts = false;
    }

    using IEventListener::IEventListener;

    void addListener(IEventListenerPtr&& listener);
    void addReporter(IEventListenerPtr&& reporter);

   public:
    CATCH_REPORTER_ALL_EVENTS(CATCH_REPORTER_DECLARE_EVENT1, CATCH_REPORTER_DECLARE_EVENT2)
};

}

#endif  // CATCH_REPORTER_MULTI_HPP_INCLUDED
