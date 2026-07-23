/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "FileOperations.h"

#include <stdio.h>
#include <stdlib.h>

#include <sys/file.h>
#include <sys/time.h>
#include <unistd.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

void ensurePath(std::string Path) {
  size_t Pos = 0;
  if (Path[0] == PATHSEP_CHAR) {
    Pos++;
  }

  while ((Pos = Path.find(PATHSEP_CHAR, Pos)) != std::string::npos) {
    std::string Portion = Path.substr(0, Pos);
    if (!Portion.empty()) {
      int Err = mkdir(Portion.c_str(), 0775);
      if (Err == -1 && errno != EEXIST) {
        perror("mkdir failed");
        exit(1);
      }
    }

    Pos++;
  }
}

AutoLockFile::AutoLockFile(const std::string &SrcFile,
                           const std::string &DstFile) {
  this->Filename = DstFile;
  FileDescriptor = open(SrcFile.c_str(), O_RDONLY);
  if (FileDescriptor == -1) {
    return;
  }

  do {
    int rv = flock(FileDescriptor, LOCK_EX);
    if (rv == 0) {
      break;
    }
  } while (true);
}

AutoLockFile::~AutoLockFile() { close(FileDescriptor); }

bool AutoLockFile::success() { return FileDescriptor != -1; }

FILE *AutoLockFile::openTmp() {
  int TmpDescriptor =
      open((Filename + ".tmp").c_str(), O_WRONLY | O_APPEND | O_CREAT, 0666);
  return fdopen(TmpDescriptor, "ab");
}

bool AutoLockFile::moveTmp() {
  if (unlink(Filename.c_str()) == -1) {
    if (errno != ENOENT) {
      return false;
    }
  }
  return rename((Filename + ".tmp").c_str(), Filename.c_str()) == 0;
}

std::string getAbsolutePath(const std::string &Filename) {
  char Full[4096];
  if (!realpath(Filename.c_str(), Full)) {
    return std::string("");
  }
  return std::string(Full);
}
