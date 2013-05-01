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

#ifndef __LAUNCHER_HPP__
#define __LAUNCHER_HPP__

#include <map>
#include <string>

#include <mesos/mesos.hpp>

#include <stout/uuid.hpp>

#include "slave/flags.hpp"

namespace mesos {
namespace internal {
namespace launcher {

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
// Isolators that wish to override the default behaviour can subclass
// Launcher and override some of the methods to perform extra actions.
class ExecutorLauncher {
public:
  ExecutorLauncher(
      const SlaveID& slaveId,
      const FrameworkID& frameworkId,
      const ExecutorID& executorId,
      const UUID& uuid,
      const CommandInfo& commandInfo,
      const std::string& user,
      const std::string& workDirectory,
      const std::string& slaveWorkDirectory,
      const std::string& slavePid,
      const std::string& frameworksHome,
      const std::string& hadoopHome,
      bool redirectIO,
      bool shouldSwitchUser,
      bool checkpoint);

  virtual ~ExecutorLauncher();

  // Initialize the working directory and fetch the executor.
  virtual int setup();

  // Launches the downloaded executor.
  virtual int launch();

  // Convenience function that calls setup() and then launch().
  virtual int run();

  // Return a map of environment variables for exec'ing a
  // launch_main.cpp (mesos-launcher binary) process. This is used
  // by isolators that cannot exec the user's executor directly
  // (e.g., due to potential deadlocks in forked process).
  virtual std::map<std::string, std::string> getLauncherEnvironment();

protected:
  // Download the required files for the executor from the given set of URIs.
  // Optionally, it will set the executable file permissions for the files.
  // This method is expected to place files in the workDirectory.
  virtual int fetchExecutors();

  // Return a map of environment variables for launching a
  // framework's executor.
  virtual std::map<std::string, std::string> getEnvironment();

  // Set up environment variables for launching a
  // framework's executor.
  virtual void setupEnvironment();

  // Switch to a framework's user in preparation for exec()'ing its executor.
  virtual void switchUser();

protected:
  const SlaveID slaveId;
  const FrameworkID frameworkId;
  const ExecutorID executorId;
  const UUID uuid;
  const CommandInfo commandInfo;
  const std::string user;
  const std::string workDirectory;
  const std::string slaveDirectory;
  const std::string slavePid;
  const std::string frameworksHome;
  const std::string hadoopHome;
  const bool redirectIO;   // Whether to redirect stdout and stderr to files.
  const bool shouldSwitchUser; // Whether to setuid to framework's user.
  const bool checkpoint; // Whether the framework enabled checkpointing.
};

} // namespace launcher {
} // namespace internal {
} // namespace mesos {

#endif // __LAUNCHER_HPP__
