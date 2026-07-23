/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef DOM_MEDIA_IPC_REMOTEDECODEUTILS_H_
#define DOM_MEDIA_IPC_REMOTEDECODEUTILS_H_

#include "mozilla/Logging.h"
#include "mozilla/RemoteMediaManagerChild.h"
#include "mozilla/ipc/UtilityProcessTypes.h"

namespace mozilla {

inline LazyLogModule gRemoteDecodeLog{"RemoteDecode"};

ipc::UtilityProcessKind GetCurrentUtilityProcessKind();

ipc::UtilityProcessKind GetUtilityProcessKindFromLocation(RemoteMediaIn aLocation);

RemoteMediaIn GetRemoteMediaInFromKind(ipc::UtilityProcessKind aKind);

RemoteMediaIn GetRemoteMediaInFromVideoBridgeSource(
    layers::VideoBridgeSource aSource);

layers::VideoBridgeSource GetVideoBridgeSourceFromRemoteMediaIn(
    RemoteMediaIn aSource);

const char* RemoteMediaInToStr(RemoteMediaIn aLocation);

}  

#endif  // DOM_MEDIA_IPC_REMOTEDECODEUTILS_H_
