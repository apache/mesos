#ifndef __LXC_ISOLATION_MODULE_HPP__
#define __LXC_ISOLATION_MODULE_HPP__

#include <string>

#include <boost/unordered_map.hpp>

#include "isolation_module.hpp"
#include "slave.hpp"

#include "messaging/messages.hpp"


namespace mesos { namespace internal { namespace slave {

using std::string;
using boost::unordered_map;

class LxcIsolationModule : public IsolationModule {
public:
  // Reaps framework containers and tells the slave if they exit
  class Reaper : public Process {
    LxcIsolationModule* module;

  protected:
    void operator () ();

  public:
    Reaper(LxcIsolationModule* module);
  };

  // Extra shutdown message for reaper
  enum { SHUTDOWN_REAPER = PROCESS_MSGID };

  // Per-framework information object maintained in info hashmap
  struct FrameworkInfo {
    string container;    // Name of Linux container used for this framework
    pid_t lxcExecutePid; // PID of lxc-execute command running the executor
  };

private:
  bool initialized;
  Slave* slave;
  unordered_map<FrameworkID, FrameworkInfo*> infos;
  Reaper* reaper;

public:
  LxcIsolationModule();

  virtual ~LxcIsolationModule();

  virtual void initialize(Slave* slave);

  virtual void launchExecutor(Framework* framework, Executor* executor);

  virtual void killExecutor(Framework* framework, Executor* executor);

  virtual void resourcesChanged(Framework* framework, Executor* executor);

protected:
  // Run a shell command formatted with varargs and return its exit code.
  int shell(const char* format, ...);

  // Attempt to set a resource limit of a framework's container for a given
  // cgroup property (e.g. cpu.shares). Returns true on success.
  bool setResourceLimit(Framework* fw, const string& property, int64_t value);
};

}}}

#endif /* __LXC_ISOLATION_MODULE_HPP__ */
