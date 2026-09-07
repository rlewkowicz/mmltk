/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef frontend_TokenKind_h
#define frontend_TokenKind_h

#include <stdint.h>

#include "js/TypeDecls.h"  // IF_DECORATORS

#define FOR_EACH_TOKEN_KIND_WITH_RANGE(MACRO, RANGE)                   \
  MACRO(Eof, "end of script")                                          \
                                                                       \
                             \
  MACRO(Eol, "line terminator")                                        \
                                                                       \
  MACRO(Semi, "';'")                                                   \
  MACRO(Comma, "','")                                                  \
  MACRO(Hook, "'?'")                                  \
  MACRO(Colon, "':'")                                 \
  MACRO(Inc, "'++'")                                    \
  MACRO(Dec, "'--'")                                    \
  MACRO(Dot, "'.'")                               \
  MACRO(TripleDot, "'...'")    \
  MACRO(OptionalChain, "'?.'")                                         \
  MACRO(LeftBracket, "'['")                                            \
  MACRO(RightBracket, "']'")                                           \
  MACRO(LeftCurly, "'{'")                                              \
  MACRO(RightCurly, "'}'")                                             \
  MACRO(LeftParen, "'('")                                              \
  MACRO(RightParen, "')'")                                             \
  MACRO(Name, "identifier")                                            \
  MACRO(PrivateName, "private identifier")                             \
  MACRO(Number, "numeric literal")                                     \
  MACRO(String, "string literal")                                      \
  MACRO(BigInt, "bigint literal")                                      \
  IF_DECORATORS(MACRO(At, "'@'"))                                      \
                                                                       \
                     \
  MACRO(TemplateHead, "'${'")                                          \
                           \
  MACRO(NoSubsTemplate, "template literal")                            \
                                                                       \
  MACRO(RegExp, "regular expression literal")                          \
  MACRO(True, "boolean literal 'true'")                                \
  RANGE(ReservedWordLiteralFirst, True)                                \
  MACRO(False, "boolean literal 'false'")                              \
  MACRO(Null, "null literal")                                          \
  RANGE(ReservedWordLiteralLast, Null)                                 \
  MACRO(This, "keyword 'this'")                                        \
  RANGE(KeywordFirst, This)                                            \
  MACRO(Function, "keyword 'function'")                                \
  MACRO(If, "keyword 'if'")                                            \
  MACRO(Else, "keyword 'else'")                                        \
  MACRO(Switch, "keyword 'switch'")                                    \
  MACRO(Case, "keyword 'case'")                                        \
  MACRO(Default, "keyword 'default'")                                  \
  MACRO(While, "keyword 'while'")                                      \
  MACRO(Do, "keyword 'do'")                                            \
  MACRO(For, "keyword 'for'")                                          \
  MACRO(Break, "keyword 'break'")                                      \
  MACRO(Continue, "keyword 'continue'")                                \
  MACRO(Var, "keyword 'var'")                                          \
  MACRO(Const, "keyword 'const'")                                      \
  MACRO(With, "keyword 'with'")                                        \
  MACRO(Return, "keyword 'return'")                                    \
  MACRO(New, "keyword 'new'")                                          \
  MACRO(Delete, "keyword 'delete'")                                    \
  MACRO(Try, "keyword 'try'")                                          \
  MACRO(Catch, "keyword 'catch'")                                      \
  MACRO(Finally, "keyword 'finally'")                                  \
  MACRO(Throw, "keyword 'throw'")                                      \
  MACRO(Debugger, "keyword 'debugger'")                                \
  MACRO(Export, "keyword 'export'")                                    \
  MACRO(Import, "keyword 'import'")                                    \
  MACRO(Class, "keyword 'class'")                                      \
  MACRO(Extends, "keyword 'extends'")                                  \
  MACRO(Super, "keyword 'super'")                                      \
  RANGE(KeywordLast, Super)                                            \
                                                                       \
                                              \
  MACRO(As, "'as'")                                                    \
  RANGE(ContextualKeywordFirst, As)                                    \
   \
  IF_DECORATORS(MACRO(Accessor, "'accessor'"))                         \
  MACRO(Async, "'async'")                                              \
  MACRO(Await, "'await'")                                              \
  MACRO(Each, "'each'")                                                \
  MACRO(From, "'from'")                                                \
  MACRO(Get, "'get'")                                                  \
  MACRO(Let, "'let'")                                                  \
  MACRO(Meta, "'meta'")                                                \
  MACRO(Of, "'of'")                                                    \
  MACRO(Set, "'set'")                                                  \
  MACRO(Static, "'static'")                                            \
  MACRO(Source, "'source'")                                            \
  MACRO(Target, "'target'")                                            \
  IF_EXPLICIT_RESOURCE_MANAGEMENT(MACRO(Using, "'using'"))             \
  MACRO(Yield, "'yield'")                                              \
  RANGE(ContextualKeywordLast, Yield)                                  \
                                                                       \
                                            \
  MACRO(Enum, "reserved word 'enum'")                                  \
  RANGE(FutureReservedKeywordFirst, Enum)                              \
  RANGE(FutureReservedKeywordLast, Enum)                               \
                                                                       \
                                    \
  MACRO(Implements, "reserved word 'implements'")                      \
  RANGE(StrictReservedKeywordFirst, Implements)                        \
  MACRO(Interface, "reserved word 'interface'")                        \
  MACRO(Package, "reserved word 'package'")                            \
  MACRO(Private, "reserved word 'private'")                            \
  MACRO(Protected, "reserved word 'protected'")                        \
  MACRO(Public, "reserved word 'public'")                              \
  RANGE(StrictReservedKeywordLast, Public)                             \
                                                                       \
                                                                    \
                                                                    \
  MACRO(Coalesce, "'\?\?'")    \
  RANGE(BinOpFirst, Coalesce)                                          \
  MACRO(Or, "'||'")                                    \
  MACRO(And, "'&&'")                                  \
  MACRO(BitOr, "'|'")                                  \
  MACRO(BitXor, "'^'")                                \
  MACRO(BitAnd, "'&'")                                \
                                                                       \
              \
  MACRO(StrictEq, "'==='")                                             \
  RANGE(EqualityStart, StrictEq)                                       \
  MACRO(Eq, "'=='")                                                    \
  MACRO(StrictNe, "'!=='")                                             \
  MACRO(Ne, "'!='")                                                    \
  RANGE(EqualityLast, Ne)                                              \
                                                                       \
                       \
  MACRO(Lt, "'<'")                                                     \
  RANGE(RelOpStart, Lt)                                                \
  MACRO(Le, "'<='")                                                    \
  MACRO(Gt, "'>'")                                                     \
  MACRO(Ge, "'>='")                                                    \
  RANGE(RelOpLast, Ge)                                                 \
                                                                       \
  MACRO(InstanceOf, "keyword 'instanceof'")                            \
  RANGE(KeywordBinOpFirst, InstanceOf)                                 \
  MACRO(In, "keyword 'in'")                                            \
  MACRO(PrivateIn, "keyword 'in' (private)")                           \
  RANGE(KeywordBinOpLast, PrivateIn)                                   \
                                                                       \
                                 \
  MACRO(Lsh, "'<<'")                                                   \
  RANGE(ShiftOpStart, Lsh)                                             \
  MACRO(Rsh, "'>>'")                                                   \
  MACRO(Ursh, "'>>>'")                                                 \
  RANGE(ShiftOpLast, Ursh)                                             \
                                                                       \
  MACRO(Add, "'+'")                                                    \
  MACRO(Sub, "'-'")                                                    \
  MACRO(Mul, "'*'")                                                    \
  MACRO(Div, "'/'")                                                    \
  MACRO(Mod, "'%'")                                                    \
  MACRO(Pow, "'**'")                                                   \
  RANGE(BinOpLast, Pow)                                                \
                                                                       \
                                          \
  MACRO(TypeOf, "keyword 'typeof'")                                    \
  RANGE(KeywordUnOpFirst, TypeOf)                                      \
  MACRO(Void, "keyword 'void'")                                        \
  RANGE(KeywordUnOpLast, Void)                                         \
  MACRO(Not, "'!'")                                                    \
  MACRO(BitNot, "'~'")                                                 \
                                                                       \
  MACRO(Arrow, "'=>'")                             \
                                                                       \
                        \
  MACRO(Assign, "'='")                                                 \
  RANGE(AssignmentStart, Assign)                                       \
  MACRO(AddAssign, "'+='")                                             \
  MACRO(SubAssign, "'-='")                                             \
  MACRO(CoalesceAssign, "'\?\?='")        \
  MACRO(OrAssign, "'||='")                                             \
  MACRO(AndAssign, "'&&='")                                            \
  MACRO(BitOrAssign, "'|='")                                           \
  MACRO(BitXorAssign, "'^='")                                          \
  MACRO(BitAndAssign, "'&='")                                          \
  MACRO(LshAssign, "'<<='")                                            \
  MACRO(RshAssign, "'>>='")                                            \
  MACRO(UrshAssign, "'>>>='")                                          \
  MACRO(MulAssign, "'*='")                                             \
  MACRO(DivAssign, "'/='")                                             \
  MACRO(ModAssign, "'%='")                                             \
  MACRO(PowAssign, "'**='")                                            \
  RANGE(AssignmentLast, PowAssign)

#define TOKEN_KIND_RANGE_EMIT_NONE(name, value)
#define FOR_EACH_TOKEN_KIND(MACRO) \
  FOR_EACH_TOKEN_KIND_WITH_RANGE(MACRO, TOKEN_KIND_RANGE_EMIT_NONE)

namespace js {
namespace frontend {

enum class TokenKind : uint8_t {
#define EMIT_ENUM(name, desc) name,
#define EMIT_ENUM_RANGE(name, value) name = value,
  FOR_EACH_TOKEN_KIND_WITH_RANGE(EMIT_ENUM, EMIT_ENUM_RANGE)
#undef EMIT_ENUM
#undef EMIT_ENUM_RANGE
      Limit  
};

inline bool TokenKindIsBinaryOp(TokenKind tt) {
  return TokenKind::BinOpFirst <= tt && tt <= TokenKind::BinOpLast;
}

inline bool TokenKindIsEquality(TokenKind tt) {
  return TokenKind::EqualityStart <= tt && tt <= TokenKind::EqualityLast;
}

inline bool TokenKindIsRelational(TokenKind tt) {
  return TokenKind::RelOpStart <= tt && tt <= TokenKind::RelOpLast;
}

inline bool TokenKindIsShift(TokenKind tt) {
  return TokenKind::ShiftOpStart <= tt && tt <= TokenKind::ShiftOpLast;
}

inline bool TokenKindIsAssignment(TokenKind tt) {
  return TokenKind::AssignmentStart <= tt && tt <= TokenKind::AssignmentLast;
}

[[nodiscard]] inline bool TokenKindIsKeyword(TokenKind tt) {
  return (TokenKind::KeywordFirst <= tt && tt <= TokenKind::KeywordLast) ||
         (TokenKind::KeywordBinOpFirst <= tt &&
          tt <= TokenKind::KeywordBinOpLast) ||
         (TokenKind::KeywordUnOpFirst <= tt &&
          tt <= TokenKind::KeywordUnOpLast);
}

[[nodiscard]] inline bool TokenKindIsContextualKeyword(TokenKind tt) {
  return TokenKind::ContextualKeywordFirst <= tt &&
         tt <= TokenKind::ContextualKeywordLast;
}

[[nodiscard]] inline bool TokenKindIsFutureReservedWord(TokenKind tt) {
  return TokenKind::FutureReservedKeywordFirst <= tt &&
         tt <= TokenKind::FutureReservedKeywordLast;
}

[[nodiscard]] inline bool TokenKindIsStrictReservedWord(TokenKind tt) {
  return TokenKind::StrictReservedKeywordFirst <= tt &&
         tt <= TokenKind::StrictReservedKeywordLast;
}

[[nodiscard]] inline bool TokenKindIsReservedWordLiteral(TokenKind tt) {
  return TokenKind::ReservedWordLiteralFirst <= tt &&
         tt <= TokenKind::ReservedWordLiteralLast;
}

[[nodiscard]] inline bool TokenKindIsReservedWord(TokenKind tt) {
  return TokenKindIsKeyword(tt) || TokenKindIsFutureReservedWord(tt) ||
         TokenKindIsReservedWordLiteral(tt);
}

[[nodiscard]] inline bool TokenKindIsPossibleIdentifier(TokenKind tt) {
  return tt == TokenKind::Name || TokenKindIsContextualKeyword(tt) ||
         TokenKindIsStrictReservedWord(tt);
}

[[nodiscard]] inline bool TokenKindIsPossibleIdentifierName(TokenKind tt) {
  return TokenKindIsPossibleIdentifier(tt) || TokenKindIsReservedWord(tt);
}

}  
}  

#endif /* frontend_TokenKind_h */
