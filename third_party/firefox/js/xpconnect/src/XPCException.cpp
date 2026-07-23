/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


#include "xpcprivate.h"
#include "nsError.h"


static const struct ResultMap {
  nsresult rv;
  const char* name;
  const char* format;
} map[] = {
#define XPC_MSG_DEF(val, format) {(val), #val, format},
#include "xpc.msg"
#include "extra_xpc_msg.h"
#undef XPC_MSG_DEF
    {NS_OK, nullptr, nullptr}  
};

#define RESULT_COUNT (std::size(map) - 1)

bool nsXPCException::NameAndFormatForNSResult(nsresult rv, const char** name,
                                              const char** format) {
  for (const ResultMap* p = map; p->name; p++) {
    if (rv == p->rv) {
      if (name) *name = p->name;
      if (format) *format = p->format;
      return true;
    }
  }
  return false;
}

const void* nsXPCException::IterateNSResults(nsresult* rv, const char** name,
                                             const char** format,
                                             const void** iterp) {
  const ResultMap* p = (const ResultMap*)*iterp;
  if (!p) {
    p = map;
  } else {
    p++;
  }
  if (!p->name) {
    p = nullptr;
  } else {
    if (rv) {
      *rv = p->rv;
    }
    if (name) {
      *name = p->name;
    }
    if (format) {
      *format = p->format;
    }
  }
  *iterp = p;
  return p;
}

uint32_t nsXPCException::GetNSResultCount() { return RESULT_COUNT; }
