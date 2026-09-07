pub(crate) mod poll_fd;
pub(crate) mod types;

pub(crate) mod syscalls;

#[cfg(any(linux_kernel, target_os = "illumos", target_os = "redox"))]
pub mod epoll;
