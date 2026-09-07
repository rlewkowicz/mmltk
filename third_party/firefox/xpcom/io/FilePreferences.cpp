/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "FilePreferences.h"

#include "mozilla/Atomics.h"
#include "mozilla/ClearOnShutdown.h"
#include "mozilla/Preferences.h"
#include "mozilla/StaticMutex.h"
#include "mozilla/StaticPtr.h"
#include "mozilla/TextUtils.h"
#include "mozilla/Tokenizer.h"
#include "nsAppDirectoryServiceDefs.h"
#include "nsDirectoryServiceDefs.h"
#include "nsDirectoryServiceUtils.h"
#include "nsString.h"

namespace mozilla {
namespace FilePreferences {

static StaticMutex sMutex;

static bool sBlockUNCPaths = false;
typedef nsTArray<nsString> WinPaths;

static WinPaths& PathAllowlist() MOZ_REQUIRES(sMutex) {
  sMutex.AssertCurrentThreadOwns();

  static WinPaths sPaths MOZ_GUARDED_BY(sMutex);
  return sPaths;
}

typedef char char_path_t;

static bool sForbiddenPathsEmpty = false;
static Atomic<bool, Relaxed> sForbiddenPathsEmptyQuickCheck{false};

typedef nsTArray<nsTString<char_path_t>> Paths;
static StaticAutoPtr<Paths> sForbiddenPaths;

static Paths& ForbiddenPaths() {
  sMutex.AssertCurrentThreadOwns();
  if (!sForbiddenPaths) {
    sForbiddenPaths = new nsTArray<nsTString<char_path_t>>();
    ClearOnShutdown(&sForbiddenPaths);
  }
  return *sForbiddenPaths;
}

static void AllowUNCDirectory(char const* directory) {
  nsCOMPtr<nsIFile> file;
  NS_GetSpecialDirectory(directory, getter_AddRefs(file));
  if (!file) {
    return;
  }

  nsString path;
  if (NS_FAILED(file->GetTarget(path))) {
    return;
  }

  if (!StringBeginsWith(path, u"\\\\"_ns)) {
    return;
  }

  StaticMutexAutoLock lock(sMutex);

  if (!PathAllowlist().Contains(path)) {
    PathAllowlist().AppendElement(path);
  }
}

void InitPrefs() {
  sBlockUNCPaths =
      Preferences::GetBool("network.file.disable_unc_paths", false);

  nsTAutoString<char_path_t> forbidden;
  Preferences::GetCString("network.file.path_blacklist", forbidden);

  StaticMutexAutoLock lock(sMutex);

  if (forbidden.IsEmpty()) {
    sForbiddenPathsEmptyQuickCheck = (sForbiddenPathsEmpty = true);
    return;
  }

  ForbiddenPaths().Clear();
  TTokenizer<char_path_t> p(forbidden);
  while (!p.CheckEOF()) {
    nsTString<char_path_t> path;
    (void)p.ReadUntil(TTokenizer<char_path_t>::Token::Char(','), path);
    path.Trim(" ");
    if (!path.IsEmpty()) {
      ForbiddenPaths().AppendElement(path);
    }
    (void)p.CheckChar(',');
  }

  sForbiddenPathsEmptyQuickCheck =
      (sForbiddenPathsEmpty = ForbiddenPaths().Length() == 0);
}

void InitDirectoriesAllowlist() {
  AllowUNCDirectory(NS_GRE_DIR);
  AllowUNCDirectory(NS_APP_USER_PROFILE_50_DIR);
  AllowUNCDirectory(NS_APP_USER_PROFILE_LOCAL_50_DIR);
}

namespace {  

template <typename TChar>
class TNormalizer : public TTokenizer<TChar> {
  typedef TTokenizer<TChar> base;

 public:
  typedef typename base::Token Token;

  TNormalizer(const nsTSubstring<TChar>& aFilePath, const Token& aSeparator)
      : TTokenizer<TChar>(aFilePath), mSeparator(aSeparator) {}

  bool Get(nsTSubstring<TChar>& aNormalizedFilePath) {
    aNormalizedFilePath.Truncate();


    if (base::Check(mSeparator)) {
      aNormalizedFilePath.Append(mSeparator.AsChar());
    }

    while (base::HasInput()) {
      if (!ConsumeName()) {
        return false;
      }
    }

    for (auto const& name : mStack) {
      aNormalizedFilePath.Append(name);
    }

    return true;
  }

 private:
  bool ConsumeName() {
    if (base::CheckEOF()) {
      return true;
    }

    if (CheckCurrentDir()) {
      return true;
    }

    if (CheckParentDir()) {
      if (!mStack.Length()) {
        return false;
      }

      mStack.RemoveLastElement();
      return true;
    }

    nsTDependentSubstring<TChar> name;
    if (base::ReadUntil(mSeparator, name, base::INCLUDE_LAST) &&
        name.Length() == 1) {
      return false;
    }
    mStack.AppendElement(name);

    return true;
  }

  bool CheckParentDir() {
    typename nsTString<TChar>::const_char_iterator cursor = base::mCursor;
    if (base::CheckChar('.') && base::CheckChar('.') && CheckSeparator()) {
      return true;
    }

    base::mCursor = cursor;
    return false;
  }

  bool CheckCurrentDir() {
    typename nsTString<TChar>::const_char_iterator cursor = base::mCursor;
    if (base::CheckChar('.') && CheckSeparator()) {
      return true;
    }

    base::mCursor = cursor;
    return false;
  }

  bool CheckSeparator() { return base::Check(mSeparator) || base::CheckEOF(); }

  Token const mSeparator;
  nsTArray<nsTDependentSubstring<TChar>> mStack;
};


}  

bool IsBlockedUNCPath(const nsAString& aFilePath) {
  typedef TNormalizer<char16_t> Normalizer;
  if (!sBlockUNCPaths) {
    return false;
  }

  if (!StringBeginsWith(aFilePath, u"\\\\"_ns)) {
    return false;
  }


  nsAutoString normalized;
  if (!Normalizer(aFilePath, Normalizer::Token::Char('\\')).Get(normalized)) {
    return true;
  }

  StaticMutexAutoLock lock(sMutex);

  for (const auto& allowedPrefix : PathAllowlist()) {
    if (StringBeginsWith(normalized, allowedPrefix)) {
      if (normalized.Length() == allowedPrefix.Length()) {
        return false;
      }
      if (normalized[allowedPrefix.Length()] == L'\\') {
        return false;
      }

      break;
    }
  }

  return true;
}

const char kPathSeparator = '/';

bool IsAllowedPath(const nsTSubstring<char_path_t>& aFilePath) {
  typedef TNormalizer<char_path_t> Normalizer;

  if (sForbiddenPathsEmptyQuickCheck) {
    return true;
  }

  StaticMutexAutoLock lock(sMutex);

  if (sForbiddenPathsEmpty) {
    return true;
  }

  if (!sForbiddenPaths) {
    return true;
  }

  nsTAutoString<char_path_t> normalized;
  if (!Normalizer(aFilePath, Normalizer::Token::Char(kPathSeparator))
           .Get(normalized)) {
    return false;
  }

  for (const auto& prefix : ForbiddenPaths()) {
    if (StringBeginsWith(normalized, prefix)) {
      if (normalized.Length() > prefix.Length() &&
          normalized[prefix.Length()] != kPathSeparator) {
        continue;
      }
      return false;
    }
  }

  return true;
}


void testing::SetBlockUNCPaths(bool aBlock) { sBlockUNCPaths = aBlock; }

void testing::AddDirectoryToAllowlist(nsAString const& aPath) {
  StaticMutexAutoLock lock(sMutex);
  PathAllowlist().AppendElement(aPath);
}

bool testing::NormalizePath(nsAString const& aPath, nsAString& aNormalized) {
  typedef TNormalizer<char16_t> Normalizer;
  Normalizer normalizer(aPath, Normalizer::Token::Char('\\'));
  return normalizer.Get(aNormalized);
}

}  
}  
