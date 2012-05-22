#include <sys/types.h> // For pid_t.

#include <fstream>
#include <set>
#include <string>

#include "common/try.hpp"
#include "common/utils.hpp"

#include "linux/proc.hpp"

using std::ifstream;
using std::set;
using std::string;

namespace mesos {
namespace internal {
namespace proc {

Try<set<pid_t> > pids()
{
  set<pid_t> pids;

  foreach (const string& path, utils::os::listdir("/proc")) {
    Try<pid_t> pid = utils::numify<pid_t>(path);

    // Ignore paths that can't be numified.
    if (pid.isSome()) {
      pids.insert(pid.get());
    }
  }

  if (!pids.empty()) {
    return pids;
  } else {
    return Try<set<pid_t> >::error("Failed to determine pids from /proc");
  }
}


Try<SystemStatistics> stat()
{
  unsigned long long btime;

  ifstream file("/proc/stat");

  if (!file.is_open()) {
    return Try<SystemStatistics>::error("Failed to open /proc/stat");
  }

  while (!file.eof()) {
    string line;
    getline(file, line);
    if (file.fail() && !file.eof()) {
      file.close();
      return Try<SystemStatistics>::error("Failed to read /proc/stat");
    } else if (line.find("btime ") == 0) {
      Try<unsigned long long> number =
        utils::numify<unsigned long long>(line.substr(6));
      if (number.isSome()) {
        btime = number.get();
      } else {
        return Try<SystemStatistics>::error(number.error());
      }
    }
  }

  file.close();

  return SystemStatistics(btime);
}


Try<ProcessStatistics> stat(pid_t pid)
{
  string path = "/proc/" + utils::stringify(pid) + "/stat";

  ifstream file(path.c_str());

  if (!file.is_open()) {
    return Try<ProcessStatistics>::error("Failed to open " + path);
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
  int exit_signal;
  int processor;
  unsigned int rt_priority;
  unsigned int policy;
  unsigned long long delayacct_blkio_ticks;
  unsigned long guest_time;
  unsigned int cguest_time;

  string _; // For ignoring fields.

  // Parse all fields from stat.
  file >> _ >> comm >> state >> ppid >> pgrp >> session >> tty_nr
       >> tpgid >> flags >> minflt >> cminflt >> majflt >> cmajflt
       >> utime >> stime >> cutime >> cstime >> priority >> nice
       >> num_threads >> itrealvalue >> starttime >> vsize >> rss
       >> rsslim >> startcode >> endcode >> startstack >> kstkeip
       >> signal >> blocked >> sigcatch >> wchan >> nswap >> cnswap
       >> exit_signal >> processor >> rt_priority >> policy
       >> delayacct_blkio_ticks >> guest_time >> cguest_time;

  // Check for any read/parse errors.
  if (file.fail() && !file.eof()) {
    file.close();
    return Try<ProcessStatistics>::error("Failed to read/parse " + path);
  }

  file.close();

  return ProcessStatistics(pid, comm, state, ppid, pgrp, session, tty_nr,
                           tpgid, flags, minflt, cminflt, majflt, cmajflt,
                           utime, stime, cutime, cstime, priority, nice,
                           num_threads, itrealvalue, starttime, vsize, rss,
                           rsslim, startcode, endcode, startstack, kstkeip,
                           signal, blocked, sigcatch, wchan, nswap, cnswap,
                           exit_signal, processor, rt_priority, policy,
                           delayacct_blkio_ticks, guest_time, cguest_time);
}

} // namespace proc {
} // namespace internal {
} // namespace mesos {
