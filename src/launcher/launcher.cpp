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

#include <stout/fatal.hpp>
#include <stout/foreach.hpp>
#include <stout/net.hpp>
#include <stout/os.hpp>

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
    const CommandInfo& _commandInfo,
    const string& _user,
    const string& _workDirectory,
    const string& _slavePid,
    const string& _frameworksHome,
    const string& _hadoopHome,
    bool _redirectIO,
    bool _shouldSwitchUser,
    const string& _container)
  : frameworkId(_frameworkId),
    executorId(_executorId),
    commandInfo(_commandInfo),
    user(_user),
    workDirectory(_workDirectory),
    slavePid(_slavePid),
    frameworksHome(_frameworksHome),
    hadoopHome(_hadoopHome),
    redirectIO(_redirectIO),
    shouldSwitchUser(_shouldSwitchUser),
    container(_container) {}


ExecutorLauncher::~ExecutorLauncher() {}


int ExecutorLauncher::run()
{
  initializeWorkingDirectory();

  // Enter working directory
  if (chdir(workDirectory.c_str()) < 0) {
    fatalerror("chdir into framework working directory failed");
  }

  if (shouldSwitchUser) {
    switchUser();
  }

  // Redirect output to files in working dir if required.
  if (redirectIO) {
    if (freopen("stdout", "w", stdout) == NULL) {
      fatalerror("freopen failed");
    }
    if (freopen("stderr", "w", stderr) == NULL) {
      fatalerror("freopen failed");
    }
  }

  fetchExecutors();

  setupEnvironment();

  const string& command = commandInfo.value();

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
      os::system("lxc-stop -n " + container);
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
    if (!os::chown(user, workDirectory)) {
      fatal("Failed to change ownership of framework's working directory %s "
            "to user %s", workDirectory.c_str(), user.c_str());
    }
  }
}


// Download the executor's files and optionally set executable permissions
// if requested.
void ExecutorLauncher::fetchExecutors()
{
  foreach(const CommandInfo::URI& uri, commandInfo.uris()) {
    string resource = uri.value();
    bool executable = uri.has_executable() && uri.executable();

    // Some checks to make sure using the URI value in shell commands
    // is safe. TODO(benh): These should be pushed into the scheduler
    // driver and reported to the user.
    if (resource.find_first_of('\\') != string::npos ||
        resource.find_first_of('\'') != string::npos ||
        resource.find_first_of('\0') != string::npos) {
      fatal("Illegal characters in URI");
    }

    // Grab the resource from HDFS if its path begins with hdfs:// or
    // htfp://. TODO(matei): Enforce some size limits on files we get
    // from HDFS
    if (resource.find("hdfs://") == 0 || resource.find("hftp://") == 0) {
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

      string localFile = string("./") + basename((char *) resource.c_str());
      ostringstream command;
      command << hadoopScript << " fs -copyToLocal '" << resource
              << "' '" << localFile << "'";
      cout << "Downloading resource from " << resource << endl;
      cout << "HDFS command: " << command.str() << endl;

      int ret = os::system(command.str());
      if (ret != 0) {
        fatal("HDFS copyToLocal failed: return code %d", ret);
      }
      resource = localFile;
    } else if (resource.find("http://") == 0
               || resource.find("https://") == 0
               || resource.find("ftp://") == 0
               || resource.find("ftps://") == 0) {
      string path = resource.substr(resource.find("://") + 3);
      CHECK(path.find("/") != string::npos) << "Malformed URL (missing path)";
      CHECK(path.size() > path.find("/") + 1) << "Malformed URL (missing path)";
      path =  "./" + path.substr(path.find_last_of("/") + 1);
      cout << "Downloading " << resource << " to " << path << endl;
      Try<int> code = net::download(resource, path);
      if (code.isError()) {
        fatal("Error downloading resource: %s", code.error().c_str());
      } else if (code.get() != 200) {
        fatal("Error downloading resource, received HTTP/FTP return code: %d",
              code.get());
      }
      resource = path;
    } else { // Copy the local resource.
      if (resource.find_first_of("/") != 0) {
        // We got a non-Hadoop and non-absolute path.
        if (frameworksHome != "") {
          resource = frameworksHome + "/" + resource;
          cout << "Prepended configuration option frameworks_home to resource "
            << "path, making it: " << resource << endl;
        } else {
          fatal("A relative path was passed for the resource, but "
            "the configuration option frameworks_home is not set. "
            "Please either specify this config option "
            "or avoid using a relative path.");
        }
      }

      // Copy the resource to the current working directory.
      ostringstream command;
      command << "cp " << resource << " .";
      cout << "Copying resource from " << resource << " to .";

      int ret = os::system(command.str());
      if (ret != 0) {
        fatal("Copy failed: return code %d", ret);
      }
      resource = "./" + os::basename(resource);
    }

    if (executable &&
        !os::chmod(resource.c_str(), S_IRWXU | S_IRGRP | S_IXGRP |
                          S_IROTH | S_IXOTH)) {
      fatalerror("chmod failed");
    }

    // Extract any .tgz, tar.gz, or zip files.
    if (strings::endsWith(resource, ".tgz") ||
        strings::endsWith(resource, ".tar.gz")) {
      string command = "tar xzf '" + resource + "'";
      cout << "Extracting resource: " + command << endl;
      int code = os::system(command);
      if (code != 0) {
        fatal("Failed to extract resource: tar exit code %d", code);
      }
    } else if (strings::endsWith(resource, ".zip")) {
      string command = "unzip '" + resource + "'";
      cout << "Extracting resource: " + command << endl;
      int code = os::system(command);
      if (code != 0) {
        fatal("Failed to extract resource: unzip exit code %d", code);
      }
    }
  }
}


// Set up environment variables for launching a framework's executor.
void ExecutorLauncher::setupEnvironment()
{
  // Set up the environment as specified in the ExecutorInfo.
  if (commandInfo.has_environment()) {
    foreach (const Environment::Variable& variable,
             commandInfo.environment().variables()) {
      os::setenv(variable.name(), variable.value());
    }
  }

  // Set Mesos environment variables for slave ID, framework ID, etc.
  os::setenv("MESOS_DIRECTORY", workDirectory);
  os::setenv("MESOS_SLAVE_PID", slavePid);
  os::setenv("MESOS_FRAMEWORK_ID", frameworkId.value());
  os::setenv("MESOS_EXECUTOR_ID", executorId.value());

  // Set LIBPROCESS_PORT so that we bind to a random free port.
  os::setenv("LIBPROCESS_PORT", "0");
}


void ExecutorLauncher::switchUser()
{
  if (!os::su(user)) {
    fatal("Failed to switch to user %s for executor %s of framework %s",
          user.c_str(), executorId.value().c_str(), frameworkId.value().c_str());
  }
}


void ExecutorLauncher::setupEnvironmentForLauncherMain()
{
  setupEnvironment();

  // Set up Mesos environment variables that launcher/main.cpp will
  // pass as arguments to an ExecutorLauncher there.
  string uris = "";
  foreach (const CommandInfo::URI& uri, commandInfo.uris()) {
   uris += uri.value() + "+" +
           (uri.has_executable() && uri.executable() ? "1" : "0");
   uris += " ";
  }

  // Remove extra space at the end.
  if (uris.size() > 0) {
    uris = strings::trim(uris);
  }

  os::setenv("MESOS_FRAMEWORK_ID", frameworkId.value());
  os::setenv("MESOS_COMMAND", commandInfo.value());
  os::setenv("MESOS_EXECUTOR_URIS", uris);
  os::setenv("MESOS_USER", user);
  os::setenv("MESOS_WORK_DIRECTORY", workDirectory);
  os::setenv("MESOS_SLAVE_PID", slavePid);
  os::setenv("MESOS_HADOOP_HOME", hadoopHome);
  os::setenv("MESOS_REDIRECT_IO", redirectIO ? "1" : "0");
  os::setenv("MESOS_SWITCH_USER", shouldSwitchUser ? "1" : "0");
  os::setenv("MESOS_CONTAINER", container);
}

} // namespace launcher {
} // namespace internal {
} // namespace mesos {
