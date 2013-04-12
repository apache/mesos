#include <errno.h>
#include <signal.h>

#include <glog/logging.h>

#include <sys/types.h> // For pid_t.

#include <fstream>
#include <list>
#include <queue>
#include <set>
#include <string>
#include <vector>

#include <stout/error.hpp>
#include <stout/foreach.hpp>
#include <stout/none.hpp>
#include <stout/numify.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>

#include "linux/proc.hpp"

using std::ifstream;
using std::list;
using std::queue;
using std::set;
using std::string;
using std::vector;

namespace mesos {
namespace internal {
namespace proc {

Try<set<pid_t> > pids()
{
  set<pid_t> pids;

  foreach (const string& file, os::ls("/proc")) {
    Try<pid_t> pid = numify<pid_t>(file);

    // Ignore files that can't be numified.
    if (pid.isSome()) {
      pids.insert(pid.get());
    }
  }

  if (!pids.empty()) {
    return pids;
  } else {
    return Error("Failed to determine pids from /proc");
  }
}


Try<set<pid_t> > children(pid_t pid, bool recursive)
{
  const Try<set<pid_t> >& pids = proc::pids();
  if (pids.isError()) {
    return Error(pids.error());
  }

  // Stat all the processes.
  list<ProcessStatus> processes;
  foreach (pid_t _pid, pids.get()) {
    const Try<ProcessStatus>& process = status(_pid);
    if (process.isSome()) {
      processes.push_back(process.get());
    }
  }

  // Perform a breadth first search for descendants.
  set<pid_t> descendants;
  queue<pid_t> parents;
  parents.push(pid);

  do {
    pid_t parent = parents.front();
    parents.pop();

    // Search for children of parent.
    foreach (const ProcessStatus& process, processes) {
      if (process.ppid == parent) {
        // Have we seen this child yet?
        if (descendants.insert(process.pid).second) {
          parents.push(process.pid);
        }
      }
    }
  } while (recursive && !parents.empty());

  return descendants;
}


Try<list<CPU> > cpus()
{
  list<CPU> results;

  ifstream file("/proc/cpuinfo");

  if (!file.is_open()) {
    return Error("Failed to open /proc/cpuinfo");
  }

  // Placeholders as we parse the file.
  Option<unsigned int> id;
  Option<unsigned int> core;
  Option<unsigned int> socket;

  string line;
  while (getline(file, line)) {
    if (line.find("processor") == 0 ||
        line.find("physical id") == 0 ||
        line.find("core id") == 0) {
      // Get out and parse the value.
      vector<string> tokens = strings::tokenize(line, ": ");
      CHECK(tokens.size() >= 2) << stringify(tokens);
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
          return Error("Unexpected format of /proc/cpuinfo");
        }
        socket = value.get();
      } else if (line.find("core id") == 0) {
        if (core.isSome()) {
          return Error("Unexpected format of /proc/cpuinfo");
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
    file.close();
    return Error("Failed to read /proc/cpuinfo");
  }

  file.close();

  return results;
}


Try<SystemStatus> status()
{
  unsigned long long btime = 0;

  ifstream file("/proc/stat");

  if (!file.is_open()) {
    return Error("Failed to open /proc/stat");
  }

  string line;
  while (getline(file, line)) {
    if (line.find("btime ") == 0) {
      Try<unsigned long long> number =
        numify<unsigned long long>(line.substr(6));
      if (number.isSome()) {
        btime = number.get();
      } else {
        return Error("Failed to parse /proc/stat: " + number.error());
      }
      break;
    }
  }

  if (file.fail() && !file.eof()) {
    file.close();
    return Error("Failed to read /proc/stat");
  }

  file.close();

  return SystemStatus(btime);
}


Try<ProcessStatus> status(pid_t pid)
{
  string path = "/proc/" + stringify(pid) + "/stat";

  ifstream file(path.c_str());

  if (!file.is_open()) {
    return Error("Failed to open '" + path + "'");
  }

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

  string _; // For ignoring fields.

  // Parse all fields from stat.
  file >> _ >> comm >> state >> ppid >> pgrp >> session >> tty_nr
       >> tpgid >> flags >> minflt >> cminflt >> majflt >> cmajflt
       >> utime >> stime >> cutime >> cstime >> priority >> nice
       >> num_threads >> itrealvalue >> starttime >> vsize >> rss
       >> rsslim >> startcode >> endcode >> startstack >> kstkeip
       >> signal >> blocked >> sigcatch >> wchan >> nswap >> cnswap;

  // Check for any read/parse errors.
  if (file.fail() && !file.eof()) {
    file.close();
    return Error("Failed to read/parse '" + path + "'");
  }

  file.close();

  return ProcessStatus(pid, comm, state, ppid, pgrp, session, tty_nr,
                       tpgid, flags, minflt, cminflt, majflt, cmajflt,
                       utime, stime, cutime, cstime, priority, nice,
                       num_threads, itrealvalue, starttime, vsize, rss,
                       rsslim, startcode, endcode, startstack, kstkeip,
                       signal, blocked, sigcatch, wchan, nswap, cnswap);
}

} // namespace proc {
} // namespace internal {
} // namespace mesos {
