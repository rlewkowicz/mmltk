

#include <catch2/matchers/internal/catch_matchers_impl.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/interfaces/catch_interfaces_registry_hub.hpp>
#include <catch2/internal/catch_move_and_forward.hpp>

namespace Catch {

void handleExceptionMatchExpr(AssertionHandler& handler, StringMatcher const& matcher) {
    std::string exceptionMessage = Catch::translateActiveException();
    MatchExpr<std::string, StringMatcher const&> expr(CATCH_MOVE(exceptionMessage), matcher);
    handler.handleExpr(expr);
}

}  
