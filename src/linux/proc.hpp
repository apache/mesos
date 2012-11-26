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

#include <list>
#include <set>
#include <string>

#include <stout/try.hpp>

namespace mesos {
namespace internal {
namespace proc {

// Forward declarations.
struct CPU;
struct SystemStatistics;
struct ProcessStatistics;


// Reads from /proc and returns a list of all running processes.
Try<std::set<pid_t> > pids();

// Reads from /proc/cpuinfo and returns a list of CPUs.
Try<std::list<CPU> > cpus();

// Returns the system statistics from /proc/stat.
Try<SystemStatistics> stat();

// Returns the process statistics from /proc/[pid]/stat.
Try<ProcessStatistics> stat(pid_t pid);


// Representation of a processor (really an execution unit since this
// captures "hardware threads" as well) modeled after /proc/cpuinfo.
struct CPU
{
  CPU(unsigned int _id, unsigned int _core, unsigned int _socket)
    : id(_id), core(_core), socket(_socket) {}

  bool operator == (const CPU& that) const
  {
    return (id == that.id) && (core == that.core) && (socket == that.socket);
  }

  bool operator < (const CPU& that) const
  {
    // Sort by (socket, core, id).
    if (socket != that.socket) {
      return socket < that.socket;
    }

    // On the same socket.
    if (core != that.core) {
      return core < that.core;
    }

    // On the same core.
    return id < that.id;
  }

  // These are non-const because we need the default assignment operator.
  unsigned int id; // "processor"
  unsigned int core; // "core id"
  unsigned int socket; // "physical id"
};


inline std::ostream& operator << (std::ostream& out, const CPU& cpu)
{
  return out << "CPU (id:" << cpu.id << ", "
             << "core:" << cpu.core << ", "
             << "socket:" << cpu.socket << ")";
}


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
// For more information, see:
// http://www.kernel.org/doc/Documentation/filesystems/proc.txt
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
      unsigned long _cnswap)
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
    cnswap(_cnswap)
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
};

} // namespace proc {
} // namespace internal {
} // namespace mesos {

#endif // __PROC_HPP__
