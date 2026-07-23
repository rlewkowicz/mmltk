/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef _mozilla_dom_WorkerIPCUtils_h
#define _mozilla_dom_WorkerIPCUtils_h

#include "ipc/IPCMessageUtils.h"
#include "mozilla/dom/BindingIPCUtils.h"

#include "mozilla/dom/FetchIPCTypes.h"

#undef None

#include "mozilla/dom/WorkerBinding.h"

namespace IPC {

template <>
struct ParamTraits<mozilla::dom::WorkerType>
    : public mozilla::dom::WebIDLEnumSerializer<mozilla::dom::WorkerType> {};

DEFINE_IPC_SERIALIZER_WITH_FIELDS(mozilla::dom::WorkerOptions, mType,
                                  mCredentials, mName);

}  

#endif  // _mozilla_dom_WorkerIPCUtils_h
