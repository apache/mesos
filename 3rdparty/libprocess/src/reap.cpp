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
// unfortunate 1 second reap delay.

class ReaperProcess : public Process<ReaperProcess>
{
public:
  ReaperProcess() : ProcessBase(ID::generate("reaper")) {}

  Future<Option<int> > reap(pid_t pid)
  {
    // Check to see if this pid exists.
    const Result<os::Process>& process = os::process(pid);

    if (process.isSome()) {
      // The process exists, we add it to the promises map.
      Owned<Promise<Option<int> > > promise(new Promise<Option<int> >());
      promises.put(pid, promise);
      return promise->future();
    } else if (process.isNone()) {
      return None();
    } else {
      return Failure(
          "Failed to monitor process " + stringify(pid) + ": " +
          process.error());
    }
  }

protected:
  virtual void initialize() { wait(); }

  void wait()
  {
    // There are a few cases to consider here for each pid:
    //   1) The process is our child. In this case, we will notify
    //      with the exit status once it terminates.
    //   2) The process exists but is not our child. In this case,
    //      we'll notify with None() once it no longer exists, since
    //      we cannot reap it.
    //   3) The process does not exist, notify with None() since it
    //      has likely been reaped elsewhere.

    foreach (pid_t pid, promises.keys()) {
      int status;
      if (waitpid(pid, &status, WNOHANG) > 0) {
        // Terminated child process.
        notify(pid, status);
      } else if (errno == ECHILD) {
        // The process is not our child, or does not exist. We want to
        // notify with None() once it no longer exists (reaped by
        // someone else).
        const Result<os::Process>& process = os::process(pid);

        if (process.isError()) {
          notify(pid, Error(process.error()));
        } else if (process.isNone()) {
          // The process has been reaped.
          notify(pid, None());
        }
      }
    }

    delay(Seconds(1), self(), &ReaperProcess::wait); // Reap forever!
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
