

#include <catch2/internal/catch_source_line_info.hpp>

#include <cstring>
#include <ostream>

namespace Catch {

bool SourceLineInfo::operator==(SourceLineInfo const& other) const noexcept {
    return line == other.line && (file == other.file || std::strcmp(file, other.file) == 0);
}
bool SourceLineInfo::operator<(SourceLineInfo const& other) const noexcept {
    return line < other.line || (line == other.line && file != other.file && (std::strcmp(file, other.file) < 0));
}

std::ostream& operator<<(std::ostream& os, SourceLineInfo const& info) {
#ifndef __GNUG__
    os << info.file << '(' << info.line << ')';
#else
    os << info.file << ':' << info.line;
#endif
    return os;
}

}  
