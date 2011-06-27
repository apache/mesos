#ifndef __ISOLATION_MODULE_HPP__
#define __ISOLATION_MODULE_HPP__

#include <string>

#include <mesos/mesos.hpp>

#include <process/process.hpp>

#include "configurator/configuration.hpp"

#include "common/resources.hpp"


namespace mesos { namespace internal { namespace slave {

class Slave;


class IsolationModule : public process::Process<IsolationModule>
{
public:
  static IsolationModule* create(const std::string& type);
  static void destroy(IsolationModule* module);

  virtual ~IsolationModule() {}

  // Called during slave initialization.
  virtual void initialize(const Configuration& conf,
                          bool local,
                          const process::PID<Slave>& slave) = 0;

  // Called by the slave to launch an executor for a given framework.
  virtual void launchExecutor(const FrameworkID& frameworkId,
                              const FrameworkInfo& frameworkInfo,
                              const ExecutorInfo& executorInfo,
                              const std::string& directory) = 0;

  // Terminate a framework's executor, if it is still running.
  // The executor is expected to be gone after this method exits.
  virtual void killExecutor(const FrameworkID& frameworkId,
                            const ExecutorID& executorId) = 0;

  // Update the resource limits for a given framework. This method will
  // be called only after an executor for the framework is started.
  virtual void resourcesChanged(const FrameworkID& frameworkId,
                                const ExecutorID& executorId,
                                const Resources& resources) = 0;
};

}}} // namespace mesos { namespace internal { namespace slave {

#endif // __ISOLATION_MODULE_HPP__
