#ifndef __PROCESS_BASED_ISOLATION_MODULE_HPP__
#define __PROCESS_BASED_ISOLATION_MODULE_HPP__

#include <string>

#include <sys/types.h>

#include <boost/unordered_map.hpp>

#include "launcher.hpp"
#include "isolation_module.hpp"

namespace nexus { namespace internal { namespace slave {

using std::string;
using boost::unordered_map;
using nexus::internal::launcher::ExecutorLauncher;

class ProcessBasedIsolationModule : public IsolationModule {
public:
  // Reaps child processes and tells the slave if they exit
  class Reaper : public Tuple<Process> {
    ProcessBasedIsolationModule* module;

  protected:
    void operator () ();

  public:
    Reaper(ProcessBasedIsolationModule* module);
  };

  // Extra shutdown message for reaper
  enum { SHUTDOWN_REAPER = NEXUS_MESSAGES };

protected:
  Slave* slave;
  unordered_map<FrameworkID, pid_t> pgids;
  Reaper* reaper;

public:
  ProcessBasedIsolationModule(Slave* slave);

  virtual ~ProcessBasedIsolationModule();

  virtual void frameworkAdded(Framework* framework);

  virtual void frameworkRemoved(Framework* framework);

  virtual void startExecutor(Framework *framework);

  virtual void killExecutor(Framework* framework);

  virtual void resourcesChanged(Framework* framework);

protected:
  // Main method executed after a fork() to create a Launcher for launching
  // an executor's process. The Launcher will create the child's working
  // directory, chdir() to it, fetch the executor, set environment varibles,
  // switch user, etc, and finally exec() the executor process.
  // Subclasses of ProcessBasedIsolationModule that wish to override the
  // default launching behavior should override createLauncher() and return
  // their own Launcher object (including possibly a subclass of Launcher).
  virtual ExecutorLauncher* createExecutorLauncher(Framework* framework);
};

}}}

#endif /* __PROCESS_BASED_ISOLATION_MODULE_HPP__ */
