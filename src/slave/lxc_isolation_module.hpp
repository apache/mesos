#ifndef __LXC_ISOLATION_MODULE_HPP__
#define __LXC_ISOLATION_MODULE_HPP__

#include <string>

#include "isolation_module.hpp"
#include "reaper.hpp"
#include "slave.hpp"

#include "common/hashmap.hpp"


namespace mesos { namespace internal { namespace slave {

class LxcIsolationModule
  : public IsolationModule, public ProcessExitedListener
{
public:
  LxcIsolationModule();

  virtual ~LxcIsolationModule();

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

private:
  // No copying, no assigning.
  LxcIsolationModule(const LxcIsolationModule&);
  LxcIsolationModule& operator = (const LxcIsolationModule&);

  // Per-framework information object maintained in info hashmap.
  struct ContainerInfo
  {
    FrameworkID frameworkId;
    ExecutorID executorId;
    std::string container; // Name of Linux container used for this framework.
    pid_t pid; // PID of lxc-execute command running the executor.
  };

  // TODO(benh): Make variables const by passing them via constructor.
  Configuration conf;
  bool local;
  process::PID<Slave> slave;
  bool initialized;
  Reaper* reaper;
  hashmap<FrameworkID, hashmap<ExecutorID, ContainerInfo*> > infos;
};

}}} // namespace mesos { namespace internal { namespace slave {

#endif // __LXC_ISOLATION_MODULE_HPP__
