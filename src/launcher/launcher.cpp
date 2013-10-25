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
#include <pwd.h>
#include <stdlib.h>
#include <unistd.h>

#include <iostream>
#include <map>
#include <sstream>

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <stout/fatal.hpp>
#include <stout/foreach.hpp>
#include <stout/net.hpp>
#include <stout/nothing.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>

#include "hdfs/hdfs.hpp"

#include "launcher/launcher.hpp"

#include "slave/flags.hpp"
#include "slave/paths.hpp"
#include "slave/state.hpp"

using std::cerr;
using std::cout;
using std::endl;
using std::map;
using std::ostringstream;
using std::string;

namespace mesos {
namespace internal {
namespace launcher {

ExecutorLauncher::ExecutorLauncher(
    const SlaveID& _slaveId,
    const FrameworkID& _frameworkId,
    const ExecutorID& _executorId,
    const UUID& _uuid,
    const CommandInfo& _commandInfo,
    const string& _user,
    const string& _workDirectory,
    const string& _slaveDirectory,
    const string& _slavePid,
    const string& _frameworksHome,
    const string& _hadoopHome,
    bool _redirectIO,
    bool _shouldSwitchUser,
    bool _checkpoint,
    Duration _recoveryTimeout)
  : slaveId(_slaveId),
    frameworkId(_frameworkId),
    executorId(_executorId),
    uuid(_uuid),
    commandInfo(_commandInfo),
    user(_user),
    workDirectory(_workDirectory),
    slaveDirectory(_slaveDirectory),
    slavePid(_slavePid),
    frameworksHome(_frameworksHome),
    hadoopHome(_hadoopHome),
    redirectIO(_redirectIO),
    shouldSwitchUser(_shouldSwitchUser),
    checkpoint(_checkpoint),
    recoveryTimeout(_recoveryTimeout) {}


ExecutorLauncher::~ExecutorLauncher() {}


// NOTE: We avoid fatalerror()s in this function because, we don't
// want to kill the slave (in the case of cgroups isolator).
int ExecutorLauncher::setup()
{
  // Checkpoint the forked pid, if necessary. The checkpointing must
  // be done in the forked process (cgroups isolator) or execed
  // launcher process (process isolator), because the slave process
  // can die immediately after the isolator forks but before it would
  // have a chance to write the pid to disk. That would result in an
  // orphaned executor process unknown to the recovering slave.
  if (checkpoint) {
    const string& path = slave::paths::getForkedPidPath(
        slave::paths::getMetaRootDir(slaveDirectory),
        slaveId,
        frameworkId,
        executorId,
        uuid);
    cout << "Checkpointing executor's forked pid " << getpid()
         << " to '" << path <<  "'" << endl;

    Try<Nothing> checkpoint =
      slave::state::checkpoint(path, stringify(getpid()));

    if (checkpoint.isError()) {
      cerr << "Failed to checkpoint executor's forked pid to '"
           << path << "': " << checkpoint.error();
      return -1;
    }
  }

  const string& cwd = os::getcwd();

  // TODO(benh): Do this in the slave?
  if (shouldSwitchUser) {
    Try<Nothing> chown = os::chown(user, workDirectory);

    if (chown.isError()) {
      cerr << "Failed to change ownership of the executor work directory "
           << workDirectory << " to user " << user << ": " << chown.error()
           << endl;
      return -1;
    }
  }

  // Enter working directory.
  if (!os::chdir(workDirectory)) {
    cerr << "Failed to chdir into executor work directory" << endl;
    return -1;
  }

  // Redirect output to files in working dir if required.
  // TODO(bmahler): It would be best if instead of closing stderr /
  // stdout and redirecting, we instead always output to stderr /
  // stdout. Also tee'ing their output into the work directory files
  // when redirection is desired.
  if (redirectIO) {
    if (freopen("stdout", "w", stdout) == NULL) {
      fatalerror("freopen failed");
    }
    if (freopen("stderr", "w", stderr) == NULL) {
      fatalerror("freopen failed");
    }
  }

  if (fetchExecutors() < 0) {
    cerr << "Failed to fetch executors" << endl;
    return -1;
  }

  // Go back to previous directory.
  if (!os::chdir(cwd)) {
    cerr << "Failed to chdir (back) into slave directory" << endl;
    return -1;
  }

  return 0;
}


int ExecutorLauncher::launch()
{
  // Enter working directory.
  if (os::chdir(workDirectory) < 0) {
    fatalerror("Failed to chdir into the executor work directory");
  }

  if (shouldSwitchUser) {
    switchUser();
  }

  setupEnvironment();

  const string& command = commandInfo.value();

  // Execute the command (via '/bin/sh -c command').
  execl("/bin/sh", "sh", "-c", command.c_str(), (char*) NULL);

  // If we get here, the execv call failed.
  fatalerror("Could not execute '/bin/sh -c %s'", command.c_str());

  return -1; // Silence end of non-void function warning.
}


int ExecutorLauncher::run()
{
  int ret = setup();
  if (ret < 0) {
    return ret;
  }
  return launch();
}


// Download the executor's files and optionally set executable permissions
// if requested.
int ExecutorLauncher::fetchExecutors()
{
  cout << "Fetching resources into '" << workDirectory << "'" << endl;

  foreach(const CommandInfo::URI& uri, commandInfo.uris()) {
    string resource = uri.value();
    bool executable = uri.has_executable() && uri.executable();

    cout << "Fetching resource '" << resource << "'" << endl;

    // Some checks to make sure using the URI value in shell commands
    // is safe. TODO(benh): These should be pushed into the scheduler
    // driver and reported to the user.
    if (resource.find_first_of('\\') != string::npos ||
        resource.find_first_of('\'') != string::npos ||
        resource.find_first_of('\0') != string::npos) {
      cerr << "Illegal characters in URI" << endl;
      return -1;
    }

    // Grab the resource from HDFS if its path begins with hdfs:// or
    // htfp://. TODO(matei): Enforce some size limits on files we get
    // from HDFS
    if (resource.find("hdfs://") == 0 || resource.find("hftp://") == 0) {
      HDFS hdfs(path::join(hadoopHome, "bin/hadoop"));

      Try<std::string> basename = os::basename(resource);
      if (basename.isError()) {
        cerr << basename.error() << endl;
        return -1;
      }

      string localFile = path::join(".", basename.get());

      Try<Nothing> copy = hdfs.copyToLocal(resource, localFile);

      if (copy.isError()) {
        cerr << "Failed to copy from HDFS: " << copy.error() << endl;
        return -1;
      }

      resource = localFile;
    } else if (resource.find("http://") == 0
               || resource.find("https://") == 0
               || resource.find("ftp://") == 0
               || resource.find("ftps://") == 0) {
      string path = resource.substr(resource.find("://") + 3);
      if (path.find("/") == string::npos) {
        cerr << "Malformed URL (missing path)" << endl;
        return -1;
      }

      if (path.size() <= path.find("/") + 1) {
        cerr << "Malformed URL (missing path)" << endl;
        return -1;
      }

      path =  path::join(".", path.substr(path.find_last_of("/") + 1));
      cout << "Downloading '" << resource << "' to '" << path << "'" << endl;
      Try<int> code = net::download(resource, path);
      if (code.isError()) {
        cerr << "Error downloading resource: " << code.error().c_str() << endl;
        return -1;
      } else if (code.get() != 200) {
        cerr << "Error downloading resource, received HTTP/FTP return code "
             << code.get() << endl;
        return -1;
      }
      resource = path;
    } else { // Copy the local resource.
      if (resource.find_first_of("/") != 0) {
        // We got a non-Hadoop and non-absolute path.
        if (frameworksHome != "") {
          resource = path::join(frameworksHome, resource);
          cout << "Prepended configuration option frameworks_home to resource "
               << "path, making it: '" << resource << "'" << endl;
        } else {
          cerr << "A relative path was passed for the resource, but "
               << "the configuration option frameworks_home is not set. "
               << "Please either specify this config option "
               << "or avoid using a relative path" << endl;
          return -1;
        }
      }

      // Copy the resource to the current working directory.
      ostringstream command;
      command << "cp '" << resource << "' .";
      cout << "Copying resource from '" << resource << "' to ." << endl;

      int status = os::system(command.str());
      if (status != 0) {
        cerr << "Failed to copy '" << resource
             << "' : Exit status " << status << endl;
        return -1;
      }

      Try<std::string> base = os::basename(resource);
      if (base.isError()) {
        cerr << base.error() << endl;
        return -1;
      }

      resource = path::join(".", base.get());
    }

    if (shouldSwitchUser) {
      Try<Nothing> chown = os::chown(user, resource);

      if (chown.isError()) {
        cerr << "Failed to chown '" << resource << "' to user " << user << ": "
             << chown.error() << endl;
        return -1;
      }
    }

    if (executable &&
        !os::chmod(resource, S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH)) {
      cerr << "Failed to chmod '" << resource << "'" << endl;
      return -1;
    }

    // Extract any .tgz, tar.gz, tar.bz2 or zip files.
    if (strings::endsWith(resource, ".tgz") ||
        strings::endsWith(resource, ".tar.gz")) {
      string command = "tar xzf '" + resource + "'";
      cout << "Extracting resource: " << command << endl;
      int code = os::system(command);
      if (code != 0) {
        cerr << "Failed to extract resource: tar exit code " << code << endl;
        return -1;
      }
    } else if (strings::endsWith(resource, ".tbz2") ||
               strings::endsWith(resource, ".tar.bz2")) {
      string command = "tar xjf '" + resource + "'";
      cout << "Extracting resource: " << command << endl;
      int code = os::system(command);
      if (code != 0) {
        cerr << "Failed to extract resource: tar exit code " << code << endl;
        return -1;
      }
    } else if (strings::endsWith(resource, ".txz") ||
               strings::endsWith(resource, ".tar.xz")) {
      // If you want to use XZ on Mac OS, you can try the packages here:
      // http://macpkg.sourceforge.net/
      string command = "tar xJf '" + resource + "'";
      cout << "Extracting resource: " << command << endl;
      int code = os::system(command);
      if (code != 0) {
        cerr << "Failed to extract resource: tar exit code " << code << endl;
        return -1;
      }
    } else if (strings::endsWith(resource, ".zip")) {
      string command = "unzip '" + resource + "'";
      cout << "Extracting resource: " << command << endl;
      int code = os::system(command);
      if (code != 0) {
        cerr << "Failed to extract resource: unzip exit code " << code << endl;
        return -1;
      }
    }
  }

  // Recursively chown the work directory, since extraction may have occurred.
  if (shouldSwitchUser) {
    Try<Nothing> chown = os::chown(user, ".");

    if (chown.isError()) {
      cerr << "Failed to recursively chown the work directory "
           << workDirectory << " to user " << user << ": " << chown.error()
           << endl;
      return -1;
    }
  }

  return 0;
}


void ExecutorLauncher::switchUser()
{
  if (!os::su(user)) {
    fatal("Failed to switch to user %s for executor %s of framework %s",
          user.c_str(), executorId.value().c_str(), frameworkId.value().c_str());
  }
}


// Set up environment variables for launching a framework's executor.
void ExecutorLauncher::setupEnvironment()
{
  foreachpair (const string& key, const string& value, getEnvironment()) {
    os::setenv(key, value);
  }
}


map<string, string> ExecutorLauncher::getEnvironment()
{
  map<string, string> env;

  // Set LIBPROCESS_PORT so that we bind to a random free port (since
  // this might have been set via --port option). We do this before
  // the environment variables below in case it is included.
  env["LIBPROCESS_PORT"] = "0";

  // Also add MESOS_NATIVE_LIBRARY if it's not already present (and
  // like above, we do this before the environment variables below in
  // case the framework wants to override).
  if (!os::hasenv("MESOS_NATIVE_LIBRARY")) {
    string path =
#ifdef __APPLE__
      LIBDIR "/libmesos-" VERSION ".dylib";
#else
      LIBDIR "/libmesos-" VERSION ".so";
#endif
    if (os::exists(path)) {
      env["MESOS_NATIVE_LIBRARY"] = path;
    }
  }

  // Set up the environment as specified in the ExecutorInfo.
  if (commandInfo.has_environment()) {
    foreach (const Environment::Variable& variable,
             commandInfo.environment().variables()) {
      env[variable.name()] = variable.value();
    }
  }

  // Set Mesos environment variables for slave ID, framework ID, etc.
  env["MESOS_DIRECTORY"] = workDirectory;
  env["MESOS_SLAVE_PID"] = slavePid;
  env["MESOS_SLAVE_ID"] = slaveId.value();
  env["MESOS_FRAMEWORK_ID"] = frameworkId.value();
  env["MESOS_EXECUTOR_ID"] = executorId.value();
  env["MESOS_EXECUTOR_UUID"] = uuid.toString();
  env["MESOS_CHECKPOINT"] = checkpoint ? "1" : "0";

  if (checkpoint) {
    env["MESOS_RECOVERY_TIMEOUT"] = stringify(recoveryTimeout);
  }

  return env;
}


// Get Mesos environment variables that launcher/main.cpp will
// pass as arguments to an ExecutorLauncher there.
map<string, string> ExecutorLauncher::getLauncherEnvironment()
{
  map<string, string> env = getEnvironment();

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

  env["MESOS_EXECUTOR_URIS"] = uris;
  env["MESOS_COMMAND"] = commandInfo.value();
  env["MESOS_USER"] = user;
  env["MESOS_SLAVE_DIRECTORY"] = slaveDirectory;
  env["MESOS_HADOOP_HOME"] = hadoopHome;
  env["MESOS_REDIRECT_IO"] = redirectIO ? "1" : "0";
  env["MESOS_SWITCH_USER"] = shouldSwitchUser ? "1" : "0";

  return env;
}

} // namespace launcher {
} // namespace internal {
} // namespace mesos {
