/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef frontend_FoldConstants_h
#define frontend_FoldConstants_h

#include "frontend/Stencil.h"
#include "frontend/SyntaxParseHandler.h"

namespace js {

class FrontendContext;

namespace frontend {

class FullParseHandler;
template <class ParseHandler>
class PerHandlerParser;
class ParserAtomsTable;

[[nodiscard]] extern bool FoldConstants(FrontendContext* fc,
                                        ParserAtomsTable& parserAtoms,
                                        BigIntStencilVector& bigInts,
                                        ParseNode** pnp,
                                        FullParseHandler* handler);

[[nodiscard]] inline bool FoldConstants(FrontendContext* fc,
                                        ParserAtomsTable& parserAtoms,
                                        BigIntStencilVector& bigInts,
                                        typename SyntaxParseHandler::Node* pnp,
                                        SyntaxParseHandler* handler) {
  return true;
}

} 
} 

#endif /* frontend_FoldConstants_h */
