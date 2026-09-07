/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

function Promise_finally(onFinally) {
  var promise = this;

  if (!IsObject(promise)) {
    ThrowTypeError(JSMSG_INCOMPATIBLE_PROTO, "Promise", "finally", "value");
  }

  var C = SpeciesConstructor(promise, GetBuiltinConstructor("Promise"));

  assert(IsConstructor(C), "SpeciesConstructor returns a constructor function");

  var thenFinally, catchFinally;
  if (!IsCallable(onFinally)) {
    thenFinally = onFinally;
    catchFinally = onFinally;
  } else {
    // prettier-ignore
    (thenFinally) = function(value) {

      var result = callContentFunction(onFinally, undefined);


      var promise = PromiseResolve(C, result);


      return callContentFunction(promise.then, promise, function() {
        return value;
      });
    };

    // prettier-ignore
    (catchFinally) = function(reason) {

      var result = callContentFunction(onFinally, undefined);


      var promise = PromiseResolve(C, result);


      return callContentFunction(promise.then, promise, function() {
        throw reason;
      });
    };
  }

  return callContentFunction(promise.then, promise, thenFinally, catchFinally);
}
