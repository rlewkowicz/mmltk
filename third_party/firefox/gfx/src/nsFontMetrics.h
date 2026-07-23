/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#if !defined(NSFONTMETRICS_H_)
#define NSFONTMETRICS_H_

#include <stdint.h>          // for uint32_t
#include <sys/types.h>       // for int32_t
#include "mozilla/RefPtr.h"  // for RefPtr
#include "nsCoord.h"         // for nscoord
#include "nsFont.h"          // for nsFont
#include "nsISupports.h"     // for NS_INLINE_DECL_REFCOUNTING
#include "nscore.h"          // for char16_t

class gfxContext;
class gfxFontGroup;
class gfxUserFontSet;
class gfxTextPerfMetrics;
class nsPresContext;
class nsAtom;
struct nsBoundingMetrics;

namespace mozilla {
enum class StyleTextOrientation : uint8_t;
namespace gfx {
class DrawTarget;
}  
}  

class nsFontMetrics final {
 public:
  typedef mozilla::gfx::DrawTarget DrawTarget;

  enum FontOrientation { eHorizontal, eVertical };

  struct MOZ_STACK_CLASS Params {
    nsAtom* language = nullptr;
    bool explicitLanguage = false;
    FontOrientation orientation = eHorizontal;
    gfxUserFontSet* userFontSet = nullptr;
    gfxTextPerfMetrics* textPerf = nullptr;
    gfxFontFeatureValueSet* featureValueLookup = nullptr;
  };

  nsFontMetrics(const nsFont& aFont, const Params& aParams,
                nsPresContext* aContext);

  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(nsFontMetrics)

  void Destroy();

  nscoord AlphabeticBaseline() const;

  nscoord CentralBaseline() const;

  nscoord XMiddleBaseline() const;

  nscoord IdeographicUnderBaseline() const;

  nscoord IdeographicOverBaseline() const;

  nscoord IdeographicInkUnderBaseline() const;

  nscoord IdeographicInkOverBaseline() const;

  nscoord HangingBaseline() const;

  nscoord MathBaseline() const;

  nscoord XHeight() const;

  nscoord CapHeight() const;

  nscoord SuperscriptOffset() const;

  nscoord SubscriptOffset() const;

  void GetStrikeout(nscoord& aOffset, nscoord& aSize) const;

  void GetUnderline(nscoord& aOffset, nscoord& aSize) const;

  nscoord InternalLeading() const;

  nscoord ExternalLeading() const;

  nscoord EmHeight() const;

  nscoord TrimmedAscent() const;

  nscoord TrimmedDescent() const;

  nscoord MaxHeight() const;

  nscoord MaxAscent() const;

  nscoord MaxDescent() const;

  nscoord MaxAdvance() const;

  nscoord AveCharWidth() const;

  nscoord ZeroOrAveCharWidth() const;

  nscoord SpaceWidth() const;

  nscoord InterScriptSpacingWidth() const;

  const nsFont& Font() const { return mFont; }

  nsAtom* Language() const { return mLanguage; }

  FontOrientation Orientation() const { return mOrientation; }

  int32_t GetMaxStringLength() const;

  nscoord GetWidth(const char* aString, uint32_t aLength,
                   DrawTarget* aDrawTarget) const;
  nscoord GetWidth(const char16_t* aString, uint32_t aLength,
                   DrawTarget* aDrawTarget) const;

  void DrawString(const char* aString, uint32_t aLength, nscoord aX, nscoord aY,
                  gfxContext* aContext) const;
  void DrawString(const char16_t* aString, uint32_t aLength, nscoord aX,
                  nscoord aY, gfxContext* aContext,
                  DrawTarget* aTextRunConstructionDrawTarget) const;

  nsBoundingMetrics GetBoundingMetrics(const char16_t* aString,
                                       uint32_t aLength,
                                       DrawTarget* aDrawTarget) const;

  nsBoundingMetrics GetInkBoundsForInkOverflow(const char16_t* aString,
                                               uint32_t aLength,
                                               DrawTarget* aDrawTarget) const;

  void SetTextRunRTL(bool aIsRTL) { mTextRunRTL = aIsRTL; }
  bool GetTextRunRTL() const { return mTextRunRTL; }

  void SetVertical(bool aVertical) { mVertical = aVertical; }
  bool GetVertical() const { return mVertical; }

  void SetTextOrientation(mozilla::StyleTextOrientation aTextOrientation) {
    mTextOrientation = aTextOrientation;
  }
  mozilla::StyleTextOrientation GetTextOrientation() const {
    return mTextOrientation;
  }

  bool ExplicitLanguage() const { return mExplicitLanguage; }

  gfxFontGroup* GetThebesFontGroup() const { return mFontGroup; }
  gfxUserFontSet* GetUserFontSet() const;

  int32_t AppUnitsPerDevPixel() const { return mP2A; }


 private:
  ~nsFontMetrics();

  const nsFont mFont;
  RefPtr<gfxFontGroup> mFontGroup;
  RefPtr<nsAtom> const mLanguage;
  nsPresContext* MOZ_NON_OWNING_REF mPresContext;
  const int32_t mP2A;

  const FontOrientation mOrientation;

  const bool mExplicitLanguage;


  bool mTextRunRTL;
  bool mVertical;
  mozilla::StyleTextOrientation mTextOrientation;
};

#endif
