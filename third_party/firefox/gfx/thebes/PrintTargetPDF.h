/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef MOZILLA_GFX_PRINTTARGETPDF_H
#define MOZILLA_GFX_PRINTTARGETPDF_H

#include "nsCOMPtr.h"
#include "nsIOutputStream.h"
#include "PrintTarget.h"

namespace mozilla {
namespace gfx {

class PrintTargetPDF final : public PrintTarget {
 public:
  static already_AddRefed<PrintTarget> CreateOrNull(
      nsIOutputStream* aStream, const IntSize& aSizeInPoints);

  nsresult BeginPage(const IntSize& aSizeInPoints) override;
  nsresult EndPage() override;
  void Finish() override;

 private:
  PrintTargetPDF(cairo_surface_t* aCairoSurface, const IntSize& aSize,
                 nsIOutputStream* aStream);
  virtual ~PrintTargetPDF();

  nsCOMPtr<nsIOutputStream> mStream;
};

}  
}  

#endif /* MOZILLA_GFX_PRINTTARGETPDF_H */
