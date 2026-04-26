
//              Copyright Catch2 Authors
// Distributed under the Boost Software License, Version 1.0.
//   (See accompanying file LICENSE.txt or copy at
//        https://www.boost.org/LICENSE_1_0.txt)

// SPDX-License-Identifier: BSL-1.0
#ifndef CATCH_TEST_CASE_TRACKER_HPP_INCLUDED
#define CATCH_TEST_CASE_TRACKER_HPP_INCLUDED

#include <catch2/internal/catch_lifetimebound.hpp>
#include <catch2/internal/catch_source_line_info.hpp>
#include <catch2/internal/catch_unique_ptr.hpp>
#include <catch2/internal/catch_stringref.hpp>

#include <string>
#include <vector>

namespace Catch {

struct PathFilter;

namespace TestCaseTracking {

struct NameAndLocation {
    std::string name;
    SourceLineInfo location;

    NameAndLocation(std::string&& _name, SourceLineInfo const& _location);
    friend bool operator==(NameAndLocation const& lhs, NameAndLocation const& rhs) {
        if (lhs.location.line != rhs.location.line) {
            return false;
        }
        return lhs.name == rhs.name && lhs.location == rhs.location;
    }
    friend bool operator!=(NameAndLocation const& lhs, NameAndLocation const& rhs) {
        return !(lhs == rhs);
    }
};

struct NameAndLocationRef {
    StringRef name;
    SourceLineInfo location;

    constexpr NameAndLocationRef(StringRef name_ CATCH_ATTR_LIFETIMEBOUND, SourceLineInfo location_)
        : name(name_), location(location_) {}

    friend bool operator==(NameAndLocation const& lhs, NameAndLocationRef const& rhs) {
        if (lhs.location.line != rhs.location.line) {
            return false;
        }
        return StringRef(lhs.name) == rhs.name && lhs.location == rhs.location;
    }
    friend bool operator==(NameAndLocationRef const& lhs, NameAndLocation const& rhs) {
        return rhs == lhs;
    }
};

class ITracker;

using ITrackerPtr = Catch::Detail::unique_ptr<ITracker>;

class ITracker {
    NameAndLocation m_nameAndLocation;

    using Children = std::vector<ITrackerPtr>;

   protected:
    enum CycleState {
        NotStarted,
        Executing,
        ExecutingChildren,
        NeedsAnotherRun,
        CompletedSuccessfully,
        Failed
    };

    ITracker* m_parent = nullptr;
    Children m_children;
    CycleState m_runState = NotStarted;

    std::vector<PathFilter> const* m_filterRef = nullptr;

    size_t m_allTrackerDepth = static_cast<size_t>(-2);
    size_t m_sectionOnlyDepth = static_cast<size_t>(-2);
    bool m_newStyleFilters = false;

   public:
    ITracker(NameAndLocation&& nameAndLoc, ITracker* parent);

    NameAndLocation const& nameAndLocation() const {
        return m_nameAndLocation;
    }
    ITracker* parent() const {
        return m_parent;
    }

    virtual ~ITracker();

    virtual bool isComplete() const = 0;
    bool isSuccessfullyCompleted() const {
        return m_runState == CompletedSuccessfully;
    }
    bool isOpen() const;
    bool hasStarted() const;

    void setFilters(std::vector<PathFilter> const* filters, bool newStyleFilters) {
        m_filterRef = filters;
        m_newStyleFilters = newStyleFilters;
    }

    virtual void close() = 0;
    virtual void fail() = 0;
    void markAsNeedingAnotherRun();

    void addChild(ITrackerPtr&& child);
    ITracker* findChild(NameAndLocationRef const& nameAndLocation);
    bool hasChildren() const {
        return !m_children.empty();
    }

    void openChild();

    virtual bool isSectionTracker() const;
    virtual bool isGeneratorTracker() const;
};

class TrackerContext {
    enum RunState {
        NotStarted,
        Executing,
        CompletedCycle
    };

    ITrackerPtr m_rootTracker;
    ITracker* m_currentTracker = nullptr;
    RunState m_runState = NotStarted;

   public:
    ITracker& startRun();

    void startCycle() {
        m_currentTracker = m_rootTracker.get();
        m_runState = Executing;
    }
    void completeCycle();

    bool completedCycle() const;
    ITracker& currentTracker() {
        return *m_currentTracker;
    }
    void setCurrentTracker(ITracker* tracker);
};

class TrackerBase : public ITracker {
   protected:
    TrackerContext& m_ctx;

   public:
    TrackerBase(NameAndLocation&& nameAndLocation, TrackerContext& ctx, ITracker* parent);

    bool isComplete() const override;

    void open();

    void close() override;
    void fail() override;

   private:
    void moveToParent();
    void moveToThis();
};

class SectionTracker final : public TrackerBase {
    StringRef m_trimmed_name;

   public:
    SectionTracker(NameAndLocation&& nameAndLocation, TrackerContext& ctx, ITracker* parent);

    bool isSectionTracker() const override;

    bool isComplete() const override;

    static SectionTracker& acquire(TrackerContext& ctx, NameAndLocationRef const& nameAndLocation);

    void tryOpen();

    StringRef trimmedName() const;
};

}  // namespace TestCaseTracking

using TestCaseTracking::ITracker;
using TestCaseTracking::TrackerContext;
using TestCaseTracking::SectionTracker;

}  // namespace Catch

#endif  // CATCH_TEST_CASE_TRACKER_HPP_INCLUDED
