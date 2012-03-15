/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <dirent.h>
#include <errno.h>
#include <libgen.h>
#include <stdlib.h>
#include <pwd.h>

#include <iostream>
#include <sstream>

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "common/fatal.hpp"
#include "common/foreach.hpp"
#include "common/utils.hpp"

#include "launcher/launcher.hpp"

using std::cout;
using std::endl;
using std::ostringstream;
using std::string;

namespace mesos {
namespace internal {
namespace launcher {

ExecutorLauncher::ExecutorLauncher(
    const FrameworkID& _frameworkId,
    const ExecutorID& _executorId,
    const string& _executorUri,
    const string& _command,
    const string& _user,
    const string& _workDirectory,
    const string& _slavePid,
    const string& _frameworksHome,
    const string& _mesosHome,
    const string& _hadoopHome,
    bool _redirectIO,
    bool _shouldSwitchUser,
    const string& _container,
    const Environment& _environment)
  : frameworkId(_frameworkId),
    executorId(_executorId),
    executorUri(_executorUri),
    command(_command),
    user(_user),
    workDirectory(_workDirectory),
    slavePid(_slavePid),
    frameworksHome(_frameworksHome),
    mesosHome(_mesosHome),
    hadoopHome(_hadoopHome),
    redirectIO(_redirectIO),
    shouldSwitchUser(_shouldSwitchUser),
    container(_container),
    environment(_environment) {}


ExecutorLauncher::~ExecutorLauncher() {}


int ExecutorLauncher::run()
{
  initializeWorkingDirectory();

  // Enter working directory
  if (chdir(workDirectory.c_str()) < 0) {
    fatalerror("chdir into framework working directory failed");
  }

  // Redirect output to files in working dir if required
  if (redirectIO) {
    if (freopen("stdout", "w", stdout) == NULL) {
      fatalerror("freopen failed");
    }
    if (freopen("stderr", "w", stderr) == NULL) {
      fatalerror("freopen failed");
    }
  }

  fetchExecutor(); // TODO(benh): fetchExecutors();

  setupEnvironment();

  if (shouldSwitchUser) {
    switchUser();
  }

  // TODO(benh): Clean up this gross special cased LXC garbage!!!!
  if (container != "") {
    // If we are running with a container than we need to fork an
    // extra time so that we can correctly cleanup the container when
    // the executor exits.
    pid_t pid;
    if ((pid = fork()) == -1) {
      fatalerror("Failed to fork to run '%s'", command.c_str());
    }

    if (pid != 0) {
      // In parent process, wait for the child to finish.
      int status;
      wait(&status);
      utils::os::system("lxc-stop -n " + container);
      return status;
    }
  }

  // Execute the command (via '/bin/sh -c command').
  execl("/bin/sh", "sh", "-c", command.c_str(), (char*) NULL);

  // If we get here, the execv call failedl
  fatalerror("Could not execute '/bin/sh -c %s'", command.c_str());
}


// Own the working directory, if necessary.
void ExecutorLauncher::initializeWorkingDirectory()
{
  // TODO(benh): Do this in the slave?
  if (shouldSwitchUser) {
    if (!utils::os::chown(user, workDirectory)) {
      fatal("Failed to change ownership of framework's working directory");
    }
  }
}


// Download the executor's binary if required and return its path.
void ExecutorLauncher::fetchExecutor()
{
  if (executorUri != "") {
    string executor = executorUri;

    // Some checks to make using the executor in shell commands safe;
    // these should be pushed into the master and reported to the user
    if (executor.find_first_of('\\') != string::npos ||
        executor.find_first_of('\'') != string::npos ||
        executor.find_first_of('\0') != string::npos) {
      fatal("Illegal characters in executor path");
    }
    // Grab the executor from HDFS if its path begins with hdfs://
    // TODO: Enforce some size limits on files we get from HDFS
    if (executor.find("hdfs://") == 0) {
      // Locate Hadoop's bin/hadoop script. If a Hadoop home was given to us by
      // the slave (from the Mesos config file), use that. Otherwise check for
      // a HADOOP_HOME environment variable. Finally, if that doesn't exist,
      // try looking for hadoop on the PATH.
      string hadoopScript;
      if (hadoopHome != "") {
        hadoopScript = hadoopHome + "/bin/hadoop";
      } else if (getenv("HADOOP_HOME") != 0) {
        hadoopScript = string(getenv("HADOOP_HOME")) + "/bin/hadoop";
      } else {
        hadoopScript = "hadoop"; // Look for hadoop on the PATH.
      }

      string localFile = string("./") + basename((char *) executor.c_str());
      ostringstream command;
      command << hadoopScript << " fs -copyToLocal '" << executor
              << "' '" << localFile << "'";
      cout << "Downloading executor from " << executor << endl;
      cout << "HDFS command: " << command.str() << endl;

      int ret = system(command.str().c_str());
      if (ret != 0)
        fatal("HDFS copyToLocal failed: return code %d", ret);
      executor = localFile;
      if (chmod(executor.c_str(), S_IRWXU | S_IRGRP | S_IXGRP |
                S_IROTH | S_IXOTH) != 0)
        fatalerror("chmod failed");
    } else if (executor.find_first_of("/") != 0) {
      // We got a non-Hadoop and non-absolute path.
      // Try prepending MESOS_HOME to it.
      if (frameworksHome != "") {
        executor = frameworksHome + "/" + executor;
        cout << "Prepended frameworksHome to executor path, making it: "
             << executor << endl;
      } else {
        if (mesosHome != "") {
          executor = mesosHome + "/frameworks/" + executor;
          cout << "Prepended MESOS_HOME/frameworks/ to relative "
               << "executor path, making it: " << executor << endl;
        } else {
          fatal("A relative path was passed for the executor, but "  \
                "neither MESOS_HOME nor MESOS_FRAMEWORKS_HOME is set."  \
                "Please either specify one of these config options "    \
                "or avoid using a relative path.");
        }
      }
    }

    // If the executor was a .tgz, untar it in the work directory. The .tgz
    // expected to contain a single directory. This directory should contain
    // a program or script called "executor" to run the executor. We chdir
    // into this directory and run the script from in there.
    if (executor.rfind(".tgz") == executor.size() - strlen(".tgz")) {
      string command = "tar xzf '" + executor + "'";
      cout << "Untarring executor: " + command << endl;
      int ret = system(command.c_str());
      if (ret != 0)
        fatal("Untar failed: return code %d", ret);
      // The .tgz should have contained a single directory; find it
      if (DIR *dir = opendir(".")) {
        bool found = false;
        string dirname = "";
        while (struct dirent *ent = readdir(dir)) {
          if (string(".") != ent->d_name && string("..") != ent->d_name) {
            struct stat info;
            if (stat(ent->d_name, &info) == 0) {
              if (S_ISDIR(info.st_mode)) {
                if (found) // Already found a directory earlier
                  fatal("Executor .tgz must contain a single directory");
                dirname = ent->d_name;
                found = true;
              }
            } else {
              fatalerror("Stat failed on %s", ent->d_name);
            }
          }
        }
        if (!found) // No directory found
          fatal("Executor .tgz must contain a single directory");
        if (chdir(dirname.c_str()) < 0)
          fatalerror("Chdir failed");
        executor = "./executor";
      } else {
        fatalerror("Failed to list work directory");
      }
    }
  }
}


// Set up environment variables for launching a framework's executor.
void ExecutorLauncher::setupEnvironment()
{
  // Set up the environment as specified in the ExecutorInfo.
  foreach (const Environment::Variable& variable, environment.variables()) {
    utils::os::setenv(variable.name(), variable.value());
  }

  // Set Mesos environment variables for slave ID, framework ID, etc.
  utils::os::setenv("MESOS_DIRECTORY", workDirectory);
  utils::os::setenv("MESOS_SLAVE_PID", slavePid);
  utils::os::setenv("MESOS_FRAMEWORK_ID", frameworkId.value());
  utils::os::setenv("MESOS_EXECUTOR_ID", executorId.value());

  // Set LIBPROCESS_PORT so that we bind to a random free port.
  utils::os::setenv("LIBPROCESS_PORT", "0");

  // Set MESOS_HOME so that Java and Python executors can find libraries
  if (mesosHome != "") {
    utils::os::setenv("MESOS_HOME", mesosHome);
  }
}


void ExecutorLauncher::switchUser()
{
  cout << "Switching user to " << user << endl;
  if (!utils::os::su(user)) {
    fatal("Failed to switch to user");
  }
}


void ExecutorLauncher::setupEnvironmentForLauncherMain()
{
  setupEnvironment();

  // Set up Mesos environment variables that launcher/main.cpp will
  // pass as arguments to an ExecutorLauncher there.
  utils::os::setenv("MESOS_FRAMEWORK_ID", frameworkId.value());
  utils::os::setenv("MESOS_EXECUTOR_URI", executorUri);
  utils::os::setenv("MESOS_COMMAND", command);
  utils::os::setenv("MESOS_USER", user);
  utils::os::setenv("MESOS_WORK_DIRECTORY", workDirectory);
  utils::os::setenv("MESOS_SLAVE_PID", slavePid);
  utils::os::setenv("MESOS_HOME", mesosHome);
  utils::os::setenv("MESOS_HADOOP_HOME", hadoopHome);
  utils::os::setenv("MESOS_REDIRECT_IO", redirectIO ? "1" : "0");
  utils::os::setenv("MESOS_SWITCH_USER", shouldSwitchUser ? "1" : "0");
  utils::os::setenv("MESOS_CONTAINER", container);
}

} // namespace launcher {
} // namespace internal {
} // namespace mesos {
