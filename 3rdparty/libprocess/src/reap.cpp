#include <glog/logging.h>

#include <sys/types.h>
#include <sys/wait.h>

#include <process/delay.hpp>
#include <process/future.hpp>
#include <process/id.hpp>
#include <process/once.hpp>
#include <process/owned.hpp>
#include <process/reap.hpp>

#include <stout/check.hpp>
#include <stout/foreach.hpp>
#include <stout/multihashmap.hpp>
#include <stout/none.hpp>
#include <stout/os.hpp>
#include <stout/result.hpp>
#include <stout/try.hpp>

namespace process {


// TODO(bmahler): This can be optimized to use a thread per pid, where
// each thread makes a blocking call to waitpid. This eliminates the
// unfortunate poll delay.
//
// Simple bounded linear model for computing the poll interval.
// Values were chosen such that at (50 pids, 100 ms) the CPU usage is
// less than approx. 0.5% of a single core, and at (500 pids, 1000 ms)
// less than approx. 1.0% of single core. Tested on Linux 3.10 with
// Intel Xeon E5620 and OSX 10.9 with Intel i7 4980HQ.
//
//              1000ms          _____
//                             /
//  (interval)                /
//                           /
//               100ms -----/
//                          50  500
//
//                       (# pids)
//
const size_t LOW_PID_COUNT = 50;
Duration MIN_REAP_INTERVAL() { return Milliseconds(100); }

const size_t HIGH_PID_COUNT = 500;
Duration MAX_REAP_INTERVAL() { return Seconds(1); }


class ReaperProcess : public Process<ReaperProcess>
{
public:
  ReaperProcess() : ProcessBase(ID::generate("reaper")) {}

  Future<Option<int> > reap(pid_t pid)
  {
    // Check to see if this pid exists.
    if (os::exists(pid)) {
      Owned<Promise<Option<int> > > promise(new Promise<Option<int> >());
      promises.put(pid, promise);
      return promise->future();
    } else {
      return None();
    }
  }

protected:
  virtual void initialize() { wait(); }

  void wait()
  {
    // There are two cases to consider for each pid when it terminates:
    //   1) The process is our child. In this case, we will reap the process and
    //      notify with the exit status.
    //   2) The process was not our child. In this case, it will be reaped by
    //      someone else (its parent or init, if reparented) so we cannot know
    //      the exit status and we must notify with None().
    //
    // NOTE: A child can only be reaped by us, the parent. If a child exits
    // between waitpid and the (!exists) conditional it will still exist as a
    // zombie; it will be reaped by us on the next loop.
    foreach (pid_t pid, promises.keys()) {
      int status;
      if (waitpid(pid, &status, WNOHANG) > 0) {
        // We have reaped a child.
        notify(pid, status);
      } else if (!os::exists(pid)) {
        // The process no longer exists and has been reaped by someone else.
        notify(pid, None());
      }
    }

    delay(interval(), self(), &ReaperProcess::wait); // Reap forever!
  }

  void notify(pid_t pid, Result<int> status)
  {
    foreach (const Owned<Promise<Option<int> > >& promise, promises.get(pid)) {
      if (status.isError()) {
        promise->fail(status.error());
      } else if (status.isNone()) {
        promise->set(Option<int>::none());
      } else {
        promise->set(Option<int>(status.get()));
      }
    }
    promises.remove(pid);
  }

private:
  const Duration interval()
  {
    size_t count = promises.size();

    if (count <= LOW_PID_COUNT) {
      return MIN_REAP_INTERVAL();
    } else if (count >= HIGH_PID_COUNT) {
      return MAX_REAP_INTERVAL();
    }

    // Linear interpolation between min and max reap intervals.
    double fraction =
      ((double) (count - LOW_PID_COUNT) / (HIGH_PID_COUNT - LOW_PID_COUNT));

    return (MIN_REAP_INTERVAL() +
            (MAX_REAP_INTERVAL() - MIN_REAP_INTERVAL()) * fraction);
  }

  multihashmap<pid_t, Owned<Promise<Option<int> > > > promises;
};


// Global reaper object.
static ReaperProcess* reaper = NULL;


Future<Option<int> > reap(pid_t pid)
{
  static Once* initialized = new Once();

  if (!initialized->once()) {
    reaper = new ReaperProcess();
    spawn(reaper);
    initialized->done();
  }

  CHECK_NOTNULL(reaper);

  return dispatch(reaper, &ReaperProcess::reap, pid);
}

} // namespace process {
