/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#if !defined(FileOperations_h)
#define FileOperations_h

#include <stdio.h>
#include <string>

#define PATHSEP_CHAR '/'
#define PATHSEP_STRING "/"

void ensurePath(std::string Path);

std::string getAbsolutePath(const std::string &Filename);

struct AutoLockFile {
  std::string Filename;

  int FileDescriptor = -1;

  AutoLockFile(const std::string &SrcFile, const std::string &DstFile);
  ~AutoLockFile();

  bool success();


  FILE *openTmp();
  bool moveTmp();
};

#endif
