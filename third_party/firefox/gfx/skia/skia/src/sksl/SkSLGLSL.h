/*
 * Copyright 2021 Google LLC
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#if !defined(SkSLGLSL_DEFINED)
#define SkSLGLSL_DEFINED

namespace SkSL {

enum class GLSLGeneration {
    k110,
    k100es = k110,
    k130,
    k140,
    k150,
    k330,
    k300es = k330,
    k400,
    k420,
    k310es,
    k320es,
};

} 

#endif
