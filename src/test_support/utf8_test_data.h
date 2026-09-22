#pragma once
#include <array>
#include <string_view>
namespace mmltk::testsupport {
inline constexpr std::array<std::string_view, 19> kMalformedUtf8{
 "\x80",
 "\xbf",  // stray continuations
 "\xc0\x80",
 "\xc1\xbf",  // overlong two-byte forms
 "\xe0\x80\x80",
 "\xe0\x9f\xbf",  // overlong three-byte forms
 "\xf0\x80\x80\x80",
 "\xf0\x8f\xbf\xbf",  // overlong four-byte forms
 "\xed\xa0\x80",
 "\xed\xbf\xbf",  // surrogate limits
 "\xf4\x90\x80\x80",
 "\xf5\x80\x80\x80",
 "\xff",  // out of range
 "\xc2x",
 "\xe1\x80x",
 "\xf1\x80\x80x",  // invalid continuations
 "\xc2",
 "\xe0\xa0",
 "\xf0\x90\x80",  // truncated scalars
};
}  // namespace mmltk::testsupport
