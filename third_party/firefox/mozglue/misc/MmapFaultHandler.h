/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#if !defined(MmapFaultHandler_h_)
#define MmapFaultHandler_h_

#if defined(__wasi__)

#  define MMAP_FAULT_HANDLER_BEGIN_HANDLE(fd) {
#  define MMAP_FAULT_HANDLER_BEGIN_BUFFER(buf, bufLen) {
#  define MMAP_FAULT_HANDLER_CATCH(retval) }

#else

#  include "mozilla/Attributes.h"
#  include "mozilla/Types.h"
#  include <stdint.h>
#  include <setjmp.h>

class MOZ_RAII MmapAccessScope {
 public:
  MFBT_API MmapAccessScope(void* aBuf, uint32_t aBufLen,
                           const char* aFilename = nullptr);
  MFBT_API ~MmapAccessScope();

  MmapAccessScope(const MmapAccessScope&) = delete;
  MmapAccessScope& operator=(const MmapAccessScope&) = delete;

  void SetThreadLocalScope();
  bool IsInsideBuffer(void* aPtr);
  void CrashWithInfo(void* aPtr);

  sigjmp_buf mJmpBuf;

 private:
  void* mBuf;
  const char* mFilename;
  uint32_t mBufLen;
  MmapAccessScope* mPreviousScope;
};

template <typename T>
inline bool ValidFD(T fd) {
  return !!fd;
}

#  define MMAP_FAULT_HANDLER_BEGIN_HANDLE(fd)                   \
    {                                                           \
      void* mmapScopeBuf = nullptr;                             \
      nsCString mmapScopeFilename;                              \
      uint32_t mmapScopeBufLen = 0;                             \
      if (ValidFD(fd) && fd->mFileStart && fd->mTotalLen > 0) { \
        mmapScopeBuf = (void*)fd->mFileStart;                   \
        mmapScopeBufLen = fd->mTotalLen;                        \
      }                                                         \
      if (ValidFD(fd) && fd->mFile) {                           \
        nsCOMPtr<nsIFile> file = fd->mFile.GetBaseFile();       \
        if (file) {                                             \
          file->GetNativeLeafName(mmapScopeFilename);           \
        }                                                       \
      }                                                         \
      MmapAccessScope mmapScope(mmapScopeBuf, mmapScopeBufLen,  \
                                mmapScopeFilename.get());       \
      if (sigsetjmp(mmapScope.mJmpBuf, 0) == 0) {
#  define MMAP_FAULT_HANDLER_BEGIN_BUFFER(buf, bufLen)   \
    {                                                    \
      MmapAccessScope mmapScope((void*)(buf), (bufLen)); \
      if (sigsetjmp(mmapScope.mJmpBuf, 0) == 0) {
#  define MMAP_FAULT_HANDLER_CATCH(retval)                       \
    }                                                            \
    else {                                                       \
      NS_WARNING("SIGBUS received when accessing mmapped file"); \
      return retval;                                             \
    }                                                            \
    }

#endif

#endif
