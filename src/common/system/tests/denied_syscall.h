#pragma once
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cerrno>
#include <cstddef>
#include <stdexcept>
namespace mmltk::common::system::test_support {
// A child-only syscall denial exercises the ordinary error path without
// weakening or changing the parent's privileges and execution policy.
template <class Work>
int with_denied_syscall(int number, Work&& work) {
 const auto child = ::fork();
 if (child < 0) throw std::runtime_error("cannot fork syscall denial fixture");
 if (child == 0) {
  if (number == SYS_setpriority && ::setpriority(PRIO_PROCESS, 0, 0) != 0) _exit(2);
  sock_filter instructions[] = {
   BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, nr)),
   BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, static_cast<unsigned>(number), 0, 1),
   BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
   BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
  };
  sock_fprog program{4, instructions};
  if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) || ::prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program)) _exit(2);
  try {
   _exit(work() ? 0 : 1);
  } catch (...) { _exit(3); }
 }
 int status{};
 while (::waitpid(child, &status, 0) < 0)
  if (errno != EINTR) throw std::runtime_error("cannot join syscall fixture");
 return WIFEXITED(status) ? WEXITSTATUS(status) : 4;
}
}  // namespace mmltk::common::system::test_support
