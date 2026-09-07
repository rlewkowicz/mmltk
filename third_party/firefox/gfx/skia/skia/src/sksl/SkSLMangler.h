/*
 * Copyright 2021 Google LLC
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#if !defined(SKSL_MANGLER)
#define SKSL_MANGLER

#include <string>
#include <string_view>

namespace SkSL {

class SymbolTable;

class Mangler {
public:
    std::string uniqueName(std::string_view baseName, SymbolTable* symbolTable);

    void reset() {
        fCounter = 0;
    }

private:
    int fCounter = 0;
};

} 

#endif
