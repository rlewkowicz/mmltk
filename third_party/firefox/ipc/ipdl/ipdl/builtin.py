# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.


CTypes = (
    "bool",
    "char",
    "short",
    "int",
    "long",
    "float",
    "double",
)

Types = (
    "int8_t",
    "uint8_t",
    "int16_t",
    "uint16_t",
    "int32_t",
    "uint32_t",
    "int64_t",
    "uint64_t",
    "intptr_t",
    "uintptr_t",
    "nsresult",
    "nsString",
    "nsCString",
    "mozilla::ipc::Shmem",
    "mozilla::ipc::ByteBuf",
    "mozilla::UniquePtr",
    "mozilla::ipc::FileDescriptor",
)


HeaderIncludes = (
    "mozilla/Attributes.h",
    "IPCMessageStart.h",
    "mozilla/RefPtr.h",
    "nsString.h",
    "nsTArray.h",
    "nsTHashtable.h",
    "mozilla/MozPromise.h",
    "mozilla/OperatorNewExtensions.h",
    "mozilla/UniquePtr.h",
    "mozilla/ipc/ByteBuf.h",
    "mozilla/ipc/FileDescriptor.h",
    "mozilla/ipc/IPCForwards.h",
    "mozilla/ipc/Shmem.h",
)

CppIncludes = (
    "ipc/IPCMessageUtils.h",
    "ipc/IPCMessageUtilsSpecializations.h",
    "nsIFile.h",
    "mozilla/ipc/Endpoint.h",
    "mozilla/ipc/ProtocolMessageUtils.h",
    "mozilla/ipc/ProtocolUtils.h",
    "mozilla/ipc/ShmemMessageUtils.h",
    "mozilla/ipc/TaintingIPCUtils.h",
)
