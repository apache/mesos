#ifndef __ISOLATION_MODULE_HPP__
#define __ISOLATION_MODULE_HPP__

#include <string>


namespace mesos { namespace internal { namespace slave {

class Slave;
class Framework;
class Executor;


class IsolationModule {
public:
  static IsolationModule * create(const std::string &type);
  static void destroy(IsolationModule *module);

  virtual ~IsolationModule() {}

  // Called during slave initialization.
  virtual void initialize(Slave *slave) {}

  // Called by the slave to launch an executor for a given framework.
  virtual void launchExecutor(Framework* framework, Executor* executor) = 0;

  // Terminate a framework's executor, if it is still running.
  // The executor is expected to be gone after this method exits.
  virtual void killExecutor(Framework* framework, Executor* executor) = 0;

  // Update the resource limits for a given framework. This method will
  // be called only after an executor for the framework is started.
  virtual void resourcesChanged(Framework *framework, Executor* executor) {}
};

}}}

#endif /* __ISOLATION_MODULE_HPP__ */
