#include <project.h>

#include <process.hpp>

#include <iostream>
#include <string>

#include "fatal.hpp"
#include "messages.hpp"


namespace mesos { namespace internal { namespace projd {

using namespace mesos;
using namespace mesos::internal;

using std::cout;
using std::cerr;
using std::endl;
using std::string;


const int LIMIT_UPDATE_INTERVAL = 5; // seconds


inline int shell(const char *fmt, ...)
{
  char *cmd;
  FILE *f;
  int ret;
  va_list args;
  va_start(args, fmt);
  if (vasprintf(&cmd, fmt, args) == -1)
    return -1;
  if ((f = popen(cmd, "w")) == NULL)
    return -1;
  ret = pclose(f);
  free(cmd);
  va_end(args);
  return ret;
}


/*
 * N.B. Some project and resource control manipulations are done
 * assuming that the project database is using the /etc/project file
 * as it's backend. In addition, 'prctl' can only be invoked for
 * projects that are currently active, so it might fail if their is a
 * race with a dying executor.
*/

inline void projadd(const string &user, const string &project)
{
  if (shell("projadd -U %s %s", user.c_str(), project.c_str()) != 0)
    fatal("projadd failed");
}


inline void projdel(const string &project)
{
  if (shell("projdel %s", project.c_str()) != 0)
    fatal("projdel failed");
}


class ProjectDaemon : public RecordProcess
{
public:
  PID parent;

  struct project proj;
  char projbuf[PROJECT_BUFSZ];

  int32_t cpuShares;
  int32_t mem;

  class LimitSetter : public RecordProcess
  {
    ProjectDaemon *daemon;

  public:
    LimitSetter(ProjectDaemon *daemon_) : daemon(daemon_) {}

    void operator () ()
    {
      int32_t prevCpuShares = 1;
      int32_t prevMem = 512 * Megabyte;
      while (true) {
        pause(LIMIT_UPDATE_INTERVAL);
        if (daemon->cpuShares != prevCpuShares) {
          prevCpuShares = daemon->cpuShares;
          set_cpu_shares(prevCpuShares);
        }
        if (daemon->mem != prevMem) {
          prevMem = daemon->mem;
          set_max_rss(prevMem);
        }
      }
    }

  private:
    void set_max_rss(int32_t rssInMb)
    {
      int64_t rssInBytes = rssInMb * 1024LL * 1024LL;
      if (shell("projmod -s -K rcap.max-rss=%lld %s",
                rssInBytes, daemon->proj.pj_name) != 0)
        fatal("set_max_rss failed");
    }

    int set_cpu_shares(int shares)
    {
      if (shell("prctl -n project.cpu-shares -r -v %d -i project %s",
                shares, daemon->proj.pj_name) != 0)
        fatal("set_cpu_shares failed");
  //     rctlblk_t *rblk_old, *rblk_new;

  //     if ((rblk_old = (rctlblk_t *) malloc(rctlblk_size())) == NULL)
  //       return -1;

  //     if ((rblk_new = (rctlblk_t *) malloc(rctlblk_size())) == NULL) {
  //       free(rblk_old);
  //       return -1;
  //     }

  //     if (getrctl(pr, "project.cpu-shares", NULL, rblk_old, RCTL_FIRST) == -1) {
  //       free(rblk_old);
  //       free(rblk_new);
  //       return -1;
  //     }

  //     bcopy(rblk_old, rblk_new, rctlblk_size());

  //     rctlblk_set_value(rblk_new, shares);

  //     if (pr_setrctl(pr, "project.cpu-shares", rblk_old, rblk_new,
  // 		   RCTL_REPLACE) != 0)
  //       return -1;

  //     free(rblk_old);
  //     free(rblk_new);

  //     return 0;
    }
  };

  void operator () ()
  {
    if (getprojbyid(getprojid(), &proj, projbuf, PROJECT_BUFSZ) == NULL)
      fatalerror("getprojbyid failed");

    cout << "projd started for " << proj.pj_name << endl;

    link(parent);
    send(parent, pack<PD2S_REGISTER_PROJD>(proj.pj_name));

    cpuShares = 1;
    mem = 512 * Megabyte;

    link(spawn(new LimitSetter(this)));

    do {
      switch (receive()) {
      case S2PD_UPDATE_RESOURCES: {
        Resources res;
	tie(res) = unpack<S2PD_UPDATE_RESOURCES>(body());
	this->cpuShares = (res.cpus > 0 ? res.cpus*10 : 1);
	this->mem = (res.mem > 0 ? res.mem : 512 * Megabyte);
	break;
      }
      case S2PD_KILL_ALL: {
	this->cpuShares = 1;
	this->mem = 512 * Megabyte;
	// Some processes might have already terminated themselves, so
	// don't worry about checking the return value.
	shell("for pid in $(pgrep -J %s | grep -v %d | grep -v $$); do kill -9 $pid; done", proj.pj_name, getpid());
	send(parent, pack<PD2S_PROJECT_READY>(proj.pj_name));
	break;
      }
      case PROCESS_EXIT:
	exit(1);
      }
    } while (true);
  }

public:
  ProjectDaemon(const PID &_parent) : parent(_parent) {}
};

}}}

int main(int argc, char **argv)
{
  PID parent;

  char *value;

  /* Get parent PID from environment. */
  value = getenv("PARENT_PID");

  if (value == NULL)
    fatal("expecting PARENT_PID in environment");


  parent = make_pid(value);

  if (!parent)
    fatal("cannot parse PARENT_PID");

  mesos::internal::projd::ProjectDaemon projd(parent);

  Process::wait(Process::spawn(&projd));

  return 0;
}
