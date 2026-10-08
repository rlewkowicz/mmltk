module;
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unicode/ucol.h>
#include <unicode/uloc.h>
#include <unicode/utypes.h>

export module mmltk.tools.cpp_locale;

export {
namespace mmltk::tools {

// Inputs are UTF-8; JSON keys containing isolated UTF-16 surrogates must escape
// those units as JSON \\uXXXX before passing them to this comparator.
inline int locale_compare(std::string_view left, std::string_view right, bool english = false) {
  struct Collator {
    UCollator* handle = nullptr;
    explicit Collator(const char* locale) {
      UErrorCode status = U_ZERO_ERROR;
      handle = ucol_open(locale, &status);
      if (U_FAILURE(status)) {
        ucol_close(handle);
        throw std::runtime_error("cannot initialize report collation: " + std::string(u_errorName(status)));
      }
    }
    ~Collator() { ucol_close(handle); }
    Collator(const Collator&) = delete;
    Collator& operator=(const Collator&) = delete;
  };
  const std::string_view locale = uloc_getDefault();
  // V8 maps ICU's C/POSIX locale to en-US for Intl's default locale.
  thread_local Collator defaults(locale == "en_US_POSIX" || locale == "C" ? "en_US" : uloc_getDefault());
  thread_local Collator en("en");
  constexpr auto maximum = static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max());
  if (left.size() > maximum || right.size() > maximum) throw std::length_error("report collation text is too long");
  UErrorCode status = U_ZERO_ERROR;
  const auto result = ucol_strcollUTF8((english ? en : defaults).handle,
      left.data(), static_cast<std::int32_t>(left.size()), right.data(), static_cast<std::int32_t>(right.size()), &status);
  if (U_FAILURE(status)) throw std::runtime_error("cannot compare report text: " + std::string(u_errorName(status)));
  return result < 0 ? -1 : result > 0 ? 1 : 0;
}

}  // namespace mmltk::tools

}
