#ifndef __SOLARIS_PROJECT_ISOLATION_MODULE_HPP__
#define __SOLARIS_PROJECT_ISOLATION_MODULE_HPP__

#include <string>
#include <queue>

#include <boost/unordered_map.hpp>

#include "process_based_isolation_module.hpp"

namespace nexus { namespace internal { namespace slave {

using std::string;
using std::queue;

using boost::unordered_map;

using nexus::internal::launcher::ExecutorLauncher;

class SolarisProjectIsolationModule : public ProcessBasedIsolationModule {
protected:
  class Communicator : public RecordProcess {
    friend class SolarisProjectIsolationModule;

    bool shouldRun;
    SolarisProjectIsolationModule* module;

  public:
    Communicator(SolarisProjectIsolationModule* module);
    void operator() ();
    void stop();

  private:
    void launchProjds();
    void launchProjd(const string &project);
  };

  class ProjectLauncher : public ExecutorLauncher {
    string project;

  public:
    ProjectLauncher(FrameworkID _frameworkId, const string& _executorPath,
		    const string& _user, const string& _workDirectory,
		    const string& _slavePid, bool _redirectIO,
		    const string& project);

    virtual void switchUser();
  };

  queue<string> projects;
  unordered_map<string, PID> projds;
  unordered_map<FrameworkID, string> frameworkProject;
  Communicator* comm;

public:
  SolarisProjectIsolationModule();

  virtual ~SolarisProjectIsolationModule();

  virtual void initialize(Slave* slave);

  virtual void startExecutor(Framework* framework);

  virtual void killExecutor(Framework* framework);

  virtual void resourcesChanged(Framework* framework);

protected:
  virtual ExecutorLauncher* createExecutorLauncher(Framework* framework);
};

}}}

#endif /* __SOLARIS_PROJECT_ISOLATION_MODULE_HPP__ */
