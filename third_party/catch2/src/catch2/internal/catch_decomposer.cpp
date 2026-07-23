

#include <catch2/internal/catch_decomposer.hpp>

namespace Catch {

void ITransientExpression::streamReconstructedExpression(std::ostream& os) const {
    os << "Some class derived from ITransientExpression without overriding streamReconstructedExpression";
}

void formatReconstructedExpression(std::ostream& os, std::string const& lhs, StringRef op, std::string const& rhs) {
    if (lhs.size() + rhs.size() < 40 && lhs.find('\n') == std::string::npos && rhs.find('\n') == std::string::npos)
        os << lhs << ' ' << op << ' ' << rhs;
    else
        os << lhs << '\n' << op << '\n' << rhs;
}
}  
