
//              Copyright Catch2 Authors
// Distributed under the Boost Software License, Version 1.0.
//   (See accompanying file LICENSE.txt or copy at
//        https://www.boost.org/LICENSE_1_0.txt)

// SPDX-License-Identifier: BSL-1.0
#ifndef CATCH_INTERFACES_GENERATORTRACKER_HPP_INCLUDED
#define CATCH_INTERFACES_GENERATORTRACKER_HPP_INCLUDED

#include <catch2/internal/catch_unique_ptr.hpp>
#include <catch2/internal/catch_stringref.hpp>

#include <string>

namespace Catch {

namespace Generators {
class GeneratorUntypedBase {
    mutable std::string m_stringReprCache;

    std::size_t m_currentElementIndex = 0;

    virtual bool next() = 0;

    virtual std::string stringifyImpl() const = 0;

    virtual void skipToNthElementImpl(std::size_t n);

   public:
    GeneratorUntypedBase() = default;
    GeneratorUntypedBase(GeneratorUntypedBase const&) = default;
    GeneratorUntypedBase& operator=(GeneratorUntypedBase const&) = default;

    virtual ~GeneratorUntypedBase();

    bool countedNext();

    std::size_t currentElementIndex() const {
        return m_currentElementIndex;
    }

    void skipToNthElement(std::size_t n);

    StringRef currentElementAsString() const;

    virtual bool isFinite() const;
};
using GeneratorBasePtr = Catch::Detail::unique_ptr<GeneratorUntypedBase>;

}  // namespace Generators

class IGeneratorTracker {
   public:
    virtual ~IGeneratorTracker();
    virtual auto getGenerator() const -> Generators::GeneratorBasePtr const& = 0;
};

}  // namespace Catch

#endif  // CATCH_INTERFACES_GENERATORTRACKER_HPP_INCLUDED
