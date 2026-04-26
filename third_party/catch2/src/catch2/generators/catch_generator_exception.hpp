
//              Copyright Catch2 Authors
// Distributed under the Boost Software License, Version 1.0.
//   (See accompanying file LICENSE.txt or copy at
//        https://www.boost.org/LICENSE_1_0.txt)

// SPDX-License-Identifier: BSL-1.0
#ifndef CATCH_GENERATOR_EXCEPTION_HPP_INCLUDED
#define CATCH_GENERATOR_EXCEPTION_HPP_INCLUDED

#include <exception>

namespace Catch {

class GeneratorException : public std::exception {
    const char* const m_msg = "";

   public:
    GeneratorException(const char* msg) : m_msg(msg) {}

    const char* what() const noexcept final;
};

}  // namespace Catch

#endif  // CATCH_GENERATOR_EXCEPTION_HPP_INCLUDED
