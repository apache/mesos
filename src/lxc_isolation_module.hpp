#ifndef __LXC_ISOLATION_MODULE_HPP__
#define __LXC_ISOLATION_MODULE_HPP__

#include <string>

#include <boost/unordered_map.hpp>

#include "isolation_module.hpp"

namespace nexus { namespace internal { namespace slave {

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
  enum { SHUTDOWN_REAPER = NEXUS_MESSAGES };

protected:
  Slave* slave;
  unordered_map<FrameworkID, string> container;
  unordered_map<FrameworkID, pid_t> lxcExecutePid;
  Reaper* reaper;

public:
  LxcIsolationModule(Slave* slave);

  virtual ~LxcIsolationModule();

  virtual void frameworkAdded(Framework* framework);

  virtual void frameworkRemoved(Framework* framework);

  virtual void startExecutor(Framework* framework);

  virtual void killExecutor(Framework* framework);

  virtual void resourcesChanged(Framework* framework);

protected:
  int shell(const char* format, ...);
};

}}}

#endif /* __LXC_ISOLATION_MODULE_HPP__ */
