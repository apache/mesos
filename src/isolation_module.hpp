#ifndef __ISOLATION_MODULE_HPP__
#define __ISOLATION_MODULE_HPP__

#include "slave.hpp"

namespace nexus { namespace internal { namespace slave {

class IsolationModule {
public:
  virtual ~IsolationModule() {}

  // Called when a new framework is launched on the slave.
  virtual void frameworkAdded(Framework *framework) {}

  // Called when a framework is removed.
  virtual void frameworkRemoved(Framework *framework) {}

  // Called by the slave to launch an executor for a given framework,
  // only if no executor was started or killExecutor was used.
  virtual void startExecutor(Framework *framework) = 0;

  // Terminate a framework's executor, if it is still running.
  // The executor is expected to be gone after this method exits,
  // and startExecutor may be called later to launch a new one.
  virtual void killExecutor(Framework *framework) = 0;

  // Update the resource limits for a given framework. This method will
  // be called only after an executor for the framework is started.
  virtual void resourcesChanged(Framework *framework) = 0;
};

}}}

#endif /* __ISOLATION_MODULE_HPP__ */
