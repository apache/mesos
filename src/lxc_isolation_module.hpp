#ifndef __LXC_ISOLATION_MODULE_HPP__
#define __LXC_ISOLATION_MODULE_HPP__

#include <string>

#include <boost/unordered_map.hpp>

#include "isolation_module.hpp"
#include "messages.hpp"
#include "slave.hpp"

namespace mesos { namespace internal { namespace slave {

using std::string;
using boost::unordered_map;

class LxcIsolationModule : public IsolationModule {
public:
  // Reaps framework containers and tells the slave if they exit
  class Reaper : public Tuple<Process> {
    LxcIsolationModule* module;

  protected:
    void operator () ();

  public:
    Reaper(LxcIsolationModule* module);
  };

  // Extra shutdown message for reaper
  enum { SHUTDOWN_REAPER = MESOS_MESSAGES };

  // Per-framework information object maintained in info hashmap
  struct FrameworkInfo {
    string container;    // Name of Linux container used for this framework
    pid_t lxcExecutePid; // PID of lxc-execute command running the executor
  };

protected:
  bool initialized;
  Slave* slave;
  unordered_map<FrameworkID, FrameworkInfo*> infos;
  Reaper* reaper;

public:
  LxcIsolationModule();

  virtual ~LxcIsolationModule();

  virtual void initialize(Slave* slave);

  virtual void frameworkAdded(Framework* framework);

  virtual void frameworkRemoved(Framework* framework);

  virtual void startExecutor(Framework* framework);

  virtual void killExecutor(Framework* framework);

  virtual void resourcesChanged(Framework* framework);

protected:
  // Run a shell command formatted with varargs and return its exit code.
  int shell(const char* format, ...);

  // Attempt to set a resource limit of a framework's container for a given
  // cgroup property (e.g. cpu.shares). Returns true on success.
  bool setResourceLimit(Framework* fw, const string& property, int64_t value);
};

}}}

#endif /* __LXC_ISOLATION_MODULE_HPP__ */
