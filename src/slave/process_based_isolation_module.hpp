#ifndef __PROCESS_BASED_ISOLATION_MODULE_HPP__
#define __PROCESS_BASED_ISOLATION_MODULE_HPP__

#include <string>

#include <sys/types.h>

#include "isolation_module.hpp"
#include "reaper.hpp"
#include "slave.hpp"

#include "common/hashmap.hpp"

#include "launcher/launcher.hpp"


namespace mesos { namespace internal { namespace slave {

class ProcessBasedIsolationModule
  : public IsolationModule, public ProcessExitedListener
{
public:
  ProcessBasedIsolationModule();

  virtual ~ProcessBasedIsolationModule();

  virtual void initialize(const Configuration& conf,
                          bool local,
                          const process::PID<Slave>& slave);

  virtual void launchExecutor(const FrameworkID& frameworkId,
                              const FrameworkInfo& frameworkInfo,
                              const ExecutorInfo& executorInfo,
                              const std::string& directory);

  virtual void killExecutor(const FrameworkID& frameworkId,
                            const ExecutorID& executorId);

  virtual void resourcesChanged(const FrameworkID& frameworkId,
                                const ExecutorID& executorId,
                                const Resources& resources);

  virtual void processExited(pid_t pid, int status);

protected:
  // Main method executed after a fork() to create a Launcher for launching
  // an executor's process. The Launcher will create the child's working
  // directory, chdir() to it, fetch the executor, set environment varibles,
  // switch user, etc, and finally exec() the executor process.
  // Subclasses of ProcessBasedIsolationModule that wish to override the
  // default launching behavior should override createLauncher() and return
  // their own Launcher object (including possibly a subclass of Launcher).
  virtual launcher::ExecutorLauncher* createExecutorLauncher(
      const FrameworkID& frameworkId,
      const FrameworkInfo& frameworkInfo,
      const ExecutorInfo& executorInfo,
      const std::string& directory);

private:
  // No copying, no assigning.
  ProcessBasedIsolationModule(const ProcessBasedIsolationModule&);
  ProcessBasedIsolationModule& operator = (const ProcessBasedIsolationModule&);

  // TODO(benh): Make variables const by passing them via constructor.
  Configuration conf;
  bool local;
  process::PID<Slave> slave;
  bool initialized;
  Reaper* reaper;
  hashmap<FrameworkID, hashmap<ExecutorID, pid_t> > pgids;
};

}}}

#endif /* __PROCESS_BASED_ISOLATION_MODULE_HPP__ */
