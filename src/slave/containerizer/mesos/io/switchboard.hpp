// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef __MESOS_CONTAINERIZER_IO_SWITCHBOARD_HPP__
#define __MESOS_CONTAINERIZER_IO_SWITCHBOARD_HPP__

#include <string>

#include <process/future.hpp>
#include <process/http.hpp>
#include <process/owned.hpp>
#include <process/socket.hpp>

#include <stout/try.hpp>

#include <mesos/slave/containerizer.hpp>
#include <mesos/slave/container_logger.hpp>

#include "slave/flags.hpp"

#include "slave/containerizer/mesos/isolator.hpp"

namespace mesos {
namespace internal {
namespace slave {

// The `IOSwitchboard` is a component in the agent whose job it is to
// instantiate an `IOSwitchboardServer` that can be used to feed the
// stdin to a container from an external source, as well as redirect
// the stdin/stdout of a container to multiple targets.
//
// The primary motivation of this component is to enable support in
// mesos similar to `docker attach` and `docker exec` whereby an
// external client can attach to the stdin/stdout/stderr of a running
// container as well as launch arbitrary subcommands inside a
// container and attach to its stdin/stdout/stderr.
//
// The I/O switchboard is integrated with `MesosContainerizer` through
// the `Isolator` interface.
class IOSwitchboard : public MesosIsolatorProcess
{
public:
  static Try<IOSwitchboard*> create(
      const Flags& flags,
      bool local);

  ~IOSwitchboard() override;

  bool supportsNesting() override;
  bool supportsStandalone() override;

  process::Future<Nothing> recover(
    const std::vector<mesos::slave::ContainerState>& states,
    const hashset<ContainerID>& orphans) override;

  process::Future<Option<mesos::slave::ContainerLaunchInfo>> prepare(
      const ContainerID& containerId,
      const mesos::slave::ContainerConfig& containerConfig) override;

  process::Future<mesos::slave::ContainerLimitation> watch(
    const ContainerID& containerId) override;

  process::Future<Nothing> cleanup(
      const ContainerID& containerId) override;

  // Connect to the `IOSwitchboard` associated with `containerId`.
  process::Future<process::http::Connection> connect(
      const ContainerID& containerId) const;

  // Transfer ownership of a `ContainerIO` struct for a given
  // container out of the `IOSwitchboard` and into the caller.
  process::Future<Option<mesos::slave::ContainerIO>> extractContainerIO(
      const ContainerID& containerID);

  // Helper function that returns `true` if `IOSwitchboardServer`
  // needs to be enabled for the given `ContainerConfig`. It must
  // be enabled for `DEBUG` containers and ones that need `TTYInfo`.
  static bool requiresServer(
      const mesos::slave::ContainerConfig& containerConfig);

private:
  struct Info
  {
    Info(Option<pid_t> _pid, const process::Future<Option<int>>& _status)
      : pid(_pid),
        status(_status) {}

    Option<pid_t> pid;
    process::Future<Option<int>> status;
    process::Promise<mesos::slave::ContainerLimitation> limitation;
  };

  IOSwitchboard(
      const Flags& flags,
      bool local,
      process::Owned<mesos::slave::ContainerLogger> logger);

  process::Future<Option<mesos::slave::ContainerLaunchInfo>> _prepare(
      const ContainerID& containerId,
      const mesos::slave::ContainerConfig& containerConfig,
      const mesos::slave::ContainerIO& loggerIO);

  process::Future<process::http::Connection> _connect(
      const ContainerID& containerId) const;

  process::Future<Option<mesos::slave::ContainerIO>> _extractContainerIO(
      const ContainerID& containerID);

#ifndef __WINDOWS__
  void reaped(
      const ContainerID& containerId,
      const process::Future<Option<int>>& future);
#endif // __WINDOWS__

  Flags flags;
  bool local;
  process::Owned<mesos::slave::ContainerLogger> logger;
  hashmap<ContainerID, process::Owned<Info>> infos;

  // We use a separate hashmap to hold the `ContainerIO` for each
  // container because we need to maintain this information even in
  // the case were we only instantiate the logger and never spawn an
  // `IOSwitchbaordProcess`. Also, the lifetime of the `ContainerIO`
  // is shorter lived than the `Info` struct, as it should be removed
  // from this hash as soon as ownership is transferred out of the
  // `IOSwitchboard` via a call to `extractContainerIO()`.
  hashmap<ContainerID, mesos::slave::ContainerIO> containerIOs;
};


#ifndef __WINDOWS__
// The `IOSwitchboardServer` encapsulates the server side logic for
// redirecting the `stdin/stdout/stderr` of a container to/from
// multiple sources/targets. It runs an HTTP server over a unix domain
// socket in order to process incoming `ATTACH_CONTAINER_INPUT` and
// `ATTACH_CONTAINER_OUTPUT` calls and redirect a containers
// `stdin/stdout/stderr` through them. In 'local' mode, it is run
// inside the agent itself. In 'non-local' mode, it is run as an
// external process to survive agent restarts.
class IOSwitchboardServerProcess;


class IOSwitchboardServer
{
public:
  // The set of flags to pass to the io switchboard server when
  // launched in an external binary.
  struct Flags : public virtual flags::FlagsBase
  {
    Flags()
    {
      setUsageMessage(
        "Usage: " + stringify(NAME) + " [options]\n"
        "The io switchboard server is designed to feed stdin to a container\n"
        "from an external source, as well as redirect the stdin/stdout of a\n"
        "container to multiple targets.\n"
        "\n"
        "It runs an HTTP server over a unix domain socket in order to process\n"
        "incoming `ATTACH_CONTAINER_INPUT` and `ATTACH_CONTAINER_OUTPUT`\n"
        "calls and redirect a containers `stdin/stdout/stderr` through them.\n"
        "\n"
        "The primary motivation of this component is to enable support in\n"
        "mesos similar to `docker attach` and `docker exec` whereby an\n"
        "external client can attach to the stdin/stdout/stderr of a running\n"
        "container as well as launch arbitrary subcommands inside a container\n"
        "and attach to its stdin/stdout/stderr.\n");

      add(&Flags::tty,
          "tty",
          "If a pseudo terminal has been allocated for the container.",
           false);

      add(&Flags::stdin_to_fd,
          "stdin_to_fd",
          "The file descriptor where incoming stdin data should be written.");

      add(&Flags::stdout_from_fd,
          "stdout_from_fd",
          "The file descriptor that should be read to consume stdout data.");

      add(&Flags::stdout_to_fd,
          "stdout_to_fd",
          "A file descriptor where data read from\n"
          "'stdout_from_fd' should be redirected to.");

      add(&Flags::stderr_from_fd,
          "stderr_from_fd",
          "The file descriptor that should be read to consume stderr data.");

      add(&Flags::stderr_to_fd,
          "stderr_to_fd",
          "A file descriptor where data read from\n"
          "'stderr_from_fd' should be redirected to.");

      add(&Flags::wait_for_connection,
          "wait_for_connection",
          "A boolean indicating whether the server should wait for the\n"
          "first connection before reading any data from the '*_from_fd's.",
          false);

      add(&Flags::socket_path,
          "socket_address",
          "The path of the unix domain socket this\n"
          "io switchboard should attach itself to.");

      add(&Flags::heartbeat_interval,
          "heartbeat_interval",
          "A heartbeat interval (e.g. '5secs', '10mins') for messages to\n"
          "be sent to any open 'ATTACH_CONTAINER_OUTPUT' connections.");
    }

    bool tty;
    Option<int> stdin_to_fd;
    Option<int> stdout_from_fd;
    Option<int> stdout_to_fd;
    Option<int> stderr_from_fd;
    Option<int> stderr_to_fd;
    Option<std::string> socket_path;
    bool wait_for_connection;
    Option<Duration> heartbeat_interval;
  };

  static const char NAME[];

  static Try<process::Owned<IOSwitchboardServer>> create(
      bool tty,
      int stdinToFd,
      int stdoutFromFd,
      int stdoutToFd,
      int stderrFromFd,
      int stderrToFd,
      const std::string& socketPath,
      bool waitForConnection = false,
      Option<Duration> heartbeatInterval = None());

  ~IOSwitchboardServer();

  // Run the io switchboard server.
  process::Future<Nothing> run();

  // Forcibly unblock the io switchboard server if it
  // has been started with `waitForConnection` set to `true`.
  process::Future<Nothing> unblock();

private:
  IOSwitchboardServer(
      bool tty,
      int stdinToFd,
      int stdoutFromFd,
      int stdoutToFd,
      int stderrFromFd,
      int stderrToFd,
      const process::network::unix::Socket& socket,
      bool waitForConnection,
      Option<Duration> heartbeatInterval);

  process::Owned<IOSwitchboardServerProcess> process;
};
#endif // __WINDOWS__

} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __MESOS_CONTAINERIZER_IO_SWITCHBOARD_HPP__
