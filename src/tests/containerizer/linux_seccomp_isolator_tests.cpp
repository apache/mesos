// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <gtest/gtest.h>

#include <stout/gtest.hpp>
#include <stout/path.hpp>
#include <stout/stringify.hpp>
#include <stout/try.hpp>

#include "linux/capabilities.hpp"

#include "tests/cluster.hpp"
#include "tests/environment.hpp"
#include "tests/mesos.hpp"

using mesos::internal::capabilities::Capability;
using mesos::internal::capabilities::CHOWN;
using mesos::internal::capabilities::SYS_ADMIN;

using mesos::internal::slave::Containerizer;
using mesos::internal::slave::Fetcher;
using mesos::internal::slave::MesosContainerizer;

using mesos::internal::slave::state::SlaveState;

using mesos::slave::ContainerTermination;

using process::Future;
using process::Owned;

using std::list;
using std::map;
using std::ostream;
using std::set;
using std::string;

namespace mesos {
namespace internal {
namespace tests {

// This is a modified version of the Docker's default Seccomp profile:
// https://github.com/moby/moby/blob/master/profiles/seccomp/default.json
// This profile allows `pivot_root` system call which is used by the
// Mesos containerizer.
constexpr char TEST_SECCOMP_PROFILE[] = R"~(
{
  "defaultAction": "SCMP_ACT_ERRNO",
  "archMap": [
    {
      "architecture": "SCMP_ARCH_X86_64",
      "subArchitectures": [
        "SCMP_ARCH_X86",
        "SCMP_ARCH_X32"
      ]
    },
    {
      "architecture": "SCMP_ARCH_AARCH64",
      "subArchitectures": [
        "SCMP_ARCH_ARM"
      ]
    },
    {
      "architecture": "SCMP_ARCH_MIPS64",
      "subArchitectures": [
        "SCMP_ARCH_MIPS",
        "SCMP_ARCH_MIPS64N32"
      ]
    },
    {
      "architecture": "SCMP_ARCH_MIPS64N32",
      "subArchitectures": [
        "SCMP_ARCH_MIPS",
        "SCMP_ARCH_MIPS64"
      ]
    },
    {
      "architecture": "SCMP_ARCH_MIPSEL64",
      "subArchitectures": [
        "SCMP_ARCH_MIPSEL",
        "SCMP_ARCH_MIPSEL64N32"
      ]
    },
    {
      "architecture": "SCMP_ARCH_MIPSEL64N32",
      "subArchitectures": [
        "SCMP_ARCH_MIPSEL",
        "SCMP_ARCH_MIPSEL64"
      ]
    },
    {
      "architecture": "SCMP_ARCH_S390X",
      "subArchitectures": [
        "SCMP_ARCH_S390"
      ]
    }
  ],
  "syscalls": [
    {
      "names": [
        "accept",
        "accept4",
        "access",
        "adjtimex",
        "alarm",
        "bind",
        "brk",
        "capget",
        "capset",
        "chdir",
        "chmod",
        "chown",
        "chown32",
        "clock_getres",
        "clock_gettime",
        "clock_nanosleep",
        "close",
        "connect",
        "copy_file_range",
        "creat",
        "dup",
        "dup2",
        "dup3",
        "epoll_create",
        "epoll_create1",
        "epoll_ctl",
        "epoll_ctl_old",
        "epoll_pwait",
        "epoll_wait",
        "epoll_wait_old",
        "eventfd",
        "eventfd2",
        "execve",
        "execveat",
        "exit",
        "exit_group",
        "faccessat",
        "fadvise64",
        "fadvise64_64",
        "fallocate",
        "fanotify_mark",
        "fchdir",
        "fchmod",
        "fchmodat",
        "fchown",
        "fchown32",
        "fchownat",
        "fcntl",
        "fcntl64",
        "fdatasync",
        "fgetxattr",
        "flistxattr",
        "flock",
        "fork",
        "fremovexattr",
        "fsetxattr",
        "fstat",
        "fstat64",
        "fstatat64",
        "fstatfs",
        "fstatfs64",
        "fsync",
        "ftruncate",
        "ftruncate64",
        "futex",
        "futimesat",
        "getcpu",
        "getcwd",
        "getdents",
        "getdents64",
        "getegid",
        "getegid32",
        "geteuid",
        "geteuid32",
        "getgid",
        "getgid32",
        "getgroups",
        "getgroups32",
        "getitimer",
        "getpeername",
        "getpgid",
        "getpgrp",
        "getpid",
        "getppid",
        "getpriority",
        "getrandom",
        "getresgid",
        "getresgid32",
        "getresuid",
        "getresuid32",
        "getrlimit",
        "get_robust_list",
        "getrusage",
        "getsid",
        "getsockname",
        "getsockopt",
        "get_thread_area",
        "gettid",
        "gettimeofday",
        "getuid",
        "getuid32",
        "getxattr",
        "inotify_add_watch",
        "inotify_init",
        "inotify_init1",
        "inotify_rm_watch",
        "io_cancel",
        "ioctl",
        "io_destroy",
        "io_getevents",
        "ioprio_get",
        "ioprio_set",
        "io_setup",
        "io_submit",
        "ipc",
        "kill",
        "lchown",
        "lchown32",
        "lgetxattr",
        "link",
        "linkat",
        "listen",
        "listxattr",
        "llistxattr",
        "_llseek",
        "lremovexattr",
        "lseek",
        "lsetxattr",
        "lstat",
        "lstat64",
        "madvise",
        "memfd_create",
        "mincore",
        "mkdir",
        "mkdirat",
        "mknod",
        "mknodat",
        "mlock",
        "mlock2",
        "mlockall",
        "mmap",
        "mmap2",
        "mprotect",
        "mq_getsetattr",
        "mq_notify",
        "mq_open",
        "mq_timedreceive",
        "mq_timedsend",
        "mq_unlink",
        "mremap",
        "msgctl",
        "msgget",
        "msgrcv",
        "msgsnd",
        "msync",
        "munlock",
        "munlockall",
        "munmap",
        "nanosleep",
        "newfstatat",
        "_newselect",
        "open",
        "openat",
        "pause",
        "pipe",
        "pipe2",
        "poll",
        "ppoll",
        "prctl",
        "pread64",
        "preadv",
        "preadv2",
        "prlimit64",
        "pselect6",
        "pwrite64",
        "pwritev",
        "pwritev2",
        "read",
        "readahead",
        "readlink",
        "readlinkat",
        "readv",
        "recv",
        "recvfrom",
        "recvmmsg",
        "recvmsg",
        "remap_file_pages",
        "removexattr",
        "rename",
        "renameat",
        "renameat2",
        "restart_syscall",
        "rmdir",
        "rt_sigaction",
        "rt_sigpending",
        "rt_sigprocmask",
        "rt_sigqueueinfo",
        "rt_sigreturn",
        "rt_sigsuspend",
        "rt_sigtimedwait",
        "rt_tgsigqueueinfo",
        "sched_getaffinity",
        "sched_getattr",
        "sched_getparam",
        "sched_get_priority_max",
        "sched_get_priority_min",
        "sched_getscheduler",
        "sched_rr_get_interval",
        "sched_setaffinity",
        "sched_setattr",
        "sched_setparam",
        "sched_setscheduler",
        "sched_yield",
        "seccomp",
        "select",
        "semctl",
        "semget",
        "semop",
        "semtimedop",
        "send",
        "sendfile",
        "sendfile64",
        "sendmmsg",
        "sendmsg",
        "sendto",
        "setfsgid",
        "setfsgid32",
        "setfsuid",
        "setfsuid32",
        "setgid",
        "setgid32",
        "setgroups",
        "setgroups32",
        "setitimer",
        "setpgid",
        "setpriority",
        "setregid",
        "setregid32",
        "setresgid",
        "setresgid32",
        "setresuid",
        "setresuid32",
        "setreuid",
        "setreuid32",
        "setrlimit",
        "set_robust_list",
        "setsid",
        "setsockopt",
        "set_thread_area",
        "set_tid_address",
        "setuid",
        "setuid32",
        "setxattr",
        "shmat",
        "shmctl",
        "shmdt",
        "shmget",
        "shutdown",
        "sigaltstack",
        "signalfd",
        "signalfd4",
        "sigreturn",
        "socket",
        "socketcall",
        "socketpair",
        "splice",
        "stat",
        "stat64",
        "statfs",
        "statfs64",
        "statx",
        "symlink",
        "symlinkat",
        "sync",
        "sync_file_range",
        "syncfs",
        "sysinfo",
        "tee",
        "tgkill",
        "time",
        "timer_create",
        "timer_delete",
        "timerfd_create",
        "timerfd_gettime",
        "timerfd_settime",
        "timer_getoverrun",
        "timer_gettime",
        "timer_settime",
        "times",
        "tkill",
        "truncate",
        "truncate64",
        "ugetrlimit",
        "umask",
        "uname",
        "unlink",
        "unlinkat",
        "utime",
        "utimensat",
        "utimes",
        "vfork",
        "vmsplice",
        "wait4",
        "waitid",
        "waitpid",
        "write",
        "writev"
      ],
      "action": "SCMP_ACT_ALLOW",
      "args": [],
      "comment": "",
      "includes": {},
      "excludes": {}
    },
    {
      "names": [
        "personality"
      ],
      "action": "SCMP_ACT_ALLOW",
      "args": [
        {
          "index": 0,
          "value": 0,
          "valueTwo": 0,
          "op": "SCMP_CMP_EQ"
        }
      ],
      "comment": "",
      "includes": {},
      "excludes": {}
    },
    {
      "names": [
        "personality"
      ],
      "action": "SCMP_ACT_ALLOW",
      "args": [
        {
          "index": 0,
          "value": 8,
          "valueTwo": 0,
          "op": "SCMP_CMP_EQ"
        }
      ],
      "comment": "",
      "includes": {},
      "excludes": {}
    },
    {
      "names": [
        "personality"
      ],
      "action": "SCMP_ACT_ALLOW",
      "args": [
        {
          "index": 0,
          "value": 131072,
          "valueTwo": 0,
          "op": "SCMP_CMP_EQ"
        }
      ],
      "comment": "",
      "includes": {},
      "excludes": {}
    },
    {
      "names": [
        "personality"
      ],
      "action": "SCMP_ACT_ALLOW",
      "args": [
        {
          "index": 0,
          "value": 131080,
          "valueTwo": 0,
          "op": "SCMP_CMP_EQ"
        }
      ],
      "comment": "",
      "includes": {},
      "excludes": {}
    },
    {
      "names": [
        "personality"
      ],
      "action": "SCMP_ACT_ALLOW",
      "args": [
        {
          "index": 0,
          "value": 4294967295,
          "valueTwo": 0,
          "op": "SCMP_CMP_EQ"
        }
      ],
      "comment": "",
      "includes": {},
      "excludes": {}
    },
    {
      "names": [
        "sync_file_range2"
      ],
      "action": "SCMP_ACT_ALLOW",
      "args": [],
      "comment": "",
      "includes": {
        "arches": [
          "ppc64le"
        ]
      },
      "excludes": {}
    },
    {
      "names": [
        "arm_fadvise64_64",
        "arm_sync_file_range",
        "sync_file_range2",
        "breakpoint",
        "cacheflush",
        "set_tls"
      ],
      "action": "SCMP_ACT_ALLOW",
      "args": [],
      "comment": "",
      "includes": {
        "arches": [
          "arm",
          "arm64"
        ]
      },
      "excludes": {}
    },
    {
      "names": [
        "arch_prctl"
      ],
      "action": "SCMP_ACT_ALLOW",
      "args": [],
      "comment": "",
      "includes": {
        "arches": [
          "amd64",
          "x32"
        ]
      },
      "excludes": {}
    },
    {
      "names": [
        "modify_ldt"
      ],
      "action": "SCMP_ACT_ALLOW",
      "args": [],
      "comment": "",
      "includes": {
        "arches": [
          "amd64",
          "x32",
          "x86"
        ]
      },
      "excludes": {}
    },
    {
      "names": [
        "s390_pci_mmio_read",
        "s390_pci_mmio_write",
        "s390_runtime_instr"
      ],
      "action": "SCMP_ACT_ALLOW",
      "args": [],
      "comment": "",
      "includes": {
        "arches": [
          "s390",
          "s390x"
        ]
      },
      "excludes": {}
    },
    {
      "names": [
        "open_by_handle_at"
      ],
      "action": "SCMP_ACT_ALLOW",
      "args": [],
      "comment": "",
      "includes": {
        "caps": [
          "CAP_DAC_READ_SEARCH"
        ]
      },
      "excludes": {}
    },
    {
      "names": [
        "bpf",
        "clone",
        "fanotify_init",
        "lookup_dcookie",
        "mount",
        "name_to_handle_at",
        "perf_event_open",
        "quotactl",
        "setdomainname",
        "sethostname",
        "setns",
        "syslog",
        "umount",
        "umount2",
        "unshare"
      ],
      "action": "SCMP_ACT_ALLOW",
      "args": [],
      "comment": "",
      "includes": {
        "caps": [
          "CAP_SYS_ADMIN"
        ]
      },
      "excludes": {}
    },
    {
      "names": [
        "clone"
      ],
      "action": "SCMP_ACT_ALLOW",
      "args": [
        {
          "index": 0,
          "value": 2080505856,
          "valueTwo": 0,
          "op": "SCMP_CMP_MASKED_EQ"
        }
      ],
      "comment": "",
      "includes": {},
      "excludes": {
        "caps": [
          "CAP_SYS_ADMIN"
        ],
        "arches": [
          "s390",
          "s390x"
        ]
      }
    },
    {
      "names": [
        "clone"
      ],
      "action": "SCMP_ACT_ALLOW",
      "args": [
        {
          "index": 1,
          "value": 2080505856,
          "valueTwo": 0,
          "op": "SCMP_CMP_MASKED_EQ"
        }
      ],
      "comment": "s390 parameter ordering for clone is different",
      "includes": {
        "arches": [
          "s390",
          "s390x"
        ]
      },
      "excludes": {
        "caps": [
          "CAP_SYS_ADMIN"
        ]
      }
    },
    {
      "names": [
        "reboot"
      ],
      "action": "SCMP_ACT_ALLOW",
      "args": [],
      "comment": "",
      "includes": {
        "caps": [
          "CAP_SYS_BOOT"
        ]
      },
      "excludes": {}
    },
    {
      "names": [
        "chroot",
        "pivot_root"
      ],
      "action": "SCMP_ACT_ALLOW",
      "args": [],
      "comment": "",
      "includes": {
        "caps": [
          "CAP_SYS_CHROOT"
        ]
      },
      "excludes": {}
    },
    {
      "names": [
        "delete_module",
        "init_module",
        "finit_module",
        "query_module"
      ],
      "action": "SCMP_ACT_ALLOW",
      "args": [],
      "comment": "",
      "includes": {
        "caps": [
          "CAP_SYS_MODULE"
        ]
      },
      "excludes": {}
    },
    {
      "names": [
        "acct"
      ],
      "action": "SCMP_ACT_ALLOW",
      "args": [],
      "comment": "",
      "includes": {
        "caps": [
          "CAP_SYS_PACCT"
        ]
      },
      "excludes": {}
    },
    {
      "names": [
        "kcmp",
        "process_vm_readv",
        "process_vm_writev",
        "ptrace"
      ],
      "action": "SCMP_ACT_ALLOW",
      "args": [],
      "comment": "",
      "includes": {
        "caps": [
          "CAP_SYS_PTRACE"
        ]
      },
      "excludes": {}
    },
    {
      "names": [
        "iopl",
        "ioperm"
      ],
      "action": "SCMP_ACT_ALLOW",
      "args": [],
      "comment": "",
      "includes": {
        "caps": [
          "CAP_SYS_RAWIO"
        ]
      },
      "excludes": {}
    },
    {
      "names": [
        "settimeofday",
        "stime",
        "clock_settime"
      ],
      "action": "SCMP_ACT_ALLOW",
      "args": [],
      "comment": "",
      "includes": {
        "caps": [
          "CAP_SYS_TIME"
        ]
      },
      "excludes": {}
    },
    {
      "names": [
        "vhangup"
      ],
      "action": "SCMP_ACT_ALLOW",
      "args": [],
      "comment": "",
      "includes": {
        "caps": [
          "CAP_SYS_TTY_CONFIG"
        ]
      },
      "excludes": {}
    },
    {
      "names": [
        "get_mempolicy",
        "mbind",
        "set_mempolicy"
      ],
      "action": "SCMP_ACT_ALLOW",
      "args": [],
      "comment": "",
      "includes": {
        "caps": [
          "CAP_SYS_NICE"
        ]
      },
      "excludes": {}
    },
    {
      "names": [
        "syslog"
      ],
      "action": "SCMP_ACT_ALLOW",
      "args": [],
      "comment": "",
      "includes": {
        "caps": [
          "CAP_SYSLOG"
        ]
      },
      "excludes": {}
    }
  ]
})~";


class LinuxSeccompIsolatorTest
  : public ContainerizerTest<slave::MesosContainerizer>
{
protected:
  slave::Flags CreateSlaveFlags() override
  {
    slave::Flags flags = MesosTest::CreateSlaveFlags();

    flags.isolation = "linux/seccomp";
    flags.seccomp_config_dir = os::getcwd();

    return flags;
  }

  string createProfile(const string& config) const
  {
    CHECK(!config.empty());

    Try<string> profilePath = os::mktemp(path::join(os::getcwd(), "XXXXXX"));
    EXPECT_SOME(profilePath);

    EXPECT_SOME(os::write(profilePath.get(), config));

    return Path(profilePath.get()).basename();
  }
};


// This test verifies that the Seccomp isolator fails during initialization
// when `--seccomp_config_dir` flag is not provided.
TEST_F(LinuxSeccompIsolatorTest, ROOT_MissingConfigDir)
{
  slave::Flags flags = CreateSlaveFlags();
  flags.seccomp_config_dir = None();

  Fetcher fetcher(flags);

  Try<MesosContainerizer*> create =
    MesosContainerizer::create(flags, false, &fetcher);

  ASSERT_ERROR(create);
  EXPECT_TRUE(strings::contains(
      create.error(), "Missing required `--seccomp_config_dir` flag"));
}


// This test verifies that the Seccomp isolator fails during initialization
// when default Seccomp profile is invalid.
TEST_F(LinuxSeccompIsolatorTest, ROOT_InvalidDefaultProfile)
{
  slave::Flags flags = CreateSlaveFlags();
  flags.seccomp_profile_name = createProfile("{}");

  Fetcher fetcher(flags);

  Try<MesosContainerizer*> create =
    MesosContainerizer::create(flags, false, &fetcher);

  ASSERT_ERROR(create);
  EXPECT_TRUE(strings::contains(
      create.error(), "Failed to parse Seccomp profile"));
}


// This test verifies that we can launch shell commands when the default
// Seccomp profile is enabled.
TEST_F(LinuxSeccompIsolatorTest, ROOT_SECCOMP_LaunchWithDefaultProfile)
{
  slave::Flags flags = CreateSlaveFlags();
  flags.seccomp_profile_name = createProfile(TEST_SECCOMP_PROFILE);

  Fetcher fetcher(flags);

  Try<MesosContainerizer*> create =
    MesosContainerizer::create(flags, false, &fetcher);

  ASSERT_SOME(create);

  Owned<MesosContainerizer> containerizer(create.get());

  SlaveState state;
  state.id = SlaveID();

  AWAIT_READY(containerizer->recover(state));

  ContainerID containerId;
  containerId.set_value(id::UUID::random().toString());

  Try<string> directory = environment->mkdtemp();
  ASSERT_SOME(directory);

  const string command = "id && env && uname && hostname";

  Future<Containerizer::LaunchResult> launch = containerizer->launch(
      containerId,
      createContainerConfig(
          None(),
          createExecutorInfo("executor", command, "cpus:1"),
          directory.get()),
      map<string, string>(),
      None());

  AWAIT_ASSERT_EQ(Containerizer::LaunchResult::SUCCESS, launch);

  Future<Option<ContainerTermination>> wait = containerizer->wait(containerId);

  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());
  ASSERT_TRUE(wait.get()->has_status());
  EXPECT_WEXITSTATUS_EQ(0, wait.get()->status());
}


// This test verifies that OS kernel kills the task process on invocation of
// syscall that is disabled by Seccomp profile.
TEST_F(LinuxSeccompIsolatorTest, ROOT_SECCOMP_LaunchWithUnameDisabled)
{
  const string config =
    R"~(
    {
      "defaultAction": "SCMP_ACT_ALLOW",
      "archMap": [
        {
          "architecture": "SCMP_ARCH_X86_64",
          "subArchitectures": [
            "SCMP_ARCH_X86",
            "SCMP_ARCH_X32"
          ]
        }
      ],
      "syscalls": [
        {
          "names": ["uname"],
          "action": "SCMP_ACT_ERRNO",
          "args": [],
          "includes": {},
          "excludes": {}
        }
      ]
    })~";

  slave::Flags flags = CreateSlaveFlags();
  flags.seccomp_profile_name = createProfile(config);

  Fetcher fetcher(flags);

  Try<MesosContainerizer*> create =
    MesosContainerizer::create(flags, false, &fetcher);

  ASSERT_SOME(create);

  Owned<MesosContainerizer> containerizer(create.get());

  SlaveState state;
  state.id = SlaveID();

  AWAIT_READY(containerizer->recover(state));

  ContainerID containerId;
  containerId.set_value(id::UUID::random().toString());

  Try<string> directory = environment->mkdtemp();
  ASSERT_SOME(directory);

  Future<Containerizer::LaunchResult> launch = containerizer->launch(
      containerId,
      createContainerConfig(
          None(),
          createExecutorInfo("executor", "uname", "cpus:1"),
          directory.get()),
      map<string, string>(),
      None());

  AWAIT_ASSERT_EQ(Containerizer::LaunchResult::SUCCESS, launch);

  Future<Option<ContainerTermination>> wait = containerizer->wait(containerId);

  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());
  ASSERT_TRUE(wait.get()->has_status());
  EXPECT_WEXITSTATUS_NE(0, wait.get()->status());
}


// This test verifies that we can disable Seccomp filtering for a particular
// task.
TEST_F(LinuxSeccompIsolatorTest, ROOT_SECCOMP_LaunchWithSeccompDisabled)
{
  const string config =
    R"~(
    {
      "defaultAction": "SCMP_ACT_ALLOW",
      "archMap": [
        {
          "architecture": "SCMP_ARCH_X86_64",
          "subArchitectures": [
            "SCMP_ARCH_X86",
            "SCMP_ARCH_X32"
          ]
        }
      ],
      "syscalls": [
        {
          "names": ["uname"],
          "action": "SCMP_ACT_ERRNO",
          "args": [],
          "includes": {},
          "excludes": {}
        }
      ]
    })~";

  slave::Flags flags = CreateSlaveFlags();
  flags.seccomp_profile_name = createProfile(config);

  Fetcher fetcher(flags);

  Try<MesosContainerizer*> create =
    MesosContainerizer::create(flags, false, &fetcher);

  ASSERT_SOME(create);

  Owned<MesosContainerizer> containerizer(create.get());

  SlaveState state;
  state.id = SlaveID();

  AWAIT_READY(containerizer->recover(state));

  ContainerID containerId;
  containerId.set_value(id::UUID::random().toString());

  Try<string> directory = environment->mkdtemp();
  ASSERT_SOME(directory);

  auto containerConfig =  createContainerConfig(
      None(),
      createExecutorInfo("executor", "uname", "cpus:1"),
      directory.get());

  ContainerInfo* container = containerConfig.mutable_container_info();
  container->set_type(ContainerInfo::MESOS);

  SeccompInfo* seccomp = container->mutable_linux_info()->mutable_seccomp();
  seccomp->set_unconfined(true);

  Future<Containerizer::LaunchResult> launch = containerizer->launch(
      containerId,
      containerConfig,
      map<string, string>(),
      None());

  AWAIT_ASSERT_EQ(Containerizer::LaunchResult::SUCCESS, launch);

  Future<Option<ContainerTermination>> wait = containerizer->wait(containerId);

  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());
  ASSERT_TRUE(wait.get()->has_status());
  EXPECT_WEXITSTATUS_EQ(0, wait.get()->status());
}


// This test verifies that we can launch a task container with overridden
// Seccomp profile.
TEST_F(LinuxSeccompIsolatorTest, ROOT_SECCOMP_LaunchWithOverriddenProfile)
{
  const string config =
    R"~(
    {
      "defaultAction": "SCMP_ACT_ALLOW",
      "archMap": [
        {
          "architecture": "SCMP_ARCH_X86_64",
          "subArchitectures": [
            "SCMP_ARCH_X86",
            "SCMP_ARCH_X32"
          ]
        }
      ],
      "syscalls": [
        {
          "names": ["uname"],
          "action": "SCMP_ACT_ERRNO",
          "args": [],
          "includes": {},
          "excludes": {}
        }
      ]
    })~";

  slave::Flags flags = CreateSlaveFlags();
  flags.seccomp_profile_name = createProfile(TEST_SECCOMP_PROFILE);

  Fetcher fetcher(flags);

  Try<MesosContainerizer*> create =
    MesosContainerizer::create(flags, false, &fetcher);

  ASSERT_SOME(create);

  Owned<MesosContainerizer> containerizer(create.get());

  SlaveState state;
  state.id = SlaveID();

  AWAIT_READY(containerizer->recover(state));

  ContainerID containerId;
  containerId.set_value(id::UUID::random().toString());

  Try<string> directory = environment->mkdtemp();
  ASSERT_SOME(directory);

  auto containerConfig =  createContainerConfig(
      None(),
      createExecutorInfo("executor", "uname", "cpus:1"),
      directory.get());

  ContainerInfo* container = containerConfig.mutable_container_info();
  container->set_type(ContainerInfo::MESOS);

  // Set the Seccomp profile name for this particular task.
  SeccompInfo* seccomp = container->mutable_linux_info()->mutable_seccomp();
  seccomp->set_profile_name(createProfile(config));

  Future<Containerizer::LaunchResult> launch = containerizer->launch(
      containerId,
      containerConfig,
      map<string, string>(),
      None());

  AWAIT_ASSERT_EQ(Containerizer::LaunchResult::SUCCESS, launch);

  Future<Option<ContainerTermination>> wait = containerizer->wait(containerId);

  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());
  ASSERT_TRUE(wait.get()->has_status());
  EXPECT_WEXITSTATUS_NE(0, wait.get()->status());
}


// This test verifies that launching a task with a non-existent Seccomp profile
// leads to failure.
TEST_F(
    LinuxSeccompIsolatorTest,
    ROOT_SECCOMP_LaunchWithOverriddenNonExistentProfile)
{
  slave::Flags flags = CreateSlaveFlags();
  flags.seccomp_profile_name = createProfile(TEST_SECCOMP_PROFILE);

  Fetcher fetcher(flags);

  Try<MesosContainerizer*> create =
    MesosContainerizer::create(flags, false, &fetcher);

  ASSERT_SOME(create);

  Owned<MesosContainerizer> containerizer(create.get());

  SlaveState state;
  state.id = SlaveID();

  AWAIT_READY(containerizer->recover(state));

  ContainerID containerId;
  containerId.set_value(id::UUID::random().toString());

  Try<string> directory = environment->mkdtemp();
  ASSERT_SOME(directory);

  auto containerConfig =  createContainerConfig(
      None(),
      createExecutorInfo("executor", "exit 0", "cpus:1"),
      directory.get());

  ContainerInfo* container = containerConfig.mutable_container_info();
  container->set_type(ContainerInfo::MESOS);

  // Set a non-existent Seccomp profile for this particular task.
  SeccompInfo* seccomp = container->mutable_linux_info()->mutable_seccomp();
  seccomp->set_profile_name("absent");

  Future<Containerizer::LaunchResult> launch = containerizer->launch(
      containerId,
      containerConfig,
      map<string, string>(),
      None());

  AWAIT_FAILED(launch);
}


// Param for the tests:
//   'includes_capabilities'
//            List of capabilities that are set in the `includes` section within
//            Seccomp profile for a syscall filtering rule.
//   'excludes_capabilities'
//            List of capabilities that are set in the `excludes` section within
//            Seccomp profile for a syscall filtering rule.
//   'container_capabilities'
//            Container specified effective and bounding capabilities for the
//            container.
//   'result'
//            True if the task should finish normally.
struct SeccompTestParam
{
  enum Result
  {
    FAILURE = 0,
    SUCCESS = 1
  };

  SeccompTestParam(
      const Option<set<Capability>>& _includes_capabilities,
      const Option<set<Capability>>& _excludes_capabilities,
      const Option<set<Capability>>& _container_capabilities,
      Result _result)
    : includes_capabilities(convertToList(_includes_capabilities)),
      excludes_capabilities(convertToList(_excludes_capabilities)),
      container_capabilities(convertToInfo(_container_capabilities)),
      result(_result) {}

  static const Option<CapabilityInfo> convertToInfo(
      const Option<set<Capability>>& caps)
  {
    return caps.isSome()
      ? capabilities::convert(caps.get())
      : Option<CapabilityInfo>::none();
  }

  static const Option<list<string>> convertToList(
      const Option<set<Capability>>& caps)
  {
    if (caps.isSome()) {
      list<string> capabilities;

      foreach (const Capability& capability, caps.get()) {
        capabilities.emplace_back(
            strings::format("\"CAP_%s\"", stringify(capability)).get());
      }

      return capabilities;
    }

    return None();
  }

  const Option<list<string>> includes_capabilities;
  const Option<list<string>> excludes_capabilities;

  const Option<CapabilityInfo> container_capabilities;

  const Result result;
};


ostream& operator<<(ostream& stream, const SeccompTestParam& param)
{
  if (param.includes_capabilities.isSome()) {
    stream << "includes_capabilities='"
           << stringify(param.includes_capabilities.get()) << "', ";
  } else {
    stream << "includes_capabilities='none', ";
  }

  if (param.excludes_capabilities.isSome()) {
    stream << "excludes_capabilities='"
           << stringify(param.excludes_capabilities.get()) << "', ";
  } else {
    stream << "excludes_capabilities='none', ";
  }

  if (param.container_capabilities.isSome()) {
    stream << "container_capabilities='"
           << JSON::protobuf(param.container_capabilities.get()) << "', ";
  } else {
    stream << "container_capabilities='none', ";
  }

  switch (param.result) {
    case SeccompTestParam::FAILURE:
      stream << "result=failure'";
      break;
    case SeccompTestParam::SUCCESS:
      stream << "result=success'";
  }

  return stream;
}


class LinuxSeccompIsolatorWithCapabilitiesTest
  : public LinuxSeccompIsolatorTest,
    public ::testing::WithParamInterface<SeccompTestParam>
{
public:
  LinuxSeccompIsolatorWithCapabilitiesTest()
    : param(GetParam()) {}

protected:
  SeccompTestParam param;
};


// Parameterized test confirming the filtering of Seccomp rules by capabilities.
// This test launches a container with enabled filtering of a Seccomp rule which
// disables `uname` system call depending on container's capabilities. E.g., if
// `includes` section contains `CAP_SYS_ADMIN` and the container is launched
// with `CAP_SYS_ADMIN`, then the rule is applied, so the `uname` command fails
// as expected.
TEST_P(LinuxSeccompIsolatorWithCapabilitiesTest, ROOT_SECCOMP_LaunchWithFilter)
{
  string includesCaps = "{}";
  string excludesCaps = "{}";

  if (param.includes_capabilities.isSome()) {
    includesCaps = strings::format(
        "{\"caps\": %s}",
        stringify(param.includes_capabilities.get())).get();
  }

  if (param.excludes_capabilities.isSome()) {
    excludesCaps = strings::format(
        "{\"caps\": %s}",
        stringify(param.excludes_capabilities.get())).get();
  }

  const string config = strings::format(
      R"~(
      {
        "defaultAction": "SCMP_ACT_ALLOW",
        "archMap": [
          {
            "architecture": "SCMP_ARCH_X86_64",
            "subArchitectures": [
              "SCMP_ARCH_X86",
              "SCMP_ARCH_X32"
            ]
          }
        ],
        "syscalls": [
          {
            "names": ["uname"],
            "action": "SCMP_ACT_ERRNO",
            "args": [],
            "includes": %s,
            "excludes": %s
          }
        ]
      })~",
      includesCaps,
      excludesCaps).get();

  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "linux/capabilities,linux/seccomp";
  flags.effective_capabilities = param.container_capabilities;
  flags.bounding_capabilities = param.container_capabilities;
  flags.seccomp_profile_name = createProfile(config);

  Fetcher fetcher(flags);

  Try<MesosContainerizer*> create =
    MesosContainerizer::create(flags, false, &fetcher);

  ASSERT_SOME(create);

  Owned<MesosContainerizer> containerizer(create.get());

  SlaveState state;
  state.id = SlaveID();

  AWAIT_READY(containerizer->recover(state));

  ContainerID containerId;
  containerId.set_value(id::UUID::random().toString());

  Try<string> directory = environment->mkdtemp();
  ASSERT_SOME(directory);

  Future<Containerizer::LaunchResult> launch = containerizer->launch(
      containerId,
      createContainerConfig(
          None(),
          createExecutorInfo("executor", "uname", "cpus:1"),
          directory.get()),
      map<string, string>(),
      None());

  AWAIT_ASSERT_EQ(Containerizer::LaunchResult::SUCCESS, launch);

  Future<Option<ContainerTermination>> wait = containerizer->wait(containerId);

  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());
  ASSERT_TRUE(wait.get()->has_status());

  switch (param.result) {
    case SeccompTestParam::SUCCESS:
      EXPECT_WEXITSTATUS_EQ(0, wait.get()->status());
      break;
    case SeccompTestParam::FAILURE:
      EXPECT_WEXITSTATUS_NE(0, wait.get()->status());
      break;
  }
}


INSTANTIATE_TEST_CASE_P(
    SeccompTestParam,
    LinuxSeccompIsolatorWithCapabilitiesTest,
    ::testing::Values(
        // Seccomp filter capabilities match container capabilities.
        SeccompTestParam(
            set<Capability>({SYS_ADMIN}),
            None(),
            set<Capability>({SYS_ADMIN}),
            SeccompTestParam::FAILURE),
        SeccompTestParam(
            None(),
            set<Capability>({SYS_ADMIN}),
            set<Capability>({SYS_ADMIN}),
            SeccompTestParam::SUCCESS),

        // Seccomp filter capabilities does not match container capabilities.
        SeccompTestParam(
            set<Capability>({SYS_ADMIN}),
            None(),
            set<Capability>({CHOWN}),
            SeccompTestParam::SUCCESS),
        SeccompTestParam(
            None(),
            set<Capability>({SYS_ADMIN}),
            set<Capability>({CHOWN}),
            SeccompTestParam::FAILURE)));

} // namespace tests {
} // namespace internal {
} // namespace mesos {
