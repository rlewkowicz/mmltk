/*
 * Copyright 2021 Google LLC
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#if !defined(SKSL_TRANSFORM)
#define SKSL_TRANSFORM

#include "src/sksl/SkSLDefines.h"
#include "src/sksl/ir/SkSLModifierFlags.h"

#include <cstdint>
#include <memory>

namespace SkSL {

class Block;
class Context;
class Expression;
class IndexExpression;
class Position;
class ProgramUsage;
class SymbolTable;
class Variable;
enum class ProgramKind : int8_t;
struct Module;
struct Program;

namespace Transform {

ModifierFlags AddConstToVarModifiers(const Variable& var,
                                     const Expression* initialValue,
                                     const ProgramUsage* usage);

std::unique_ptr<Expression> RewriteIndexedSwizzle(const Context& context,
                                                  const IndexExpression& swizzle);

void FindAndDeclareBuiltinFunctions(Program& program);

void FindAndDeclareBuiltinStructs(Program& program);

void FindAndDeclareBuiltinVariables(Program& program);

void EliminateUnreachableCode(Module& module, ProgramUsage* usage);
void EliminateUnreachableCode(Program& program);

void EliminateEmptyStatements(Module& module);

void EliminateUnnecessaryBraces(const Context& context, Module& module);

void ReplaceSplatCastsWithSwizzles(const Context& context, Module& module);

bool EliminateDeadFunctions(const Context& context, Module& module, ProgramUsage* usage);
bool EliminateDeadFunctions(Program& program);

bool EliminateDeadLocalVariables(const Context& context,
                                 Module& module,
                                 ProgramUsage* usage);
bool EliminateDeadLocalVariables(Program& program);
bool EliminateDeadGlobalVariables(const Context& context,
                                  Module& module,
                                  ProgramUsage* usage,
                                  bool onlyPrivateGlobals);
bool EliminateDeadGlobalVariables(Program& program);

void RenamePrivateSymbols(Context& context, Module& module, ProgramUsage* usage, ProgramKind kind);

void ReplaceConstVarsWithLiterals(Module& module, ProgramUsage* usage);

/**
 * Looks for variables inside of the top-level of switch-cases, such as:
 *
 *    case 1: int i;         // `i` is at top-level
 *    case 2: float f = 5.0; // `f` is at top-level, and has an initial-value assignment
 *    case 3: { bool b; }    // `b` is not at top-level; it has an additional scope
 *
 * If any top-level variables are found, a scoped block is created and returned which holds the
 * variable declarations from the switch-cases and into the outer scope. (Variables with additional
 * scoping are left as-is.) Then, we replace the declarations with nops or assignment statements.
 * That is, we would return a Block like this:
 *
 *    {
 *        int i;
 *        float f;
 *    }
 *
 * And we would also mutate the passed-in case statements to eliminate the variable decarations:
 *
 *    case 1: Nop;         // `i` is declared in the returned block and needs no initialization
 *    case 2: f = 5.0;     // `f` is declared in the returned block and initialized here
 *    case 3: { bool b; }  // `b` is left as-is because it has a block-scope
 *
 * This doesn't change the meaning or correctness of the code. If the switch needs to be rewriten
 * (e.g. due to the restrictions of ES2 or WGSL), this transformation prevents scoping issues with
 * variables falling out of scope between switch-cases when we fall through.
 *
 * If there are no variables at the top-level, null is returned.
 */
std::unique_ptr<Block> HoistSwitchVarDeclarationsAtTopLevel(const Context&, StatementArray& cases,
                                                            SymbolTable& symbolTable, Position pos);

} 
} 

#endif
