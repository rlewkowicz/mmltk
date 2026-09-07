// Copyright 2012 The ANGLE Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#if !defined(COMPILER_TRANSLATOR_PRAGMA_H_)
#define COMPILER_TRANSLATOR_PRAGMA_H_

struct TPragma
{
    struct STDGL
    {
        STDGL() : invariantAll(false) {}

        bool invariantAll;
    };

    TPragma() : optimize(true), debug(false) {}
    TPragma(bool o, bool d) : optimize(o), debug(d) {}

    bool optimize;
    bool debug;
    STDGL stdgl;
};

#endif
