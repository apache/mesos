/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef __PROC_HPP__
#define __PROC_HPP__

#include <sys/types.h> // For pid_t.

#include <linux/version.h>

#include <set>
#include <string>

#include "common/try.hpp"

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,27)
#error "Expecting Linux version >= 2.6.27"
#endif

namespace mesos {
namespace internal {
namespace proc {

// Forward declarations.
struct SystemStatistics;
struct ProcessStatistics;


// Reads from /proc and returns a list of all running processes.
Try<std::set<pid_t> > pids();

// Returns the system statistics from /proc/stat.
Try<SystemStatistics> stat();

// Returns the process statistics from /proc/[pid]/stat.
Try<ProcessStatistics> stat(pid_t pid);


// Snapshot of a system (modeled after /proc/stat).
struct SystemStatistics
{
  SystemStatistics(unsigned long long _btime)
    : btime(_btime)
  {}

  const unsigned long long btime; // Boot time.
  // TODO(benh): Add more.
};


// Snapshot of a process (modeled after /proc/[pid]/stat).
struct ProcessStatistics
{
  ProcessStatistics(
      pid_t _pid,
      const std::string& _comm,
      char _state,
      pid_t _ppid,
      pid_t _pgrp,
      pid_t _session,
      int _tty_nr,
      pid_t _tpgid,
      unsigned int _flags,
      unsigned long _minflt,
      unsigned long _cminflt,
      unsigned long _majflt,
      unsigned long _cmajflt,
      unsigned long _utime,
      unsigned long _stime,
      long _cutime,
      long _cstime,
      long _priority,
      long _nice,
      long _num_threads,
      long _itrealvalue,
      unsigned long long _starttime,
      unsigned long _vsize,
      long _rss,
      unsigned long _rsslim,
      unsigned long _startcode,
      unsigned long _endcode,
      unsigned long _startstack,
      unsigned long _kstkeip,
      unsigned long _signal,
      unsigned long _blocked,
      unsigned long _sigcatch,
      unsigned long _wchan,
      unsigned long _nswap,
      unsigned long _cnswap,
      int _exit_signal, // Since Linux 2.1.22.
      int _processor, // Since Linux 2.2.8.
      unsigned int _rt_priority, // Since Linux 2.5.19.
      unsigned int _policy, // Since Linux 2.5.19.
      unsigned long long _delayacct_blkio_ticks, // Since Linux 2.6.18.
      unsigned long _guest_time, // Since Linux 2.6.24.
      unsigned int _cguest_time) // Since Linux 2.6.24.
  : pid(_pid),
    comm(_comm),
    state(_state),
    ppid(_ppid),
    pgrp(_pgrp),
    session(_session),
    tty_nr(_tty_nr),
    tpgid(_tpgid),
    flags(_flags),
    minflt(_minflt),
    cminflt(_cminflt),
    majflt(_majflt),
    cmajflt(_cmajflt),
    utime(_utime),
    stime(_stime),
    cutime(_cutime),
    cstime(_cstime),
    priority(_priority),
    nice(_nice),
    num_threads(_num_threads),
    itrealvalue(_itrealvalue),
    starttime(_starttime),
    vsize(_vsize),
    rss(_rss),
    rsslim(_rsslim),
    startcode(_startcode),
    endcode(_endcode),
    startstack(_startstack),
    kstkeip(_kstkeip),
    signal(_signal),
    blocked(_blocked),
    sigcatch(_sigcatch),
    wchan(_wchan),
    nswap(_nswap),
    cnswap(_cnswap),
    exit_signal(_exit_signal),
    processor(_processor),
    rt_priority(_rt_priority),
    policy(_policy),
    delayacct_blkio_ticks(_delayacct_blkio_ticks),
    guest_time(_guest_time),
    cguest_time(_cguest_time)
  {}

  const pid_t pid;
  const std::string comm;
  const char state;
  const int ppid;
  const int pgrp;
  const int session;
  const int tty_nr;
  const int tpgid;
  const unsigned int flags;
  const unsigned long minflt;
  const unsigned long cminflt;
  const unsigned long majflt;
  const unsigned long cmajflt;
  const unsigned long utime;
  const unsigned long stime;
  const long cutime;
  const long cstime;
  const long priority;
  const long nice;
  const long num_threads;
  const long itrealvalue;
  const unsigned long long starttime;
  const unsigned long vsize;
  const long rss;
  const unsigned long rsslim;
  const unsigned long startcode;
  const unsigned long endcode;
  const unsigned long startstack;
  const unsigned long kstkeip;
  const unsigned long signal;
  const unsigned long blocked;
  const unsigned long sigcatch;
  const unsigned long wchan;
  const unsigned long nswap;
  const unsigned long cnswap;
  const int exit_signal;
  const int processor;
  const unsigned int rt_priority;
  const unsigned int policy;
  const unsigned long long delayacct_blkio_ticks;
  const unsigned long guest_time;
  const unsigned int cguest_time;
};

} // namespace proc {
} // namespace internal {
} // namespace mesos {

#endif // __PROC_HPP__
