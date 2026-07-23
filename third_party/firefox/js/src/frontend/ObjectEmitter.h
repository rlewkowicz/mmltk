/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef frontend_ObjectEmitter_h
#define frontend_ObjectEmitter_h

#include "mozilla/Attributes.h"  // MOZ_STACK_CLASS, MOZ_ALWAYS_INLINE, MOZ_RAII
#include "mozilla/Maybe.h"       // Maybe

#include <stddef.h>  // size_t
#include <stdint.h>  // uint32_t

#include "frontend/EmitterScope.h"   // EmitterScope
#include "frontend/NameOpEmitter.h"  // NameOpEmitter
#include "frontend/ParseNode.h"      // AccessorType
#include "frontend/ParserAtom.h"     // TaggedParserAtomIndex
#include "frontend/TDZCheckCache.h"  // TDZCheckCache
#include "vm/Opcodes.h"              // JSOp
#include "vm/Scope.h"                // LexicalScope

namespace js {

namespace frontend {

struct BytecodeEmitter;
class SharedContext;

class MOZ_STACK_CLASS PropertyEmitter {
 public:
  enum class Kind {
    Prototype,

    Static
  };

 protected:
  BytecodeEmitter* bce_;

  bool isClass_ = false;

  bool isStatic_ = false;

  bool isIndexOrComputed_ = false;

#ifdef DEBUG
  enum class PropertyState {
    Start,

    PropValue,

    InitHomeObj,

    PrivateMethodValue,

    InitHomeObjForPrivateMethod,

    PrivateStaticMethod,

    InitHomeObjForPrivateStaticMethod,

    IndexKey,

    IndexValue,

    InitHomeObjForIndex,

    ComputedKey,

    ComputedValue,

    InitHomeObjForComputed,

    ProtoValue,

    SpreadOperand,

    Init,
  };
  PropertyState propertyState_ = PropertyState::Start;
#endif

 public:
  explicit PropertyEmitter(BytecodeEmitter* bce);

  [[nodiscard]] bool prepareForProtoValue(uint32_t keyPos);
  [[nodiscard]] bool emitMutateProto();

  [[nodiscard]] bool prepareForSpreadOperand(uint32_t spreadPos);
  [[nodiscard]] bool emitSpread();

  [[nodiscard]] bool prepareForPropValue(uint32_t keyPos, Kind kind);

  [[nodiscard]] bool prepareForPrivateMethod();

  [[nodiscard]] bool prepareForPrivateStaticMethod(uint32_t keyPos);

  [[nodiscard]] bool prepareForIndexPropKey(uint32_t keyPos, Kind kind);
  [[nodiscard]] bool prepareForIndexPropValue();

  [[nodiscard]] bool prepareForComputedPropKey(uint32_t keyPos, Kind kind);
  [[nodiscard]] bool prepareForComputedPropValue();

  [[nodiscard]] bool emitInitHomeObject();

  [[nodiscard]] bool emitInit(AccessorType accessorType,
                              TaggedParserAtomIndex key);

  [[nodiscard]] bool emitInitIndexOrComputed(AccessorType accessorType);

  [[nodiscard]] bool emitPrivateStaticMethod(AccessorType accessorType);

  [[nodiscard]] bool skipInit();

 private:
  [[nodiscard]] MOZ_ALWAYS_INLINE bool prepareForProp(uint32_t keyPos,
                                                      bool isStatic,
                                                      bool isComputed);

  [[nodiscard]] bool emitInit(JSOp op, TaggedParserAtomIndex key);
  [[nodiscard]] bool emitInitIndexOrComputed(JSOp op);

  [[nodiscard]] bool emitPopClassConstructor();
};

class MOZ_STACK_CLASS ObjectEmitter : public PropertyEmitter {
 private:
#ifdef DEBUG
  enum class ObjectState {
    Start,

    Object,

    End,
  };
  ObjectState objectState_ = ObjectState::Start;
#endif

 public:
  explicit ObjectEmitter(BytecodeEmitter* bce);

  [[nodiscard]] bool emitObject(size_t propertyCount);
  [[nodiscard]] bool emitObjectWithTemplateOnStack();
  [[nodiscard]] bool emitEnd();
};

class MOZ_RAII AutoSaveLocalStrictMode {
  SharedContext* sc_;
  bool savedStrictness_;

 public:
  explicit AutoSaveLocalStrictMode(SharedContext* sc);
  ~AutoSaveLocalStrictMode();

  void restore();
};

class MOZ_STACK_CLASS ClassEmitter : public PropertyEmitter {
 public:
  enum class Kind {
    Expression,

    Declaration,
  };

 private:

  bool isDerived_ = false;

  mozilla::Maybe<TDZCheckCache> tdzCache_;
  mozilla::Maybe<EmitterScope> innerScope_;
  mozilla::Maybe<TDZCheckCache> bodyTdzCache_;
  mozilla::Maybe<EmitterScope> bodyScope_;
  AutoSaveLocalStrictMode strictMode_;

#ifdef DEBUG
  // clang-format off
  // clang-format on
  enum class ClassState {
    Start,

    Scope,

    BodyScope,

    Class,

    InitConstructor,

    InstanceMemberInitializers,

    InstanceMemberInitializersEnd,

    StaticMemberInitializers,

    StaticMemberInitializersEnd,

    BoundName,

    End,
  };
  ClassState classState_ = ClassState::Start;

  // clang-format off
  // clang-format on
  enum class MemberState {
    Start,

    Initializer,

    InitializerWithHomeObject,
  };
  MemberState memberState_ = MemberState::Start;

  size_t numInitializers_ = 0;
#endif

  TaggedParserAtomIndex name_;
  TaggedParserAtomIndex nameForAnonymousClass_;
  bool hasNameOnStack_ = false;
  mozilla::Maybe<NameOpEmitter> initializersAssignment_;
  size_t initializerIndex_ = 0;

 public:
  explicit ClassEmitter(BytecodeEmitter* bce);

  bool emitScope(LexicalScope::ParserData* scopeBindings);
  bool emitBodyScope(ClassBodyScope::ParserData* scopeBindings);

  [[nodiscard]] bool emitClass(TaggedParserAtomIndex name,
                               TaggedParserAtomIndex nameForAnonymousClass,
                               bool hasNameOnStack, uint8_t membersCount);
  [[nodiscard]] bool emitDerivedClass(
      TaggedParserAtomIndex name, TaggedParserAtomIndex nameForAnonymousClass,
      bool hasNameOnStack);

  [[nodiscard]] bool emitInitConstructor(bool needsHomeObject);

  [[nodiscard]] bool prepareForMemberInitializers(size_t numInitializers,
                                                  bool isStatic);
  [[nodiscard]] bool prepareForMemberInitializer();
  [[nodiscard]] bool emitMemberInitializerHomeObject(bool isStatic);
  [[nodiscard]] bool emitStoreMemberInitializer();
  [[nodiscard]] bool emitMemberInitializersEnd();

#ifdef ENABLE_DECORATORS
  [[nodiscard]] bool prepareForExtraInitializers(
      TaggedParserAtomIndex initializers);
#endif

  [[nodiscard]] bool emitBinding();

#ifdef ENABLE_DECORATORS
  [[nodiscard]] bool prepareForDecorators();
#endif

  [[nodiscard]] bool emitEnd(Kind kind);

 private:
  [[nodiscard]] bool initProtoAndCtor();

  [[nodiscard]] bool leaveBodyAndInnerScope();
};

} 
} 

#endif /* frontend_ObjectEmitter_h */
