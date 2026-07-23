/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef DOM_FS_PARENT_FILESYSTEMMANAGER_H_
#define DOM_FS_PARENT_FILESYSTEMMANAGER_H_

#include <cstdint>
#include <functional>

enum class nsresult : uint32_t;

template <class T>
class RefPtr;

namespace mozilla {

namespace ipc {

template <class T>
class Endpoint;

class IPCResult;
class PBackgroundParent;
class PrincipalInfo;

}  

namespace dom {

class PFileSystemManagerParent;

mozilla::ipc::IPCResult CreateFileSystemManagerParent(
    RefPtr<mozilla::ipc::PBackgroundParent> aBackgroundActor,
    const mozilla::ipc::PrincipalInfo& aPrincipalInfo,
    mozilla::ipc::Endpoint<mozilla::dom::PFileSystemManagerParent>&&
        aParentEndpoint,
    std::function<void(const nsresult&)>&& aResolver);

}  
}  

#endif  // DOM_FS_PARENT_FILESYSTEMMANAGER_H_
