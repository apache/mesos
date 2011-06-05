#ifndef __PROCESS_BASED_ISOLATION_MODULE_HPP__
#define __PROCESS_BASED_ISOLATION_MODULE_HPP__

#include <sys/types.h>

#include <boost/unordered_map.hpp>

#include "isolation_module.hpp"
#include "slave.hpp"

#include "launcher/launcher.hpp"

#include "messaging/messages.hpp"


namespace mesos { namespace internal { namespace slave {

class ProcessBasedIsolationModule : public IsolationModule {
public:
  ProcessBasedIsolationModule();

  virtual ~ProcessBasedIsolationModule();

  virtual void initialize(Slave *slave);

  virtual void launchExecutor(Framework* framework, Executor* executor);

  virtual void killExecutor(Framework* framework, Executor* executor);

  virtual void resourcesChanged(Framework* framework, Executor* executor);

  // Reaps child processes and tells the slave if they exit
  class Reaper : public Process {
    ProcessBasedIsolationModule* module;

  protected:
    void operator () ();

  public:
    Reaper(ProcessBasedIsolationModule* module);
  };

  // Extra shutdown message for reaper
  enum { SHUTDOWN_REAPER = PROCESS_MSGID };

protected:
  // Main method executed after a fork() to create a Launcher for launching
  // an executor's process. The Launcher will create the child's working
  // directory, chdir() to it, fetch the executor, set environment varibles,
  // switch user, etc, and finally exec() the executor process.
  // Subclasses of ProcessBasedIsolationModule that wish to override the
  // default launching behavior should override createLauncher() and return
  // their own Launcher object (including possibly a subclass of Launcher).
  virtual launcher::ExecutorLauncher* createExecutorLauncher(Framework* framework, Executor* executor);

private:
  bool initialized;
  Slave* slave;
  boost::unordered_map<FrameworkID, boost::unordered_map<ExecutorID, pid_t> > pgids;
  Reaper* reaper;
};

}}}

#endif /* __PROCESS_BASED_ISOLATION_MODULE_HPP__ */
