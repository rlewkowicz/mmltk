// Copyright (c) 2009 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.


#if !defined(BASE_PROCESS_UTIL_H_)
#define BASE_PROCESS_UTIL_H_

#include "base/basictypes.h"

#  include <dirent.h>
#  include <limits.h>
#  include <sys/types.h>

#include <cstddef>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "base/process.h"

#include "mozilla/UniquePtr.h"
#include "mozilla/UniquePtrExtensions.h"
#include "mozilla/Result.h"
#include "mozilla/ResultVariant.h"

#include "mozilla/ipc/LaunchError.h"


class CommandLine;

namespace base {

using mozilla::Err;
using mozilla::Ok;
using mozilla::Result;
using mozilla::ipc::LaunchError;

enum ProcessArchitecture {
  PROCESS_ARCH_INVALID = 0x0,
  PROCESS_ARCH_I386 = 0x1,
  PROCESS_ARCH_X86_64 = 0x2,
  PROCESS_ARCH_PPC = 0x4,
  PROCESS_ARCH_PPC_64 = 0x8,
  PROCESS_ARCH_ARM = 0x10,
  PROCESS_ARCH_ARM_64 = 0x20
};

enum {
  PROCESS_END_NORMAL_TERMINATON = 0,
  PROCESS_END_KILLED_BY_USER = 1,
  PROCESS_END_PROCESS_WAS_HUNG = 2
};

ProcessId GetCurrentProcId();

ProcessHandle GetCurrentProcessHandle();

bool OpenProcessHandle(ProcessId pid, ProcessHandle* handle);

bool OpenPrivilegedProcessHandle(ProcessId pid, ProcessHandle* handle);

void CloseProcessHandle(ProcessHandle process);

ProcessId GetProcId(ProcessHandle process);

#if defined(XP_UNIX)
void CloseSuperfluousFds(void* aCtx, bool (*aShouldPreserve)(void*, int));

typedef std::vector<std::pair<int, int> > file_handle_mapping_vector;
typedef std::map<std::string, std::string> environment_map;

struct FreeEnvVarsArray {
  void operator()(char** array);
};

typedef mozilla::UniquePtr<char*[], FreeEnvVarsArray> EnvironmentArray;
#endif

struct LaunchOptions {
  bool wait = false;

#if defined(XP_UNIX)
  environment_map env_map;

  EnvironmentArray full_env;

  std::string workdir;

  file_handle_mapping_vector fds_to_remap;
#endif


};

Result<Ok, LaunchError> LaunchApp(const std::vector<std::string>& argv,
                                  LaunchOptions&& options,
                                  ProcessHandle* process_handle);

EnvironmentArray BuildEnvironmentArray(const environment_map& env_vars_to_set);

bool KillProcess(ProcessHandle process, int exit_code);

#if defined(XP_UNIX)
enum class BlockingWait { No, Yes };
enum class ProcessStatus { Running, Exited, Killed, Error };

ProcessStatus WaitForProcess(ProcessHandle handle, BlockingWait blocking,
                             int* info_out);
#endif

}  

namespace mozilla {

class EnvironmentLog {
 public:
  template <size_t N>
  explicit EnvironmentLog(const char (&varname)[N])
      : EnvironmentLog(varname, N) {}

  ~EnvironmentLog() {}

  void print(const char* format, ...);

 private:
  explicit EnvironmentLog(const char* varname, size_t len);

  std::string fname_;

  DISALLOW_EVIL_CONSTRUCTORS(EnvironmentLog);
};

}  


#endif
