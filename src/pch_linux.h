#pragma once
// Explicit Linux/POSIX surface. Also usable without a compiled PCH artifact.
// Inventory: ./mmltk --audit-includes report (three or more distinct first-party files).
#include <arpa/inet.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <linux/ioprio.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <sys/eventfd.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/timerfd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
