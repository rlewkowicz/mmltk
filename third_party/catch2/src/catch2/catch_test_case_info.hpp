
//              Copyright Catch2 Authors
// Distributed under the Boost Software License, Version 1.0.
//   (See accompanying file LICENSE.txt or copy at
//        https://www.boost.org/LICENSE_1_0.txt)

// SPDX-License-Identifier: BSL-1.0
#ifndef CATCH_TEST_CASE_INFO_HPP_INCLUDED
#define CATCH_TEST_CASE_INFO_HPP_INCLUDED

#include <catch2/interfaces/catch_interfaces_test_invoker.hpp>
#include <catch2/internal/catch_source_line_info.hpp>
#include <catch2/internal/catch_noncopyable.hpp>
#include <catch2/internal/catch_stringref.hpp>
#include <catch2/internal/catch_unique_ptr.hpp>

#include <cstdint>
#include <string>
#include <vector>

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wpadded"
#endif

namespace Catch {

struct Tag {
    constexpr Tag(StringRef original_) : original(original_) {}
    StringRef original;

    friend bool operator<(Tag const& lhs, Tag const& rhs);
    friend bool operator==(Tag const& lhs, Tag const& rhs);
};

class ITestInvoker;
struct NameAndTags;

enum class TestCaseProperties : uint8_t {
    None = 0,
    IsHidden = 1 << 1,
    ShouldFail = 1 << 2,
    MayFail = 1 << 3,
    Throws = 1 << 4,
    NonPortable = 1 << 5,
    Benchmark = 1 << 6
};

struct TestCaseInfo : Detail::NonCopyable {
    TestCaseInfo(StringRef _className, NameAndTags const& _nameAndTags, SourceLineInfo const& _lineInfo);

    bool isHidden() const;
    bool throws() const;
    bool okToFail() const;
    bool expectedToFail() const;

    void addFilenameTag();

    friend bool operator<(TestCaseInfo const& lhs, TestCaseInfo const& rhs);

    std::string tagsAsString() const;

    std::string name;
    StringRef className;

   private:
    std::string backingTags;
    void internalAppendTag(StringRef tagString);

   public:
    std::vector<Tag> tags;
    SourceLineInfo lineInfo;
    TestCaseProperties properties = TestCaseProperties::None;
};

class TestCaseHandle {
    TestCaseInfo* m_info;
    ITestInvoker* m_invoker;

   public:
    constexpr TestCaseHandle(TestCaseInfo* info, ITestInvoker* invoker) : m_info(info), m_invoker(invoker) {}

    void prepareTestCase() const {
        m_invoker->prepareTestCase();
    }

    void tearDownTestCase() const {
        m_invoker->tearDownTestCase();
    }

    void invoke() const {
        m_invoker->invoke();
    }

    constexpr TestCaseInfo const& getTestCaseInfo() const {
        return *m_info;
    }
};

Detail::unique_ptr<TestCaseInfo> makeTestCaseInfo(StringRef className, NameAndTags const& nameAndTags,
                                                  SourceLineInfo const& lineInfo);
}  // namespace Catch

#ifdef __clang__
#pragma clang diagnostic pop
#endif

#endif  // CATCH_TEST_CASE_INFO_HPP_INCLUDED
