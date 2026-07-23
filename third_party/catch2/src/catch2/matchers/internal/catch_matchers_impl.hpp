

#ifndef CATCH_MATCHERS_IMPL_HPP_INCLUDED
#define CATCH_MATCHERS_IMPL_HPP_INCLUDED

#include <catch2/internal/catch_assertion_handler.hpp>
#include <catch2/internal/catch_source_line_info.hpp>
#include <catch2/internal/catch_decomposer.hpp>
#include <catch2/internal/catch_preprocessor_internal_stringify.hpp>
#include <catch2/internal/catch_move_and_forward.hpp>

#include <string>

namespace Catch {

CATCH_INTERNAL_START_WARNINGS_SUPPRESSION
CATCH_INTERNAL_SUPPRESS_SIGN_COMPARE_WARNINGS
CATCH_INTERNAL_SUPPRESS_NON_VIRTUAL_DTOR_WARNINGS

template <typename ArgT, typename MatcherT>
class MatchExpr : public ITransientExpression {
    ArgT&& m_arg;
    MatcherT const& m_matcher;

   public:
    constexpr MatchExpr(ArgT&& arg, MatcherT const& matcher)
        : ITransientExpression{true, matcher.match(arg)}, m_arg(CATCH_FORWARD(arg)), m_matcher(matcher) {}

    void streamReconstructedExpression(std::ostream& os) const override {
        os << Catch::Detail::stringify(m_arg) << ' ' << m_matcher.toString();
    }
};

CATCH_INTERNAL_STOP_WARNINGS_SUPPRESSION

namespace Matchers {
template <typename ArgT>
class MatcherBase;
}

using StringMatcher = Matchers::MatcherBase<std::string>;

void handleExceptionMatchExpr(AssertionHandler& handler, StringMatcher const& matcher);

template <typename ArgT, typename MatcherT>
constexpr MatchExpr<ArgT, MatcherT> makeMatchExpr(ArgT&& arg, MatcherT const& matcher) {
    return MatchExpr<ArgT, MatcherT>(CATCH_FORWARD(arg), matcher);
}

}  

#define INTERNAL_CHECK_THAT(macroName, matcher, resultDisposition, arg)                               \
    do {                                                                                              \
        Catch::AssertionHandler catchAssertionHandler(                                                \
            macroName##_catch_sr, CATCH_INTERNAL_LINEINFO,                                            \
            CATCH_INTERNAL_STRINGIFY(arg) ", " CATCH_INTERNAL_STRINGIFY(matcher), resultDisposition); \
        INTERNAL_CATCH_TRY {                                                                          \
            catchAssertionHandler.handleExpr(Catch::makeMatchExpr(arg, matcher));                     \
        }                                                                                             \
        INTERNAL_CATCH_CATCH(catchAssertionHandler)                                                   \
        catchAssertionHandler.complete();                                                             \
    } while (false)

#define INTERNAL_CATCH_THROWS_MATCHES(macroName, exceptionType, resultDisposition, matcher, ...) \
    do {                                                                                         \
        Catch::AssertionHandler catchAssertionHandler(                                           \
            macroName##_catch_sr, CATCH_INTERNAL_LINEINFO,                                       \
            CATCH_INTERNAL_STRINGIFY(__VA_ARGS__) ", " CATCH_INTERNAL_STRINGIFY(                 \
                exceptionType) ", " CATCH_INTERNAL_STRINGIFY(matcher),                           \
            resultDisposition);                                                                  \
        if (catchAssertionHandler.allowThrows())                                                 \
            try {                                                                                \
                CATCH_INTERNAL_START_WARNINGS_SUPPRESSION                                        \
                CATCH_INTERNAL_SUPPRESS_USELESS_CAST_WARNINGS                                    \
                static_cast<void>(__VA_ARGS__);                                                  \
                CATCH_INTERNAL_STOP_WARNINGS_SUPPRESSION                                         \
                catchAssertionHandler.handleUnexpectedExceptionNotThrown();                      \
            } catch (exceptionType const& ex) {                                                  \
                catchAssertionHandler.handleExpr(Catch::makeMatchExpr(ex, matcher));             \
            } catch (...) {                                                                      \
                catchAssertionHandler.handleUnexpectedInflightException();                       \
            }                                                                                    \
        else                                                                                     \
            catchAssertionHandler.handleThrowingCallSkipped();                                   \
        catchAssertionHandler.complete();                                                        \
    } while (false)

#endif  // CATCH_MATCHERS_IMPL_HPP_INCLUDED
