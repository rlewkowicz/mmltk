
//              Copyright Catch2 Authors
// Distributed under the Boost Software License, Version 1.0.
//   (See accompanying file LICENSE.txt or copy at
//        https://www.boost.org/LICENSE_1_0.txt)

// SPDX-License-Identifier: BSL-1.0
#ifndef CATCH_ISTREAM_HPP_INCLUDED
#define CATCH_ISTREAM_HPP_INCLUDED

#include <catch2/internal/catch_noncopyable.hpp>
#include <catch2/internal/catch_unique_ptr.hpp>

#include <iosfwd>
#include <string>

namespace Catch {

class IStream {
   public:
    virtual ~IStream();
    virtual std::ostream& stream() = 0;
    virtual bool isConsole() const {
        return false;
    }
};

auto makeStream(std::string const& filename) -> Detail::unique_ptr<IStream>;

}  // namespace Catch

#endif  // CATCH_STREAM_HPP_INCLUDED
