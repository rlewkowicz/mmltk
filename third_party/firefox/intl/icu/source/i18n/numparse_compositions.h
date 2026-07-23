// License & terms of use: http://www.unicode.org/copyright.html

#include "unicode/utypes.h"

#if !UCONFIG_NO_FORMATTING
#ifndef __SOURCE_NUMPARSE_COMPOSITIONS__
#define __SOURCE_NUMPARSE_COMPOSITIONS__

#include "numparse_types.h"

U_NAMESPACE_BEGIN

namespace numparse::impl {

class U_I18N_API CompositionMatcher : public NumberParseMatcher {
  protected:
    CompositionMatcher() = default;

    virtual const NumberParseMatcher* const* begin() const = 0;

    virtual const NumberParseMatcher* const* end() const = 0;
};




class U_I18N_API SeriesMatcher : public CompositionMatcher {
  public:
    bool match(StringSegment& segment, ParsedNumber& result, UErrorCode& status) const override;

    bool smokeTest(const StringSegment& segment) const override;

    void postProcess(ParsedNumber& result) const override;

    virtual int32_t length() const = 0;

  protected:
    SeriesMatcher() = default;
};

class U_I18N_API_CLASS ArraySeriesMatcher : public SeriesMatcher {
  public:
    U_I18N_API ArraySeriesMatcher();  

    typedef MaybeStackArray<const NumberParseMatcher*, 3> MatcherArray;

    U_I18N_API ArraySeriesMatcher(MatcherArray& matchers, int32_t matchersLen);

    UnicodeString toString() const override;

    U_I18N_API int32_t length() const override;

  protected:
    const NumberParseMatcher* const* begin() const override;

    const NumberParseMatcher* const* end() const override;

  private:
    MatcherArray fMatchers;
    int32_t fMatchersLen;
};

} 

U_NAMESPACE_END

#endif //__SOURCE_NUMPARSE_COMPOSITIONS__
#endif /* #if !UCONFIG_NO_FORMATTING */
