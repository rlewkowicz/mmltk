/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_PostTraversalTask_h
#define mozilla_PostTraversalTask_h

#include "mozilla/AlreadyAddRefed.h"


namespace mozilla {
class ServoStyleSet;
namespace dom {
class FontFaceSet;
class FontFaceSetImpl;
}  
namespace fontlist {
struct Family;
}  
}  
class gfxUserFontEntry;

namespace mozilla {

class PostTraversalTask {
 public:
  static PostTraversalTask DispatchLoadingEventAndReplaceReadyPromise(
      already_AddRefed<dom::FontFaceSetImpl> aFontFaceSetImpl) {
    PostTraversalTask task(Type::DispatchLoadingEventAndReplaceReadyPromise);
    task.mTarget = aFontFaceSetImpl.take();
    return task;
  }

  static PostTraversalTask LoadFontEntry(
      already_AddRefed<gfxUserFontEntry> aFontEntry) {
    PostTraversalTask task(Type::LoadFontEntry);
    task.mTarget = aFontEntry.take();
    return task;
  }

  void Run();

  PostTraversalTask(const PostTraversalTask&) = delete;
  PostTraversalTask(PostTraversalTask&& aOther)
      : PostTraversalTask(aOther.mType) {
    mTarget = aOther.mTarget;
    aOther.mTarget = nullptr;
  };

  ~PostTraversalTask();

 private:
  enum class Type {
    DispatchLoadingEventAndReplaceReadyPromise,

    LoadFontEntry,
  };

  explicit PostTraversalTask(Type aType) : mType(aType) {}

  const Type mType;
  void* mTarget = nullptr;
};

}  

#endif  // mozilla_PostTraversalTask_h
