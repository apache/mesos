#ifndef __LAUNCHER_HPP__
#define __LAUNCHER_HPP__

#include <map>
#include <string>
#include <vector>

#include <mesos.hpp>

#include "common/fatal.hpp"


namespace mesos { namespace internal { namespace launcher {

using std::map;
using std::string;
using std::vector;

// This class sets up the environment for an executor and then exec()'s it.
// It can either be used after a fork() in the slave process, or run as a
// standalone program (with the main function in launcher_main.cpp).
//
// The environment is initialized through for steps:
// 1) A work directory for the framework is created by createWorkingDirectory().
// 2) The executor is fetched off HDFS if necessary by fetchExecutor().
// 3) Environment variables are set by setupEnvironment().
// 4) We switch to the framework's user in switchUser().
//
// Isolation modules that wish to override the default behaviour can subclass
// Launcher and override some of the methods to perform extra actions.
class ExecutorLauncher {
protected:
  FrameworkID frameworkId;
  string executorUri;
  string user;
  string workDirectory; // Directory in which the framework should run
  string slavePid;
  string mesosHome;
  string hadoopHome;
  bool redirectIO;   // Whether to redirect stdout and stderr to files
  bool shouldSwitchUser; // Whether to setuid to framework's user
  map<string, string> params; // Key-value params in framework's ExecutorInfo

public:
  ExecutorLauncher(FrameworkID _frameworkId, const string& _executorUri,
                   const string& _user, const string& _workDirectory,
                   const string& _slavePid, const string& _mesosHome,
                   const string& _hadoopHome, bool _redirectIO,
                   bool _shouldSwitchUser, const map<string, string>& _params);

  virtual ~ExecutorLauncher();

  // Primary method to be called to run the user's executor.
  // Create the work directory, fetch the executor, set up the environment,
  // switch user, and exec() the user's executor.
  virtual void run();

  // Set up environment variables for exec'ing a launcher_main.cpp
  // (mesos-launcher binary) process. This is used by isolation modules that
  // cannot exec the user's executor directly, such as the LXC isolation
  // module, which must run lxc-execute and have it run the launcher.
  virtual void setupEnvironmentForLauncherMain();

protected:
  // Create the executor's working director.
  virtual void createWorkingDirectory();

  // Download the executor's binary if required and return its path.
  // This method is expected to place files in the current directory
  // (which will be the workDirectory).
  virtual string fetchExecutor();

  // Set up environment variables for launching a framework's executor.
  virtual void setupEnvironment();

  // Switch to a framework's user in preparation for exec()'ing its executor.
  virtual void switchUser();

private:
  // Set any environment variables given as env.* params in the ExecutorInfo
  void setupEnvVariablesFromParams();
};

}}}

#endif // __LAUNCHER_HPP__
