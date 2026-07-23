/*
 * Copyright 2016 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#if !defined(SkCodecAnimation_DEFINED)
#define SkCodecAnimation_DEFINED

namespace SkCodecAnimation {
    enum class DisposalMethod {
        kKeep               = 1,

        kRestoreBGColor     = 2,

        kRestorePrevious    = 3,
    };

    enum class Blend {
        kSrcOver,

        kSrc,
    };

} 
#endif
