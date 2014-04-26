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
