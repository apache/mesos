#ifndef __LAUNCHER_HPP__
#define __LAUNCHER_HPP__

#include <string>
#include <vector>

#include "nexus_types.hpp"

#include "fatal.hpp"

namespace nexus { namespace internal { namespace launcher {

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
  bool redirectIO;

public:
  ExecutorLauncher(FrameworkID _frameworkId, const string& _executorUri,
		   const string& _user, const string& _workDirectory,
		   const string& _slavePid, bool _redirectIO);

  virtual ~ExecutorLauncher();

  virtual void run();

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

  // Split a string into non-empty tokens using the given delimiter chars.
  void split(const string& str, const string& delims, vector<string>* tokens);
};

}}}

#endif // __LAUNCHER_HPP__
