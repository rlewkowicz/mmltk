/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#if !defined(nsCRTGlue_h_)
#define nsCRTGlue_h_

#include "nscore.h"

const char* NS_strspnp(const char* aDelims, const char* aStr);

char* NS_strtok(const char* aDelims, char** aStr);

uint32_t NS_strlen(const char16_t* aString);

int NS_strcmp(const char16_t* aStrA, const char16_t* aStrB);

int NS_strncmp(const char16_t* aStrA, const char16_t* aStrB, size_t aLen);

char16_t* NS_xstrdup(const char16_t* aString);

char* NS_xstrdup(const char* aString);

template <typename CharT>
CharT* NS_xstrndup(const CharT* aString, uint32_t aLen);


class nsLowerUpperUtils {
 public:
  static const unsigned char kLower2Upper[256];
  static const unsigned char kUpper2Lower[256];
};

inline char NS_ToUpper(char aChar) {
  return (char)nsLowerUpperUtils::kLower2Upper[(unsigned char)aChar];
}

inline char NS_ToLower(char aChar) {
  return (char)nsLowerUpperUtils::kUpper2Lower[(unsigned char)aChar];
}

bool NS_IsUpper(char aChar);
bool NS_IsLower(char aChar);

constexpr bool NS_IsAscii(const char* aString) {
  while (*aString) {
    if (0x80 & *aString) {
      return false;
    }
    aString++;
  }
  return true;
}

constexpr bool NS_IsAscii(const char* aString, uint32_t aLength) {
  const char* end = aString + aLength;
  while (aString < end) {
    if (0x80 & *aString) {
      return false;
    }
    aString++;
  }
  return true;
}

constexpr bool NS_IsAsciiWhitespace(char16_t aChar) {
  return aChar == ' ' || aChar == '\r' || aChar == '\n' || aChar == '\t';
}

#if !defined(XPCOM_GLUE_AVOID_NSPR)
void NS_MakeRandomString(char* aBuf, int32_t aBufLen);
#endif

#define FF '\f'
#define TAB '\t'

#define CRSTR "\015"
#define LFSTR "\012"
#define CRLF "\015\012" /* A CR LF equivalent string */

#  define OS_FILE_ILLEGAL_CHARACTERS "/:*?\"<>|"

#define KNOWN_PATH_SEPARATORS "\\/"

#if defined(XP_UNIX)
#  define FILE_PATH_SEPARATOR "/"
#else
#  error need_to_define_your_file_path_separator_and_maybe_illegal_characters
#endif

#define CONTROL_CHARACTERS           \
  "\001\002\003\004\005\006\007"     \
  "\010\011\012\013\014\015\016\017" \
  "\020\021\022\023\024\025\026\027" \
  "\030\031\032\033\034\035\036\037" \
  "\177"                             \
  "\200\201\202\203\204\205\206\207" \
  "\210\211\212\213\214\215\216\217" \
  "\220\221\222\223\224\225\226\227" \
  "\230\231\232\233\234\235\236\237"

#define FILE_ILLEGAL_CHARACTERS CONTROL_CHARACTERS OS_FILE_ILLEGAL_CHARACTERS

#endif
