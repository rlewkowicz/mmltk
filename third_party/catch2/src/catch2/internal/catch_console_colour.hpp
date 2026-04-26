
//              Copyright Catch2 Authors
// Distributed under the Boost Software License, Version 1.0.
//   (See accompanying file LICENSE.txt or copy at
//        https://www.boost.org/LICENSE_1_0.txt)

// SPDX-License-Identifier: BSL-1.0
#ifndef CATCH_CONSOLE_COLOUR_HPP_INCLUDED
#define CATCH_CONSOLE_COLOUR_HPP_INCLUDED

#include <catch2/internal/catch_unique_ptr.hpp>

#include <iosfwd>
#include <cstdint>

namespace Catch {

enum class ColourMode : std::uint8_t;
class IStream;

struct Colour {
    enum Code {
        None = 0,

        White,
        Red,
        Green,
        Blue,
        Cyan,
        Yellow,
        Grey,

        Bright = 0x10,

        BrightRed = Bright | Red,
        BrightGreen = Bright | Green,
        LightGrey = Bright | Grey,
        BrightWhite = Bright | White,
        BrightYellow = Bright | Yellow,

        FileName = LightGrey,
        Warning = BrightYellow,
        ResultError = BrightRed,
        ResultSuccess = BrightGreen,
        ResultExpectedFailure = Warning,

        Error = BrightRed,
        Success = Green,
        Skip = LightGrey,

        OriginalExpression = Cyan,
        ReconstructedExpression = BrightYellow,

        SecondaryText = LightGrey,
        Headers = White
    };
};

class ColourImpl {
   protected:
    IStream* m_stream;

   public:
    ColourImpl(IStream* stream) : m_stream(stream) {}

    class ColourGuard {
        ColourImpl const* m_colourImpl;
        Colour::Code m_code;
        bool m_engaged = false;

       public:
        ColourGuard(Colour::Code code, ColourImpl const* colour);

        ColourGuard(ColourGuard const& rhs) = delete;
        ColourGuard& operator=(ColourGuard const& rhs) = delete;

        ColourGuard(ColourGuard&& rhs) noexcept;
        ColourGuard& operator=(ColourGuard&& rhs) noexcept;

        ~ColourGuard();

        ColourGuard& engage(std::ostream& stream) &;
        ColourGuard&& engage(std::ostream& stream) &&;

       private:
        friend std::ostream& operator<<(std::ostream& lhs, ColourGuard& guard) {
            guard.engageImpl(lhs);
            return lhs;
        }
        friend std::ostream& operator<<(std::ostream& lhs, ColourGuard&& guard) {
            guard.engageImpl(lhs);
            return lhs;
        }

        void engageImpl(std::ostream& stream);
    };

    virtual ~ColourImpl();
    ColourGuard guardColour(Colour::Code colourCode);

   private:
    virtual void use(Colour::Code colourCode) const = 0;
};

Detail::unique_ptr<ColourImpl> makeColourImpl(ColourMode colourSelection, IStream* stream);

bool isColourImplAvailable(ColourMode colourSelection);

}  // namespace Catch

#endif  // CATCH_CONSOLE_COLOUR_HPP_INCLUDED
