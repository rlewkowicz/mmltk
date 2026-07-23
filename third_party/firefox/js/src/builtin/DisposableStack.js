/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

function $DisposableStackDispose() {
  var disposableStack = this;

  if (!IsObject(disposableStack) || (disposableStack = GuardToDisposableStackHelper(disposableStack)) === null) {
    return callFunction(
      CallDisposableStackMethodIfWrapped,
      this,
      "$DisposableStackDispose"
    );
  }

  var state = UnsafeGetReservedSlot(disposableStack, DISPOSABLE_STACK_STATE_SLOT);

  if (state === DISPOSABLE_STACK_STATE_DISPOSED) {
    return undefined;
  }

  UnsafeSetReservedSlot(disposableStack, DISPOSABLE_STACK_STATE_SLOT, DISPOSABLE_STACK_STATE_DISPOSED);

  var disposeCapability = UnsafeGetReservedSlot(disposableStack, DISPOSABLE_STACK_DISPOSABLE_RESOURCE_STACK_SLOT);
  UnsafeSetReservedSlot(disposableStack, DISPOSABLE_STACK_DISPOSABLE_RESOURCE_STACK_SLOT, undefined);
  if (disposeCapability === undefined) {
    return undefined;
  }
  DisposeResourcesSync(disposeCapability, disposeCapability.length);

  return undefined;
}
SetCanonicalName($DisposableStackDispose, "dispose");
