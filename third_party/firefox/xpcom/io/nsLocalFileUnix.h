/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


#if !defined(_nsLocalFileUNIX_H_)
#define _nsLocalFileUNIX_H_

#include <sys/stat.h>

#include "nscore.h"
#include "nsString.h"

#if defined(HAVE_STAT64) && defined(HAVE_LSTAT64) && !0
#  define STAT stat64
#  define LSTAT lstat64
#  define HAVE_STATS64 1
#else
#  define STAT stat
#  define LSTAT lstat
#endif

#if defined(HAVE_SYS_QUOTA_H) && defined(HAVE_LINUX_QUOTA_H)
#  define USE_LINUX_QUOTACTL
#endif

class nsLocalFile final
    : public nsIFile
{
 public:
  NS_DEFINE_STATIC_CID_ACCESSOR(NS_LOCAL_FILE_CID)

  nsLocalFile();

  static nsresult nsLocalFileConstructor(const nsIID& aIID,
                                         void** aInstancePtr);

  NS_DECL_THREADSAFE_ISUPPORTS
  NS_DECL_NSIFILE

 private:
  nsLocalFile(const nsLocalFile& aOther);
  ~nsLocalFile() = default;

 protected:
  nsCString mPath;

  void LocateNativeLeafName(nsACString::const_iterator&,
                            nsACString::const_iterator&);

  nsresult CopyDirectoryTo(nsIFile* aNewParent);
  nsresult CreateAllAncestors(uint32_t aPermissions);
  nsresult GetNativeTargetPathName(nsIFile* aNewParent,
                                   const nsACString& aNewName,
                                   nsACString& aResult);

  nsresult StatFile(struct STAT* statInfo);

  nsresult CreateAndKeepOpen(uint32_t aType, int aFlags, uint32_t aPermissions,
                             bool aSkipAncestors, PRFileDesc** aResult);

  enum class TimeField { AccessedTime, ModifiedTime };
  nsresult SetTimeImpl(PRTime aTime, TimeField aTimeField, bool aFollowLinks);
  nsresult GetTimeImpl(PRTime* aTime, TimeField aTimeField, bool aFollowLinks);

  nsresult GetCreationTimeImpl(PRTime* aCreationTime, bool aFollowLinks);

#if defined(USE_LINUX_QUOTACTL)
  template <typename StatInfoFunc, typename QuotaInfoFunc>
  nsresult GetDiskInfo(StatInfoFunc&& aStatInfoFunc,
                       QuotaInfoFunc&& aQuotaInfoFunc, int64_t* aResult);
#else
  template <typename StatInfoFunc>
  nsresult GetDiskInfo(StatInfoFunc&& aStatInfoFunc, int64_t* aResult);
#endif
};

#endif
