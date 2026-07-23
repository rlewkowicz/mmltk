/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/FileUtils.h"

#include "nscore.h"
#include "private/pprio.h"
#include "prmem.h"
#include "mozilla/MemUtils.h"

#if defined(XP_UNIX)
#  include <fcntl.h>
#  include <unistd.h>
#if defined(LINUX)
#    include <elf.h>
#endif
#  include <sys/types.h>
#  include <sys/stat.h>
#endif

#if defined(MOZILLA_INTERNAL_API)

#  include "nsString.h"

bool mozilla::fallocate(PRFileDesc* aFD, int64_t aLength) {
#if defined(HAVE_POSIX_FALLOCATE)
  return posix_fallocate(PR_FileDesc2NativeHandle(aFD), 0, aLength) == 0;
#elif defined(XP_UNIX)
  int64_t oldpos = PR_Seek64(aFD, 0, PR_SEEK_CUR);
  if (oldpos == -1) {
    return false;
  }

  struct stat buf;
  int fd = PR_FileDesc2NativeHandle(aFD);
  if (fstat(fd, &buf)) {
    return false;
  }

  if (buf.st_size >= aLength) {
    return false;
  }

  const int nBlk = buf.st_blksize;

  if (!nBlk) {
    return false;
  }

  if (ftruncate(fd, aLength)) {
    return false;
  }

  int nWrite;  
  int64_t iWrite = ((buf.st_size + 2 * nBlk - 1) / nBlk) * nBlk -
                   1;  
  while (iWrite < aLength) {
    nWrite = 0;
    if (PR_Seek64(aFD, iWrite, PR_SEEK_SET) == iWrite) {
      nWrite = PR_Write(aFD, "", 1);
    }
    if (nWrite != 1) {
      break;
    }
    iWrite += nBlk;
  }

  PR_Seek64(aFD, oldpos, PR_SEEK_SET);
  return nWrite == 1;
#else
  return false;
#endif
}

void mozilla::ReadAheadLib(nsIFile* aFile) {
#if defined(LINUX) && !0 || 0
  nsAutoCString nativePath;
  if (!aFile || NS_FAILED(aFile->GetNativePath(nativePath))) {
    return;
  }
  ReadAheadLib(nativePath.get());
#endif
}

void mozilla::ReadAheadFile(nsIFile* aFile, const size_t aOffset,
                            const size_t aCount, mozilla::filedesc_t* aOutFd) {
#if defined(LINUX) && !0 || 0
  nsAutoCString nativePath;
  if (!aFile || NS_FAILED(aFile->GetNativePath(nativePath))) {
    return;
  }
  ReadAheadFile(nativePath.get(), aOffset, aCount, aOutFd);
#endif
}

mozilla::PathString mozilla::GetLibraryName(mozilla::pathstr_t aDirectory,
                                            const char* aLib) {
  char* temp = PR_GetLibraryName(aDirectory, aLib);
  if (!temp) {
    return ""_ns;
  }
  nsAutoCString libname(temp);
  PR_FreeLibraryName(temp);
  return std::move(libname);
}

mozilla::PathString mozilla::GetLibraryFilePathname(mozilla::pathstr_t aName,
                                                    PRFuncPtr aAddr) {
  char* temp = PR_GetLibraryFilePathname(aName, aAddr);
  if (!temp) {
    return ""_ns;
  }
  nsAutoCString path(temp);
  PR_Free(temp);  
  return std::move(path);
}

#endif

#if defined(LINUX) && !0

static const unsigned int bufsize = 4096;

#if defined(__LP64__)
typedef Elf64_Ehdr Elf_Ehdr;
typedef Elf64_Phdr Elf_Phdr;
static const unsigned char ELFCLASS = ELFCLASS64;
typedef Elf64_Off Elf_Off;
#else
typedef Elf32_Ehdr Elf_Ehdr;
typedef Elf32_Phdr Elf_Phdr;
static const unsigned char ELFCLASS = ELFCLASS32;
typedef Elf32_Off Elf_Off;
#endif

#endif

void mozilla::ReadAhead(mozilla::filedesc_t aFd, const size_t aOffset,
                        const size_t aCount) {
#if defined(LINUX) && !0

  readahead(aFd, aOffset, aCount);

#endif
}

void mozilla::ReadAheadLib(mozilla::pathstr_t aFilePath) {
  if (!aFilePath) {
    return;
  }



#if defined(LINUX) && !0
  int fd = open(aFilePath, O_RDONLY);
  if (fd < 0) {
    return;
  }

  union {
    char buf[bufsize];
    Elf_Ehdr ehdr;
  } elf;
  if ((read(fd, elf.buf, bufsize) <= 0) || (memcmp(elf.buf, ELFMAG, 4)) ||
      (elf.ehdr.e_ident[EI_CLASS] != ELFCLASS) ||
      (elf.ehdr.e_phoff +
           (static_cast<Elf_Off>(elf.ehdr.e_phentsize) * elf.ehdr.e_phnum) >=
       bufsize)) {
    close(fd);
    return;
  }
  Elf_Phdr* phdr = (Elf_Phdr*)&elf.buf[elf.ehdr.e_phoff];
  Elf_Off end = 0;
  for (int phnum = elf.ehdr.e_phnum; phnum; phdr++, phnum--) {
    if ((phdr->p_type == PT_LOAD) && (end < phdr->p_offset + phdr->p_filesz)) {
      end = phdr->p_offset + phdr->p_filesz;
    }
  }
  if (end > 0) {
    ReadAhead(fd, 0, end);
  }
  close(fd);
#endif
}

void mozilla::ReadAheadFile(mozilla::pathstr_t aFilePath, const size_t aOffset,
                            const size_t aCount, mozilla::filedesc_t* aOutFd) {
#if defined(LINUX) && !0 || 0
  if (!aFilePath) {
    if (aOutFd) {
      *aOutFd = -1;
    }
    return;
  }
  int fd = open(aFilePath, O_RDONLY);
  if (aOutFd) {
    *aOutFd = fd;
  }
  if (fd < 0) {
    return;
  }
  size_t count;
  if (aCount == SIZE_MAX) {
    struct stat st;
    if (fstat(fd, &st) < 0) {
      if (!aOutFd) {
        close(fd);
      }
      return;
    }
    count = st.st_size;
  } else {
    count = aCount;
  }
  ReadAhead(fd, aOffset, count);
  if (!aOutFd) {
    close(fd);
  }
#endif
}
