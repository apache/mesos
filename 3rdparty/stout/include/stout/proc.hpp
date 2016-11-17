// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//  http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef __STOUT_PROC_HPP__
#define __STOUT_PROC_HPP__

// This file contains linux-only utilities for /proc.
#ifndef __linux__
#error "stout/proc.hpp is only available on Linux systems."
#endif

#include <errno.h>
#include <signal.h>

#include <sys/types.h> // For pid_t.

#include <fstream>
#include <list>
#include <queue>
#include <set>
#include <sstream> // For 'std::istringstream'.
#include <string>
#include <vector>

#include <stout/error.hpp>
#include <stout/foreach.hpp>
#include <stout/none.hpp>
#include <stout/numify.hpp>
#include <stout/option.hpp>
#include <stout/path.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>

#include <stout/os/exists.hpp>
#include <stout/os/ls.hpp>
#include <stout/os/read.hpp>

namespace proc {

// Snapshot of a process (modeled after /proc/[pid]/stat).
// For more information, see:
// http://www.kernel.org/doc/Documentation/filesystems/proc.txt
struct ProcessStatus
{
  ProcessStatus(
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
    cnswap(_cnswap) {}

  const pid_t pid;
  const std::string comm;
  const char state;
  const pid_t ppid;
  const pid_t pgrp;
  const pid_t session;
  const int tty_nr;
  const pid_t tpgid;
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


// Returns the process statistics from /proc/[pid]/stat.
// The return value is None if the process does not exist.
inline Result<ProcessStatus> status(pid_t pid)
{
  std::string path = "/proc/" + stringify(pid) + "/stat";

  Try<std::string> read = os::read(path);
  if (read.isError()) {
    // Need to check if file exists AFTER we open it to guarantee
    // process hasn't terminated.
    if (!os::exists(path)) {
      return None();
    }
    return Error(read.error());
  }

  std::istringstream data(read.get());

  std::string comm;
  char state;
  pid_t ppid;
  pid_t pgrp;
  pid_t session;
  int tty_nr;
  pid_t tpgid;
  unsigned int flags;
  unsigned long minflt;
  unsigned long cminflt;
  unsigned long majflt;
  unsigned long cmajflt;
  unsigned long utime;
  unsigned long stime;
  long cutime;
  long cstime;
  long priority;
  long nice;
  long num_threads;
  long itrealvalue;
  unsigned long long starttime;
  unsigned long vsize;
  long rss;
  unsigned long rsslim;
  unsigned long startcode;
  unsigned long endcode;
  unsigned long startstack;
  unsigned long kstkeip;
  unsigned long signal;
  unsigned long blocked;
  unsigned long sigcatch;
  unsigned long wchan;
  unsigned long nswap;
  unsigned long cnswap;

  // NOTE: The following are unused for now.
  // int exit_signal;
  // int processor;
  // unsigned int rt_priority;
  // unsigned int policy;
  // unsigned long long delayacct_blkio_ticks;
  // unsigned long guest_time;
  // unsigned int cguest_time;

  std::string _; // For ignoring fields.

  // Parse all fields from stat.
  data >> _ >> comm >> state >> ppid >> pgrp >> session >> tty_nr
       >> tpgid >> flags >> minflt >> cminflt >> majflt >> cmajflt
       >> utime >> stime >> cutime >> cstime >> priority >> nice
       >> num_threads >> itrealvalue >> starttime >> vsize >> rss
       >> rsslim >> startcode >> endcode >> startstack >> kstkeip
       >> signal >> blocked >> sigcatch >> wchan >> nswap >> cnswap;

  // Check for any read/parse errors.
  if (data.fail() && !data.eof()) {
    return Error("Failed to read/parse '" + path + "'");
  }

  // Remove the parentheses that is wrapped around 'comm' (when
  // printing out the process in a process tree we use parentheses to
  // indicate "zombie" processes).
  comm = strings::remove(comm, "(", strings::PREFIX);
  comm = strings::remove(comm, ")", strings::SUFFIX);

  return ProcessStatus(pid, comm, state, ppid, pgrp, session, tty_nr,
                       tpgid, flags, minflt, cminflt, majflt, cmajflt,
                       utime, stime, cutime, cstime, priority, nice,
                       num_threads, itrealvalue, starttime, vsize, rss,
                       rsslim, startcode, endcode, startstack, kstkeip,
                       signal, blocked, sigcatch, wchan, nswap, cnswap);
}


inline Result<std::string> cmdline(const Option<pid_t>& pid = None())
{
  const std::string path = pid.isSome()
    ? "/proc/" + stringify(pid.get()) + "/cmdline"
    : "/proc/cmdline";

  std::ifstream file(path.c_str());

  if (!file.is_open()) {
    // Need to check if file exists AFTER we open it to guarantee
    // process hasn't terminated (or if it has, we at least have a
    // file which the kernel _should_ respect until a close).
    if (!os::exists(path)) {
      return None();
    }
    return Error("Failed to open '" + path + "'");
  }

  std::stringbuf buffer;

  do {
    // Read each argument in "argv", separated by null bytes.
    file.get(buffer, '\0');

    // Check for any read errors.
    if (file.fail() && !file.eof()) {
      return Error("Failed to read '" + path + "'");
    } else if (!file.eof()) {
      file.get(); // Read the null byte.
      buffer.sputc(' '); // Put a space between each command line argument.
    }
  } while (!file.eof());

  return buffer.str();
}


// Reads from /proc and returns a list of all running processes.
inline Try<std::set<pid_t>> pids()
{
  std::set<pid_t> pids;

  Try<std::list<std::string>> entries = os::ls("/proc");
  if (entries.isError()) {
    return Error("Failed to list files in /proc: " + entries.error());
  }

  foreach (const std::string& entry, entries.get()) {
    Try<pid_t> pid = numify<pid_t>(entry);
    if (pid.isSome()) {
      pids.insert(pid.get()); // Ignore entries that can't be numified.
    }
  }

  if (!pids.empty()) {
    return pids;
  }

  return Error("Failed to determine pids from /proc");
}


// Reads from /proc/<pid>/task/* and returns a list of threads ids for pid.
inline Try<std::set<pid_t>> threads(pid_t pid)
{
  const std::string path = path::join("/proc", stringify(pid), "task");

  std::set<pid_t> threads;

  Try<std::list<std::string>> entries = os::ls(path);
  if (entries.isError()) {
    return Error("Failed to list files in " + path + ": " + entries.error());
  }

  foreach (const std::string& entry, entries.get()) {
    Try<pid_t> thread = numify<pid_t>(entry);
    if (thread.isSome()) {
      threads.insert(thread.get());
    }
  }

  if (!threads.empty()) {
    return threads;
  }

  return Error("Failed to determine thread ids from /proc");
}


// Snapshot of a system (modeled after /proc/stat).
struct SystemStatus
{
  SystemStatus(unsigned long long _btime) : btime(_btime) {}

  const unsigned long long btime; // Boot time.
  // TODO(benh): Add more.
};


// Returns the system statistics from /proc/stat.
inline Try<SystemStatus> status()
{
  unsigned long long btime = 0;

  std::ifstream file("/proc/stat");

  if (!file.is_open()) {
    return Error("Failed to open /proc/stat");
  }

  std::string line;
  while (std::getline(file, line)) {
    if (line.find("btime ") == 0) {
      Try<unsigned long long> number =
        numify<unsigned long long>(line.substr(6));

      if (number.isError()) {
        return Error("Failed to parse /proc/stat: " + number.error());
      }

      btime = number.get();
      break;
    }
  }

  if (file.fail() && !file.eof()) {
    return Error("Failed to read /proc/stat");
  }

  return SystemStatus(btime);
}


// Representation of a processor (really an execution unit since this
// captures "hardware threads" as well) modeled after /proc/cpuinfo.
struct CPU
{
  CPU(unsigned int _id, unsigned int _core, unsigned int _socket)
    : id(_id), core(_core), socket(_socket) {}

  // These are non-const because we need the default assignment operator.
  unsigned int id; // "processor"
  unsigned int core; // "core id"
  unsigned int socket; // "physical id"
};


inline bool operator==(const CPU& lhs, const CPU& rhs)
{
  return (lhs.id == rhs.id) && (lhs.core == rhs.core) &&
    (lhs.socket == rhs.socket);
}


inline bool operator<(const CPU& lhs, const CPU& rhs)
{
  // Sort by (socket, core, id).
  if (lhs.socket != rhs.socket) {
    return lhs.socket < rhs.socket;
  }

  // On the same socket.
  if (lhs.core != rhs.core) {
    return lhs.core < rhs.core;
  }

  // On the same core.
  return lhs.id < rhs.id;
}


inline std::ostream& operator<<(std::ostream& stream, const CPU& cpu)
{
  return stream << "CPU (id:" << cpu.id << ", "
                << "core:" << cpu.core << ", "
                << "socket:" << cpu.socket << ")";
}


// Reads from /proc/cpuinfo and returns a list of CPUs.
inline Try<std::list<CPU>> cpus()
{
  std::list<CPU> results;

  std::ifstream file("/proc/cpuinfo");

  if (!file.is_open()) {
    return Error("Failed to open /proc/cpuinfo");
  }

  // Placeholders as we parse the file.
  Option<unsigned int> id;
  Option<unsigned int> core;
  Option<unsigned int> socket;

  std::string line;
  while (std::getline(file, line)) {
    if (line.find("processor") == 0 ||
        line.find("physical id") == 0 ||
        line.find("core id") == 0) {
      // Get out and parse the value.
      std::vector<std::string> tokens = strings::tokenize(line, ": ");

      if (tokens.size() < 2) {
        return Error("Unexpected format in /proc/cpuinfo: " +
                     stringify(tokens));
      }

      Try<unsigned int> value = numify<unsigned int>(tokens.back());
      if (value.isError()) {
        return Error(value.error());
      }

      // Now save the value.
      if (line.find("processor") == 0) {
        if (id.isSome()) {
          // The physical id and core id are not present in this case.
          results.push_back(CPU(id.get(), 0, 0));
        }
        id = value.get();
      } else if (line.find("physical id") == 0) {
        if (socket.isSome()) {
          return Error("Unexpected format in /proc/cpuinfo");
        }
        socket = value.get();
      } else if (line.find("core id") == 0) {
        if (core.isSome()) {
          return Error("Unexpected format in /proc/cpuinfo");
        }
        core = value.get();
      }

      // And finally create a CPU if we have all the information.
      if (id.isSome() && core.isSome() && socket.isSome()) {
        results.push_back(CPU(id.get(), core.get(), socket.get()));
        id = None();
        core = None();
        socket = None();
      }
    }
  }

  // Add the last processor if the physical id and core id were not present.
  if (id.isSome()) {
    // The physical id and core id are not present.
    results.push_back(CPU(id.get(), 0, 0));
  }

  if (file.fail() && !file.eof()) {
    return Error("Failed to read /proc/cpuinfo");
  }

  return results;
}

} // namespace proc {

#endif // __STOUT_PROC_HPP__
