/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#if !defined(READSTRINGS_H_)
#define READSTRINGS_H_

#include "mozilla/Maybe.h"
#include "mozilla/UniquePtr.h"

#include <vector>

typedef char NS_tchar;

struct StringTable {
  mozilla::UniquePtr<char[]> title;
  mozilla::UniquePtr<char[]> info;
};

struct MARChannelStringTable {
  mozilla::UniquePtr<char[]> MARChannelID;

 public:
  MARChannelStringTable() = default;
  const mozilla::UniquePtr<char[]>& get() const { return MARChannelID; }
  mozilla::UniquePtr<char[]>& get() {
    if (!MARChannelID) {
      MARChannelID = mozilla::MakeUnique<char[]>(1);
      MARChannelID[0] = '\0';
    }
    return MARChannelID;
  }
};

int ReadStrings(const NS_tchar* path, StringTable* results);

int ReadStrings(const NS_tchar* path, const char* keyList,
                unsigned int numStrings, mozilla::UniquePtr<char[]>* results,
                const char* section = nullptr);

int ReadStringsFromBuffer(char* stringBuffer, const char* keyList,
                          unsigned int numStrings,
                          mozilla::UniquePtr<char[]>* results,
                          const char* section = nullptr);

class IniReader {
 public:
  explicit IniReader(const NS_tchar* iniPath, const char* section = nullptr);

  void AddKey(const char* key, mozilla::UniquePtr<char[]>* outputPtr);
  bool HasRead() { return mMaybeStatusCode.isSome(); }
  int Read();

 private:
  bool MaybeAddKey(const char* key, size_t& insertionIndex);

  mozilla::UniquePtr<NS_tchar[]> mPath;
  mozilla::UniquePtr<char[]> mSection;
  std::vector<mozilla::UniquePtr<char[]>> mKeys;

  template <class T>
  struct ValueOutput {
    size_t keyIndex;
    T* outputPtr;
  };

  std::vector<ValueOutput<mozilla::UniquePtr<char[]>>> mNarrowOutputs;
  mozilla::Maybe<int> mMaybeStatusCode;
};

#endif
