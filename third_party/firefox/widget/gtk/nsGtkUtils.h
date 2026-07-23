/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef nsGtkUtils_h_
#define nsGtkUtils_h_

#include <glib.h>

template <class T>
static inline gpointer FuncToGpointer(T aFunction) {
  return reinterpret_cast<gpointer>(
      reinterpret_cast<uintptr_t>
      (reinterpret_cast<void (*)()>(aFunction)));
}

template <class T>
static inline void MozClearPointer(T*& pointer, void (*destroy)(T*)) {
  T* hold = pointer;
  pointer = nullptr;
  if (hold) {
    destroy(hold);
  }
}

template <class T>
static inline void MozClearHandleID(T& handle, gboolean (*destroy)(T)) {
  if (handle) {
    destroy(handle);
    handle = 0;
  }
}

#endif  // nsGtkUtils_h_
