
//              Copyright Catch2 Authors
// Distributed under the Boost Software License, Version 1.0.
//   (See accompanying file LICENSE.txt or copy at
//        https://www.boost.org/LICENSE_1_0.txt)

// SPDX-License-Identifier: BSL-1.0
#ifndef CATCH_REPORTER_COMMON_BASE_HPP_INCLUDED
#define CATCH_REPORTER_COMMON_BASE_HPP_INCLUDED

#include <catch2/interfaces/catch_interfaces_reporter.hpp>

#include <map>
#include <string>

namespace Catch {
class ColourImpl;

class ReporterBase : public IEventListener {
   protected:
    Detail::unique_ptr<IStream> m_wrapped_stream;
    std::ostream& m_stream;
    Detail::unique_ptr<ColourImpl> m_colour;
    std::map<std::string, std::string> m_customOptions;

   public:
    ReporterBase(ReporterConfig&& config);
    ~ReporterBase() override;

    void listReporters(std::vector<ReporterDescription> const& descriptions) override;
    void listListeners(std::vector<ListenerDescription> const& descriptions) override;
    void listTests(std::vector<TestCaseHandle> const& tests) override;
    void listTags(std::vector<TagInfo> const& tags) override;
};
}  // namespace Catch

#endif  // CATCH_REPORTER_COMMON_BASE_HPP_INCLUDED
