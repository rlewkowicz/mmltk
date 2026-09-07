/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef GPU_PROMISE_HELPERS_H_
#define GPU_PROMISE_HELPERS_H_

#include "mozilla/dom/Promise.h"


namespace mozilla::webgpu::promise {

void MaybeRejectWithTypeError(RefPtr<dom::Promise>&& promise,
                              nsCString&& aMessage);
void MaybeRejectWithOperationError(RefPtr<dom::Promise>&& promise,
                                   nsCString&& aMessage);
void MaybeRejectWithAbortError(RefPtr<dom::Promise>&& promise,
                               nsCString&& aMessage);
void MaybeRejectWithNotSupportedError(RefPtr<dom::Promise>&& promise,
                                      nsCString&& aMessage);
void MaybeRejectWithInvalidStateError(RefPtr<dom::Promise>&& promise,
                                      nsCString&& aMessage);
void MaybeRejectWithPipelineError(RefPtr<dom::Promise>&& promise,
                                  RefPtr<PipelineError>&& aError);

void MaybeResolveWithUndefined(RefPtr<dom::Promise>&& promise);
void MaybeResolveWithNull(RefPtr<dom::Promise>&& promise);

template <typename T>
void MaybeResolve(RefPtr<dom::Promise>&& promise, RefPtr<T>&& aArg) {
  NS_DispatchToCurrentThread(NewCancelableRunnableMethod<RefPtr<T>>(
      "webgpu::PromiseHelpers::MaybeResolve", std::move(promise),
      &dom::Promise::MaybeResolve<RefPtr<T>>, std::move(aArg)));
}

}  

#endif  // GPU_PROMISE_HELPERS_H_
