/*
 * Copyright (C) 2010 Google Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1.  Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 * 2.  Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 * 3.  Neither the name of Apple Computer, Inc. ("Apple") nor the names of
 *     its contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE AND ITS CONTRIBUTORS "AS IS" AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL APPLE OR ITS CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef HRTFDatabaseLoader_h
#define HRTFDatabaseLoader_h

#include "HRTFDatabase.h"
#include "mozilla/MemoryReporting.h"
#include "mozilla/Mutex.h"
#include "nsHashKeys.h"

template <class EntryType>
class nsTHashtable;
template <class T>
class nsAutoRef;

namespace WebCore {


class HRTFDatabaseLoader {
 public:
  static already_AddRefed<HRTFDatabaseLoader>
  createAndLoadAsynchronouslyIfNecessary(float sampleRate);

  void AddRef() {
#if defined(DEBUG) || defined(NS_BUILD_REFCNT_LOGGING)
    int count =
#endif
        ++m_refCnt;
    MOZ_ASSERT(count > 0, "invalid ref count");
    NS_LOG_ADDREF(this, count, "HRTFDatabaseLoader", sizeof(*this));
  }

  void Release() {
    int count = m_refCnt;
    MOZ_ASSERT(count > 0, "extra release");
    if (count != 1 && m_refCnt.compareExchange(count, count - 1)) {
      NS_LOG_RELEASE(this, count - 1, "HRTFDatabaseLoader");
      return;
    }

    ProxyRelease();
  }

  bool isLoaded() const;

  void waitForLoaderThreadCompletion();

  HRTFDatabase* database() {
    if (!m_databaseLoaded) {
      return nullptr;
    }
    return m_hrtfDatabase.get();
  }

  float databaseSampleRate() const { return m_databaseSampleRate; }

  static void shutdown();

  void load();

  static size_t sizeOfLoaders(mozilla::MallocSizeOf aMallocSizeOf);

 private:
  explicit HRTFDatabaseLoader(float sampleRate);
  ~HRTFDatabaseLoader();

  size_t sizeOfIncludingThis(mozilla::MallocSizeOf aMallocSizeOf) const;

  void ProxyRelease();       
  void MainThreadRelease();  
  class ProxyReleaseEvent;

  void loadAsynchronously();

  class LoaderByRateEntry : public nsFloatHashKey {
   public:
    explicit LoaderByRateEntry(KeyTypePointer aKey)
        : nsFloatHashKey(aKey),
          mLoader()  
    {}

    size_t SizeOfExcludingThis(mozilla::MallocSizeOf aMallocSizeOf) const {
      return mLoader ? mLoader->sizeOfIncludingThis(aMallocSizeOf) : 0;
    }

    HRTFDatabaseLoader* MOZ_NON_OWNING_REF mLoader;
  };

  static nsTHashtable<LoaderByRateEntry>* s_loaderMap;  

  mozilla::Atomic<int> m_refCnt;

  nsAutoRef<HRTFDatabase> m_hrtfDatabase;

  mozilla::Mutex m_threadLock;
  PRThread* m_databaseLoaderThread MOZ_GUARDED_BY(m_threadLock);

  float m_databaseSampleRate;
  mozilla::Atomic<bool> m_databaseLoaded;
};

}  

#endif  // HRTFDatabaseLoader_h
