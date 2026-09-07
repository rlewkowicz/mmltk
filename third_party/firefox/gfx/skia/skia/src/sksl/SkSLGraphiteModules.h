/*
 * Copyright 2024 Google LLC
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#if !defined(SKSL_GRAPHITE_MODULES)
#define SKSL_GRAPHITE_MODULES

namespace SkSL::Loader {
struct GraphiteModules {
    const char* fFragmentShader;
    const char* fVertexShader;
};

GraphiteModules GetGraphiteModules();
void SetGraphiteModuleData(const GraphiteModules&);

}  

#endif
