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

#include <stdio.h>
#include <stdlib.h>

#include <list>
#include <map>
#include <string>
#include <vector>

#include <process/address.hpp>
#include <process/after.hpp>
#include <process/clock.hpp>
#include <process/collect.hpp>
#include <process/defer.hpp>
#include <process/delay.hpp>
#include <process/dispatch.hpp>
#include <process/future.hpp>
#include <process/http.hpp>
#include <process/io.hpp>
#include <process/loop.hpp>
#include <process/owned.hpp>
#include <process/process.hpp>
#include <process/reap.hpp>
#include <process/shared.hpp>
#include <process/socket.hpp>
#include <process/subprocess.hpp>

#include <stout/hashmap.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/recordio.hpp>

#include <stout/os/constants.hpp>

#ifndef __WINDOWS__
#include <stout/posix/os.hpp>
#endif // __WINDOWS__

#include <mesos/http.hpp>
#include <mesos/type_utils.hpp>

#include <mesos/agent/agent.hpp>

#include <mesos/slave/containerizer.hpp>
#include <mesos/slave/container_logger.hpp>

#include "common/http.hpp"
#include "common/recordio.hpp"
#include "common/status_utils.hpp"

#ifdef __linux__
#include "linux/systemd.hpp"
#endif // __linux__

#include "slave/flags.hpp"
#include "slave/state.hpp"

#include "slave/containerizer/mesos/paths.hpp"

#include "slave/containerizer/mesos/io/switchboard.hpp"

namespace http = process::http;

#ifndef __WINDOWS__
namespace unix = process::network::unix;
#endif // __WINDOWS__

using namespace mesos::internal::slave::containerizer::paths;

using std::list;
using std::map;
using std::string;
using std::vector;

using process::after;
using process::await;
using process::Break;
using process::Continue;
using process::ControlFlow;
using process::Clock;
using process::ErrnoFailure;
using process::Failure;
using process::Future;
using process::loop;
using process::Owned;
using process::PID;
using process::Process;
using process::Promise;
using process::Shared;
using process::Subprocess;

using process::network::internal::SocketImpl;

using mesos::slave::ContainerConfig;
using mesos::slave::ContainerClass;
using mesos::slave::ContainerLaunchInfo;
using mesos::slave::ContainerLimitation;
using mesos::slave::ContainerLogger;
using mesos::slave::ContainerIO;
using mesos::slave::ContainerState;
using mesos::slave::Isolator;

namespace mesos {
namespace internal {
namespace slave {

Try<IOSwitchboard*> IOSwitchboard::create(
    const Flags& flags,
    bool local)
{
  Try<ContainerLogger*> logger =
    ContainerLogger::create(flags.container_logger);

  if (logger.isError()) {
    return Error("Cannot create container logger: " + logger.error());
  }

  return new IOSwitchboard(
      flags,
      local,
      Owned<ContainerLogger>(logger.get()));
}


IOSwitchboard::IOSwitchboard(
    const Flags& _flags,
    bool _local,
    Owned<ContainerLogger> _logger)
  : flags(_flags),
    local(_local),
    logger(_logger) {}


IOSwitchboard::~IOSwitchboard() {}


bool IOSwitchboard::supportsNesting()
{
  return true;
}


bool IOSwitchboard::supportsStandalone()
{
  return true;
}


Future<Nothing> IOSwitchboard::recover(
    const vector<ContainerState>& states,
    const hashset<ContainerID>& orphans)
{
#ifdef __WINDOWS__
  return Nothing();
#else
  if (local) {
    return Nothing();
  }

  // Recover any active container's io switchboard info.
  //
  // NOTE: If a new agent is started with io switchboard server mode
  // disabled, we will still recover the io switchboard info for
  // containers previously launched by an agent with server mode enabled.
  foreach (const ContainerState& state, states) {
    const ContainerID& containerId = state.container_id();

    const string path = getContainerIOSwitchboardPath(
        flags.runtime_dir, containerId);

    // If we don't have a checkpoint directory created for this
    // container's io switchboard, there is nothing to recover. This
    // can only happen for containers that were launched with `DEFAULT`
    // ContainerClass *and* no `TTYInfo` set.
    if (!os::exists(path)) {
      continue;
    }

    Result<pid_t> pid = getContainerIOSwitchboardPid(
        flags.runtime_dir, containerId);

    // For active containers that have an io switchboard directory,
    // we should *always* have a valid pid file. If we don't that is a
    // an error and we should fail appropriately.
    if (!pid.isSome()) {
      return Failure("Failed to get I/O switchboard server pid for"
                     " '" + stringify(containerId) + "':"
                     " " + (pid.isError() ?
                            pid.error() :
                            "pid file does not exist"));
    }

    infos[containerId] = Owned<Info>(new Info(
      pid.get(),
      process::reap(pid.get()).onAny(defer(
          PID<IOSwitchboard>(this),
          &IOSwitchboard::reaped,
          containerId,
          lambda::_1))));
  }

  // Recover the io switchboards from any orphaned containers.
  foreach (const ContainerID& orphan, orphans) {
    const string path = getContainerIOSwitchboardPath(
        flags.runtime_dir, orphan);

    // If we don't have a checkpoint directory created for this
    // container's io switchboard, there is nothing to recover.
    if (!os::exists(path)) {
      continue;
    }

    Result<pid_t> pid = getContainerIOSwitchboardPid(
        flags.runtime_dir, orphan);

    // If we were able to retrieve the checkpointed pid, we simply
    // populate our info struct and rely on the containerizer to
    // destroy the orphaned container and call `cleanup()` on us later.
    if (pid.isSome()) {
      infos[orphan] = Owned<Info>(new Info(
        pid.get(),
        process::reap(pid.get()).onAny(defer(
            PID<IOSwitchboard>(this),
            &IOSwitchboard::reaped,
            orphan,
            lambda::_1))));
    } else {
      // If we were not able to retrieve the checkpointed pid, we
      // still need to populate our info struct (but with a pid value
      // of `None()`). This way when `cleanup()` is called, we still
      // do whatever cleanup we can (we just don't wait for the pid
      // to be reaped -- we do it immediately).
      //
      // We could enter this case under 4 conditions:
      //
      // (1) The io switchboard we are recovering was launched, but
      //     the agent died before checkpointing its pid.
      // (2) The io switchboard pid file was removed.
      // (3) There was an error reading the io switchbaord pid file.
      // (4) The io switchboard pid file was corrupted.
      //
      // We log an error in cases (3) and (4).
      infos[orphan] = Owned<Info>(new Info(
        None(),
        Future<Option<int>>(None())));

      if (pid.isError()) {
        LOG(ERROR) << "Error retrieving the 'IOSwitchboard' pid file"
                      " for orphan '" << orphan << "': " << pid.error();
      }
    }
  }

  return Nothing();
#endif // __WINDOWS__
}


Future<Option<ContainerLaunchInfo>> IOSwitchboard::prepare(
    const ContainerID& containerId,
    const ContainerConfig& containerConfig)
{
  // In local mode, the container will inherit agent's stdio.
  if (local) {
    containerIOs[containerId] = ContainerIO();
    return None();
  }

  // TODO(jieyu): Currently, if the agent fails over after the
  // executor is launched, but before its nested containers are
  // launched, the nested containers launched later might not have
  // access to the root parent container's ExecutorInfo (i.e.,
  // 'containerConfig.executor_info()' will be empty).
  return logger->prepare(containerId, containerConfig)
    .then(defer(
        PID<IOSwitchboard>(this),
        &IOSwitchboard::_prepare,
        containerId,
        containerConfig,
        lambda::_1));
}


Future<Option<ContainerLaunchInfo>> IOSwitchboard::_prepare(
    const ContainerID& containerId,
    const ContainerConfig& containerConfig,
    const ContainerIO& loggerIO)
{
  bool requiresServer = IOSwitchboard::requiresServer(containerConfig);

  // On windows, we do not yet support running an io switchboard
  // server, so we must error out if it is required.
#ifdef __WINDOWS__
  if (requiresServer) {
      return Failure(
          "IO Switchboard server is not supported on windows");
  }
#endif

  LOG(INFO) << "Container logger module finished preparing container "
            << containerId << "; IOSwitchboard server is "
            << (requiresServer ? "" : "not") << " required";

  bool hasTTY = containerConfig.has_container_info() &&
                containerConfig.container_info().has_tty_info();

  if (!requiresServer) {
    CHECK(!containerIOs.contains(containerId));
    containerIOs[containerId] = loggerIO;

    return ContainerLaunchInfo();
  }

#ifdef __WINDOWS__
  // NOTE: On Windows, both return values of
  // `IOSwitchboard::requiresServer(containerConfig)` are checked and will
  // return before reaching here.
  UNREACHABLE();
#else
  // First make sure that we haven't already spawned an io
  // switchboard server for this container.
  if (infos.contains(containerId)) {
    return Failure("Already prepared io switchboard server for container"
                   " '" + stringify(containerId) + "'");
  }

  // We need this so we can return the
  // `tty_slave_path` if there is one.
  ContainerLaunchInfo launchInfo;

  // We assign this variable to an entry in the `containerIOs` hashmap
  // at the bottom of this function. We declare it here so we can
  // populate it throughout this function and only store it back to
  // the hashmap once we know this function has succeeded.
  ContainerIO containerIO;

  // Manually construct pipes instead of using `Subprocess::PIPE`
  // so that the ownership of the FDs is properly represented. The
  // `Subprocess` spawned below owns one end of each pipe and will
  // be solely responsible for closing that end. The ownership of
  // the other end will be passed to the caller of this function
  // and eventually passed to the container being launched.
  int stdinToFd = -1;
  int stdoutFromFd = -1;
  int stderrFromFd = -1;

  // A list of file descriptors we've opened so far.
  hashset<int> openedFds = {};

  // A list of file descriptors that will be passed to the I/O
  // switchboard. We need to close those file descriptors once the
  // I/O switchboard server is forked.
  hashset<int> ioSwitchboardFds = {};

  // Helper for closing a set of file descriptors.
  auto close = [](const hashset<int>& fds) {
    foreach (int fd, fds) {
      os::close(fd);
    }
  };

  // Setup a pseudo terminal for the container.
  if (hasTTY) {
    // TODO(jieyu): Consider moving all TTY related method to stout.
    // For instance, 'stout/posix/tty.hpp'.

    // Set flag 'O_NOCTTY' so that the terminal device will not become
    // the controlling terminal for the process.
    int master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (master == -1) {
      return Failure("Failed to open a master pseudo terminal");
    }

    openedFds.insert(master);

    Try<string> slavePath = os::ptsname(master);
    if (slavePath.isError()) {
      close(openedFds);
      return Failure("Failed to get the slave pseudo terminal path: " +
                     slavePath.error());
    }

    // Unlock the slave end of the pseudo terminal.
    if (unlockpt(master) != 0) {
      close(openedFds);
      return ErrnoFailure("Failed to unlock the slave pseudo terminal");
    }

    // Set proper permission and ownership for the device.
    if (grantpt(master) != 0) {
      close(openedFds);
      return ErrnoFailure("Failed to grant the slave pseudo terminal");
    }

    if (containerConfig.has_user()) {
      Try<Nothing> chown = os::chown(
          containerConfig.user(),
          slavePath.get(),
          false);

      if (chown.isError()) {
        close(openedFds);
        return Failure("Failed to chown the slave pseudo terminal: " +
                       chown.error());
      }
    }

    // Open the slave end of the pseudo terminal. The opened file
    // descriptor will be dup'ed to stdin/out/err of the container.
    Try<int> slave = os::open(slavePath.get(), O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (slave.isError()) {
      return Failure("Failed to open the slave pseudo terminal: " +
                     slave.error());
    }

    openedFds.insert(slave.get());

    LOG(INFO) << "Allocated pseudo terminal '" << slavePath.get()
              << "' for container " << containerId;

    stdinToFd = master;
    stdoutFromFd = master;
    stderrFromFd = master;

    containerIO.in = ContainerIO::IO::FD(slave.get());
    containerIO.out = containerIO.in;
    containerIO.err = containerIO.in;

    launchInfo.set_tty_slave_path(slavePath.get());

    // The command executor requires the `tty_slave_path`
    // to also be passed as a command line argument.
    if (containerConfig.has_task_info()) {
      launchInfo.mutable_command()->add_arguments(
            "--tty_slave_path=" + slavePath.get());
    }
  } else {
    Try<std::array<int_fd, 2>> infds_ = os::pipe();
    if (infds_.isError()) {
      close(openedFds);
      return Failure("Failed to create stdin pipe: " + infds_.error());
    }

    const std::array<int_fd, 2>& infds = infds_.get();

    openedFds.insert(infds[0]);
    openedFds.insert(infds[1]);

    Try<std::array<int_fd, 2>> outfds_ = os::pipe();
    if (outfds_.isError()) {
      close(openedFds);
      return Failure("Failed to create stdout pipe: " + outfds_.error());
    }

    const std::array<int_fd, 2>& outfds = outfds_.get();

    openedFds.insert(outfds[0]);
    openedFds.insert(outfds[1]);

    Try<std::array<int_fd, 2>> errfds_ = os::pipe();
    if (errfds_.isError()) {
      close(openedFds);
      return Failure("Failed to create stderr pipe: " + errfds_.error());
    }

    const std::array<int_fd, 2>& errfds = errfds_.get();

    openedFds.insert(errfds[0]);
    openedFds.insert(errfds[1]);

    stdinToFd = infds[1];
    stdoutFromFd = outfds[0];
    stderrFromFd = errfds[0];

    containerIO.in = ContainerIO::IO::FD(infds[0]);
    containerIO.out = ContainerIO::IO::FD(outfds[1]);
    containerIO.err = ContainerIO::IO::FD(errfds[1]);
  }

  // Make sure all file descriptors opened have CLOEXEC set.
  foreach (int fd, openedFds) {
    Try<Nothing> cloexec = os::cloexec(fd);
    if (cloexec.isError()) {
      close(openedFds);
      return Failure("Failed to set cloexec: " + cloexec.error());
    }
  }

  ioSwitchboardFds.insert(stdinToFd);
  ioSwitchboardFds.insert(stdoutFromFd);
  ioSwitchboardFds.insert(stderrFromFd);

  // Set up our flags to send to the io switchboard server process.
  IOSwitchboardServer::Flags switchboardFlags;
  switchboardFlags.tty = hasTTY;
  switchboardFlags.stdin_to_fd = stdinToFd;
  switchboardFlags.stdout_from_fd = stdoutFromFd;
  switchboardFlags.stderr_from_fd = stderrFromFd;
  switchboardFlags.stdout_to_fd = STDOUT_FILENO;
  switchboardFlags.stderr_to_fd = STDERR_FILENO;
  switchboardFlags.heartbeat_interval = flags.http_heartbeat_interval;

  if (containerConfig.container_class() == ContainerClass::DEBUG) {
    switchboardFlags.wait_for_connection = true;
  } else {
    switchboardFlags.wait_for_connection = false;
  }

  switchboardFlags.socket_path = path::join(
      stringify(os::PATH_SEPARATOR),
      "tmp",
      "mesos-io-switchboard-" + id::UUID::random().toString());

  // Just before launching our io switchboard server, we need to
  // create a directory to hold checkpointed files related to the
  // server. The existence of this directory indicates that we
  // intended to launch an io switchboard server on behalf of a
  // container. The lack of any expected files in this directroy
  // during recovery/cleanup indicates that something went wrong and
  // we need to take appropriate action.
  string path = getContainerIOSwitchboardPath(flags.runtime_dir, containerId);

  Try<Nothing> mkdir = os::mkdir(path);
  if (mkdir.isError()) {
    return Failure("Error creating 'IOSwitchboard' checkpoint directory"
                   " for container '" + stringify(containerId) + "':"
                   " " + mkdir.error());
  }

  // Prepare the environment for the io switchboard server process.
  // We inherit agent environment variables except for those
  // LIBPROCESS or MESOS prefixed environment variables since io
  // switchboard server process does not rely on those environment
  // variables.
  map<string, string> environment;
  foreachpair (const string& key, const string& value, os::environment()) {
    if (!strings::startsWith(key, "LIBPROCESS_") &&
        !strings::startsWith(key, "MESOS_")) {
      environment.emplace(key, value);
    }
  }

  // TODO(jieyu): This is to make sure the libprocess of the io
  // switchboard can properly initialize and find the IP. Since we
  // don't need to use the TCP socket for communication, it's OK to
  // use a local address. Consider disable TCP socket in libprocess if
  // libprocess supports that.
  environment.emplace("LIBPROCESS_IP", "127.0.0.1");

  // TODO(jieyu): Consider making this configurable.
  environment.emplace("LIBPROCESS_NUM_WORKER_THREADS", "8");

  VLOG(1) << "Launching '" << IOSwitchboardServer::NAME << "' with flags '"
          << switchboardFlags << "' for container " << containerId;

  // If we are on systemd, then extend the life of the process as we
  // do with the executor. Any grandchildren's lives will also be
  // extended.
  vector<Subprocess::ParentHook> parentHooks;

#ifdef __linux__
  if (systemd::enabled()) {
    parentHooks.emplace_back(Subprocess::ParentHook(
        &systemd::mesos::extendLifetime));
  }
#endif // __linux__

  // Launch the io switchboard server process.
  // We `dup()` the `stdout` and `stderr` passed to us by the
  // container logger over the `stdout` and `stderr` of the io
  // switchboard process itself. In this way, the io switchboard
  // process simply needs to write to its own `stdout` and
  // `stderr` in order to send output to the logger files.
  Try<Subprocess> child = subprocess(
      path::join(flags.launcher_dir, IOSwitchboardServer::NAME),
      {IOSwitchboardServer::NAME},
      Subprocess::PATH(os::DEV_NULL),
      loggerIO.out,
      loggerIO.err,
      &switchboardFlags,
      environment,
      None(),
      parentHooks,
      {Subprocess::ChildHook::SETSID()},
      {stdinToFd, stdoutFromFd, stderrFromFd});

  if (child.isError()) {
    close(openedFds);
    return Failure("Failed to create io switchboard"
                   " server process: " + child.error());
  }

  LOG(INFO) << "Created I/O switchboard server (pid: " << child->pid()
            << ") listening on socket file '"
            << switchboardFlags.socket_path.get()
            << "' for container " << containerId;

  close(ioSwitchboardFds);

  // We remove the already closed file descriptors from 'openedFds' so
  // that we don't close multiple times if failures happen below.
  foreach (int fd, ioSwitchboardFds) {
    openedFds.erase(fd);
  }

  // Now that the child has come up, we checkpoint the socket
  // address we told it to bind to so we can access it later.
  path = getContainerIOSwitchboardSocketPath(flags.runtime_dir, containerId);

  Try<Nothing> checkpointed = slave::state::checkpoint(
      path, switchboardFlags.socket_path.get());

  if (checkpointed.isError()) {
    close(openedFds);
    return Failure("Failed to checkpoint container's socket path to"
                   " '" + path + "': " + checkpointed.error());
  }

  // We also checkpoint the child's pid.
  path = getContainerIOSwitchboardPidPath(flags.runtime_dir, containerId);

  checkpointed = slave::state::checkpoint(path, stringify(child->pid()));

  if (checkpointed.isError()) {
    close(openedFds);
    return Failure("Failed to checkpoint container's io switchboard pid to"
                   " '" + path + "': " + checkpointed.error());
  }

  // Build an info struct for this container.
  infos[containerId] = Owned<Info>(new Info(
    child->pid(),
    process::reap(child->pid()).onAny(defer(
        PID<IOSwitchboard>(this),
        &IOSwitchboard::reaped,
        containerId,
        lambda::_1))));

  // Populate the `containerIOs` hashmap.
  containerIOs[containerId] = containerIO;

  return launchInfo;
#endif // __WINDOWS__
}


Future<http::Connection> IOSwitchboard::connect(
    const ContainerID& containerId) const
{
  return dispatch(self(), [this, containerId]() {
    return _connect(containerId);
  });
}


Future<http::Connection> IOSwitchboard::_connect(
    const ContainerID& containerId) const
{
#ifdef __WINDOWS__
  return Failure("Not supported on Windows");
#else
  if (local) {
    return Failure("Not supported in local mode");
  }

  if (!infos.contains(containerId)) {
    return Failure("I/O switchboard server was disabled for this container");
  }

  // Get the io switchboard address from the `containerId`.
  Result<unix::Address> address = getContainerIOSwitchboardAddress(
      flags.runtime_dir, containerId);

  if (!address.isSome()) {
    return Failure("Failed to get the io switchboard address"
                   ": " + (address.isError() ? address.error() : "Not found"));
  }

  // Wait for the server to create the domain socket file.
  return loop(
      self(),
      []() {
        return after(Milliseconds(10));
      },
      [=](const Nothing&) -> ControlFlow<Nothing> {
        if (infos.contains(containerId) && !os::exists(address->path())) {
          return Continue();
        }
        return Break();
      })
    .then(defer(self(), [=]() -> Future<http::Connection> {
      if (!infos.contains(containerId)) {
        return Failure("I/O switchboard has shutdown");
      }

      return http::connect(address.get(), http::Scheme::HTTP);
    }));
#endif // __WINDOWS__
}


Future<Option<ContainerIO>> IOSwitchboard::extractContainerIO(
    const ContainerID& containerId)
{
  return dispatch(self(), [this, containerId]() {
    return _extractContainerIO(containerId);
  });
}


Future<Option<ContainerIO>> IOSwitchboard::_extractContainerIO(
    const ContainerID& containerId)
{
  if (!containerIOs.contains(containerId)) {
    return None();
  }

  ContainerIO containerIO = containerIOs[containerId];
  containerIOs.erase(containerId);

  return containerIO;
}


Future<ContainerLimitation> IOSwitchboard::watch(
    const ContainerID& containerId)
{
#ifdef __WINDOWS__
  return Future<ContainerLimitation>();
#else
  if (local) {
    return Future<ContainerLimitation>();
  }

  // We ignore unknown containers here because legacy containers
  // without an io switchboard directory will not have an info struct
  // created for during recovery. Likewise, containers launched
  // by a previous agent with io switchboard server mode disabled will
  // not have info structs created for them either. In both cases
  // there is nothing to watch, so we return an unsatisfiable future.
  if (!infos.contains(containerId)) {
    return Future<ContainerLimitation>();
  }

  return infos[containerId]->limitation.future();
#endif // __WINDOWS__
}


Future<Nothing> IOSwitchboard::cleanup(
    const ContainerID& containerId)
{
#ifdef __WINDOWS__
  // Since we don't support spawning an io switchboard server on
  // windows yet, there is nothing to wait for here.
  return Nothing();
#else
  if (local) {
    return Nothing();
  }

  // We ignore unknown containers here because legacy containers
  // without an io switchboard directory will not have an info struct
  // created for them during recovery. Likewise, containers launched
  // by a previous agent with io switchboard server mode disabled will
  // not have info structs created for them either. In both cases
  // there is nothing to cleanup, so we simly return `Nothing()`.
  if (!infos.contains(containerId)) {
    return Nothing();
  }

  Option<pid_t> pid = infos[containerId]->pid;
  Future<Option<int>> status = infos[containerId]->status;

  // If we have a pid, then we attempt to send it a SIGTERM to have it
  // shutdown gracefully. This is best effort, as it's likely that the
  // switchboard has already shutdown in the common case.
  //
  // NOTE: There is an unfortunate race condition here. If the io
  // switchboard terminates and the pid is reused by some other
  // process, we might be sending SIGTERM to a random process. This
  // could be a problem under high load.
  //
  // TODO(jieyu): We give the I/O switchboard server a grace period to
  // wait for the connection from the containerizer. This is for the
  // case where the container itself is short lived (e.g., a DEBUG
  // container does an 'ls' and exits). For that case, we still want
  // the subsequent attach output call to get the output from that
  // container.
  //
  // TODO(klueska): Send a message over the io switchboard server's
  // domain socket instead of using a signal.
  if (pid.isSome() && status.isPending()) {
    Clock::timer(Seconds(5), [pid, status, containerId]() {
      if (status.isPending()) {
        LOG(INFO) << "Sending SIGTERM to I/O switchboard server (pid: "
                  << pid.get() << ") since container " << containerId
                  << " is being destroyed";

        os::kill(pid.get(), SIGTERM);

        Clock::timer(Seconds(60), [pid, status, containerId]() {
          if (status.isPending()) {
            // If we are here, something really bad must have happened for I/O
            // switchboard server to not exit after SIGTERM has been sent. We
            // have seen this happen due to FD leak (see MESOS-9502). We do a
            // SIGKILL here as a safeguard so that switchboard server forcefully
            // exits and causes this cleanup feature to be completed, thus
            // unblocking the container's cleanup.
            LOG(ERROR) << "Sending SIGKILL to I/O switchboard server (pid: "
                       << pid.get() << ") for container " << containerId
                       << " since the I/O switchboard server did not terminate "
                       << "60 seconds after SIGTERM was sent to it";

            os::kill(pid.get(), SIGKILL);
          }
        });
      }
    });
  }

  // NOTE: We use 'await' here so that we can handle the FAILED and
  // DISCARDED cases as well.
  return await(vector<Future<Option<int>>>{status}).then(
      defer(self(), [this, containerId]() -> Future<Nothing> {
        // We need to call `_extractContainerIO` here in case the
        // `IOSwitchboard` still holds a reference to the container's
        // `ContainerIO` struct. We don't care about its value at this
        // point. We just need to extract it out of the hashmap (if
        // it's in there) so it can drop out of scope and all open
        // file descriptors will be closed.
        _extractContainerIO(containerId);

        // We only remove the 'containerId from our info struct once
        // we are sure that the I/O switchboard has shutdown. If we
        // removed it any earlier, attempts to connect to the I/O
        // switchboard would fail.
        //
        // NOTE: One caveat of this approach is that this lambda will
        // be invoked multiple times if `cleanup()` is called multiple
        // times before the first instance of it is triggered. This is
        // OK for now because the logic below has no side effects. If
        // the logic below gets more complicated, we may need to
        // revisit this approach.
        infos.erase(containerId);

        // Best effort removal of the unix domain socket file created for
        // this container's `IOSwitchboardServer`. If it hasn't been
        // checkpointed yet, or the socket file itself hasn't been created,
        // we simply continue without error.
        //
        // NOTE: As the I/O switchboard creates a unix domain socket using
        // a provisional address before initialiazing and renaming it, we assume
        // that the absence of the unix socket at the original address means
        // that the the I/O switchboard has been terminated before renaming.
        Result<unix::Address> address = getContainerIOSwitchboardAddress(
            flags.runtime_dir, containerId);

        const string socketPath = address.isSome()
          ? address->path()
          : getContainerIOSwitchboardSocketProvisionalPath(
                flags.runtime_dir, containerId);

        Try<Nothing> rm = os::rm(socketPath);
        if (rm.isError()) {
          LOG(ERROR) << "Failed to remove unix domain socket file"
                     << " '" << socketPath << "' for container"
                     << " '" << containerId << "': " << rm.error();
        }

        return Nothing();
      }));
#endif // __WINDOWS__
}


bool IOSwitchboard::requiresServer(const ContainerConfig& containerConfig)
{
  if (containerConfig.has_container_info() &&
      containerConfig.container_info().has_tty_info()) {
    return true;
  }

  if (containerConfig.has_container_class() &&
      containerConfig.container_class() ==
        mesos::slave::ContainerClass::DEBUG) {
    return true;
  }

  return false;
}


#ifndef __WINDOWS__
void IOSwitchboard::reaped(
    const ContainerID& containerId,
    const Future<Option<int>>& future)
{
  // NOTE: If reaping of the server process failed, we simply
  // return here because it is unknown to us whether we should
  // destroy the container or not.
  if (!future.isReady()) {
    LOG(ERROR) << "Failed to reap the I/O switchboard server: "
               << (future.isFailed() ? future.failure() : "discarded");
    return;
  }

  const Option<int>& status = future.get();

  // No need to do anything if the I/O switchboard server terminates
  // normally, or its terminal status is unknown. Only initiate the
  // destroy of the container if we know for sure that the I/O
  // switchboard server terminates unexpectedly.
  if (status.isNone()) {
    LOG(INFO) << "I/O switchboard server process for container "
              << containerId << " has terminated (status=N/A)";
    return;
  } else if (WSUCCEEDED(status.get())) {
    LOG(INFO) << "I/O switchboard server process for container "
              << containerId << " has terminated (status=0)";
    return;
  }

  // No need to proceed if the container has or is being destroyed.
  if (!infos.contains(containerId)) {
    return;
  }

  ContainerLimitation limitation;
  limitation.set_reason(TaskStatus::REASON_IO_SWITCHBOARD_EXITED);
  limitation.set_message("'IOSwitchboard' " + WSTRINGIFY(status.get()));

  infos[containerId]->limitation.set(limitation);

  LOG(ERROR) << "Unexpected termination of I/O switchboard server: "
             << limitation.message() << " for container " << containerId;
}


const char IOSwitchboardServer::NAME[] = "mesos-io-switchboard";


class IOSwitchboardServerProcess : public Process<IOSwitchboardServerProcess>
{
public:
  IOSwitchboardServerProcess(
      bool _tty,
      int _stdinToFd,
      int _stdoutFromFd,
      int _stdoutToFd,
      int _stderrFromFd,
      int _stderrToFd,
      const unix::Socket& _socket,
      bool waitForConnection,
      Option<Duration> heartbeatInterval);

  void finalize() override;

  Future<Nothing> run();

  Future<Nothing> unblock();

private:
  // TODO(bmahler): Replace this with the common StreamingHttpConnection.
  class HttpConnection
  {
  public:
    HttpConnection(
        const http::Pipe::Writer& _writer,
        const ContentType& _contentType)
      : writer(_writer),
        contentType(_contentType) {}

    bool send(const agent::ProcessIO& message)
    {
      string record = serialize(contentType, message);

      return writer.write(::recordio::encode(record));
    }

    bool close()
    {
      return writer.close();
    }

    process::Future<Nothing> closed() const
    {
      return writer.readerClosed();
    }

  private:
    http::Pipe::Writer writer;
    ContentType contentType;
  };

  // Sit in a heartbeat loop forever.
  void heartbeatLoop();

  // Sit in an accept loop forever.
  void acceptLoop();

  // Parse the request and look for `ATTACH_CONTAINER_INPUT` and
  // `ATTACH_CONTAINER_OUTPUT` calls. We call their corresponding
  // handler functions once we have parsed them. We accept calls as
  // both `APPLICATION_PROTOBUF` and `APPLICATION_JSON` and respond
  // with the same format we receive them in.
  Future<http::Response> handler(const http::Request& request);

  // Validate `ATTACH_CONTAINER_INPUT` calls.
  //
  // TODO(klueska): Move this to `src/slave/validation.hpp` and make
  // the agent validate all the calls before forwarding them to the
  // switchboard.
  Option<Error> validate(const agent::Call::AttachContainerInput& call);

  // Handle acknowledgment for `ATTACH_CONTAINER_INPUT` call.
  Future<http::Response> acknowledgeContainerInputResponse();

  // Handle `ATTACH_CONTAINER_INPUT` calls.
  Future<http::Response> attachContainerInput(
      const Owned<recordio::Reader<agent::Call>>& reader);

  // Handle `ATTACH_CONTAINER_OUTPUT` calls.
  Future<http::Response> attachContainerOutput(
      ContentType acceptType,
      Option<ContentType> messageAcceptType);

  // Asynchronously receive data as we read it from our
  // `stdoutFromFd` and `stderrFromFd` file descriptors.
  void outputHook(
      const string& data,
      const agent::ProcessIO::Data::Type& type);

  bool tty;
  int stdinToFd;
  int stdoutFromFd;
  int stdoutToFd;
  int stderrFromFd;
  int stderrToFd;
  unix::Socket socket;
  bool waitForConnection;
  Option<Duration> heartbeatInterval;
  bool inputConnected;
  // Each time the agent receives a response for `ATTACH_CONTAINER_INPUT`
  // request it sends an acknowledgment. This counter is used to delay
  // IOSwitchboard termination until all acknowledgments are received.
  size_t numPendingAcknowledgments;
  Future<unix::Socket> accept;
  Promise<Nothing> promise;
  Promise<Nothing> startRedirect;
  // Set when both stdout and stderr redirects finish.
  Promise<http::Response> redirectFinished;
  // The following must be a `std::list`
  // for proper erase semantics later on.
  list<HttpConnection> outputConnections;
  Option<Failure> failure;
};


Try<Owned<IOSwitchboardServer>> IOSwitchboardServer::create(
    bool tty,
    int stdinToFd,
    int stdoutFromFd,
    int stdoutToFd,
    int stderrFromFd,
    int stderrToFd,
    const string& socketPath,
    bool waitForConnection,
    Option<Duration> heartbeatInterval)
{
  Try<unix::Socket> socket = unix::Socket::create(SocketImpl::Kind::POLL);
  if (socket.isError()) {
    return Error("Failed to create socket: " + socket.error());
  }

  // Agent connects to the switchboard once it sees a unix socket. However,
  // the unix socket is not ready to accept connections until `listen()` has
  // been called. Therefore we initialize a unix socket using a provisional path
  // and rename it after `listen()` has been called.
  const string socketProvisionalPath =
      getContainerIOSwitchboardSocketProvisionalPath(socketPath);

  Try<unix::Address> address = unix::Address::create(socketProvisionalPath);
  if (address.isError()) {
    return Error("Failed to build address from '" + socketProvisionalPath + "':"
                 " " + address.error());
  }

  Try<unix::Address> bind = socket->bind(address.get());
  if (bind.isError()) {
    return Error("Failed to bind to address '" + socketProvisionalPath + "':"
                 " " + bind.error());
  }

  Try<Nothing> listen = socket->listen(64);
  if (listen.isError()) {
    return Error("Failed to listen on socket at address"
                 " '" + socketProvisionalPath + "': " + listen.error());
  }

  Try<Nothing> renameSocket = os::rename(socketProvisionalPath, socketPath);
  if (renameSocket.isError()) {
    return Error("Failed to rename socket from '" + socketProvisionalPath + "'"
                 " to '" + socketPath + "': " + renameSocket.error());
  }

  return new IOSwitchboardServer(
      tty,
      stdinToFd,
      stdoutFromFd,
      stdoutToFd,
      stderrFromFd,
      stderrToFd,
      socket.get(),
      waitForConnection,
      heartbeatInterval);
}


IOSwitchboardServer::IOSwitchboardServer(
    bool tty,
    int stdinToFd,
    int stdoutFromFd,
    int stdoutToFd,
    int stderrFromFd,
    int stderrToFd,
    const unix::Socket& socket,
    bool waitForConnection,
    Option<Duration> heartbeatInterval)
  : process(new IOSwitchboardServerProcess(
        tty,
        stdinToFd,
        stdoutFromFd,
        stdoutToFd,
        stderrFromFd,
        stderrToFd,
        socket,
        waitForConnection,
        heartbeatInterval))
{
  spawn(process.get());
}


IOSwitchboardServer::~IOSwitchboardServer()
{
  terminate(process.get());
  process::wait(process.get());
}


Future<Nothing> IOSwitchboardServer::run()
{
  return dispatch(process.get(), &IOSwitchboardServerProcess::run);
}


Future<Nothing> IOSwitchboardServer::unblock()
{
  return dispatch(process.get(), &IOSwitchboardServerProcess::unblock);
}


IOSwitchboardServerProcess::IOSwitchboardServerProcess(
    bool _tty,
    int _stdinToFd,
    int _stdoutFromFd,
    int _stdoutToFd,
    int _stderrFromFd,
    int _stderrToFd,
    const unix::Socket& _socket,
    bool _waitForConnection,
    Option<Duration> _heartbeatInterval)
  : tty(_tty),
    stdinToFd(_stdinToFd),
    stdoutFromFd(_stdoutFromFd),
    stdoutToFd(_stdoutToFd),
    stderrFromFd(_stderrFromFd),
    stderrToFd(_stderrToFd),
    socket(_socket),
    waitForConnection(_waitForConnection),
    heartbeatInterval(_heartbeatInterval),
    inputConnected(false),
    numPendingAcknowledgments(0) {}


Future<Nothing> IOSwitchboardServerProcess::run()
{
  if (!waitForConnection) {
    startRedirect.set(Nothing());
  }

  startRedirect.future()
    .then(defer(self(), [this]() {
      Future<Nothing> stdoutRedirect = process::io::redirect(
          stdoutFromFd,
          stdoutToFd,
          process::io::BUFFERED_READ_SIZE,
          {defer(self(),
                 &Self::outputHook,
                 lambda::_1,
                 agent::ProcessIO::Data::STDOUT)});

      // NOTE: We don't need to redirect stderr if TTY is enabled. If
      // TTY is enabled for the container, stdout and stderr for the
      // container will be redirected to the slave end of the pseudo
      // terminal device. Both stdout and stderr of the container will
      // both coming out from the master end of the pseudo terminal.
      Future<Nothing> stderrRedirect;
      if (tty) {
        stderrRedirect = Nothing();
      } else {
        stderrRedirect = process::io::redirect(
            stderrFromFd,
            stderrToFd,
            process::io::BUFFERED_READ_SIZE,
            {defer(self(),
                   &Self::outputHook,
                   lambda::_1,
                   agent::ProcessIO::Data::STDERR)});
      }

      // Set the future once our IO redirects finish. On failure,
      // fail the future.
      //
      // For now we simply assume that whenever both `stdoutRedirect`
      // and `stderrRedirect` have completed while there is no input
      // connected then it is OK to exit the switchboard process.
      // We assume this because `stdoutRedirect` and `stderrRedirect`
      // will only complete after both the read end of the `stdout`
      // stream and the read end of the `stderr` stream have been
      // drained. Since draining these `fds` represents having
      // read everything possible from a container's `stdout` and
      // `stderr` this is likely sufficient termination criteria.
      // However, there's a non-zero chance that *some* containers may
      // decide to close their `stdout` and `stderr` while expecting to
      // continue reading from `stdin`. For now we don't support
      // containers with this behavior and we will exit out of the
      // switchboard process early.
      //
      // If our IO redirects are finished and there are pending
      // acknowledgments for `ATTACH_CONTAINER_INPUT` requests, then
      // we set `redirectFinished` promise which triggers a callback for
      // `attachContainerInput()`. This callback returns a final `HTTP 200`
      // response to the client, even if the client has not yet sent the EOF
      // message.
      //
      // NOTE: We always call `terminate()` with `false` to ensure
      // that our event queue is drained before actually terminating.
      // Without this, it's possible that we might drop some data we
      // are trying to write out over any open connections we have.
      //
      // TODO(klueska): Add support to asynchronously detect when
      // `stdinToFd` has become invalid before deciding to terminate.
      stdoutRedirect
        .onFailed(defer(self(), [this](const string& message) {
           failure = Failure("Failed redirecting stdout: " + message);
           terminate(self(), false);
        }))
        .onDiscarded(defer(self(), [this]() {
           failure = Failure("Redirecting stdout discarded");
           terminate(self(), false);
        }));

      stderrRedirect
        .onFailed(defer(self(), [this](const string& message) {
           failure = Failure("Failed redirecting stderr: " + message);
           terminate(self(), false);
        }))
        .onDiscarded(defer(self(), [this]() {
           failure = Failure("Redirecting stderr discarded");
           terminate(self(), false);
        }));

      collect(stdoutRedirect, stderrRedirect)
        .then(defer(self(), [this]() {
          if (numPendingAcknowledgments > 0) {
            redirectFinished.set(http::OK());
          } else {
            terminate(self(), false);
          }
          return Nothing();
        }));

      return Nothing();
    }));

  // If we have a heartbeat interval set, send a heartbeat to all of
  // our outstanding output connections at the proper interval.
  if (heartbeatInterval.isSome()) {
    heartbeatLoop();
  }

  acceptLoop();

  return promise.future();
}


Future<Nothing> IOSwitchboardServerProcess::unblock()
{
  startRedirect.set(Nothing());
  return Nothing();
}


void IOSwitchboardServerProcess::finalize()
{
  // Discard the server socket's `accept` future so that we do not
  // maintain a reference to the socket, which would cause a leak.
  accept.discard();

  foreach (HttpConnection& connection, outputConnections) {
    connection.close();

    // It is possible that the read end of the pipe has not yet
    // finished processing the data. We wait here for the reader
    // to signal that is has finished reading.
    connection.closed().await();
  }

  if (failure.isSome()) {
    promise.fail(failure->message);
  } else {
    promise.set(Nothing());
  }
}


void IOSwitchboardServerProcess::heartbeatLoop()
{
  CHECK(heartbeatInterval.isSome());

  agent::ProcessIO message;
  message.set_type(agent::ProcessIO::CONTROL);
  message.mutable_control()->set_type(
      agent::ProcessIO::Control::HEARTBEAT);
  message.mutable_control()
    ->mutable_heartbeat()
    ->mutable_interval()
    ->set_nanoseconds(heartbeatInterval->ns());

  foreach (HttpConnection& connection, outputConnections) {
    connection.send(message);
  }

  // Dispatch back to ourselves after the `heartbeatInterval`.
  delay(heartbeatInterval.get(),
        self(),
        &IOSwitchboardServerProcess::heartbeatLoop);
}


void IOSwitchboardServerProcess::acceptLoop()
{
  // Store the server socket's `accept` future so that we can discard
  // it during process finalization. Otherwise, we would maintain a
  // reference to the socket, causing a leak.
  accept = socket.accept()
    .onAny(defer(self(), [this](const Future<unix::Socket>& socket) {
      if (!socket.isReady()) {
        failure = Failure("Failed trying to accept connection");
        terminate(self(), false);
        return;
      }

      // We intentionally ignore errors on the serve path, and assume
      // that they will eventually be propagated back to the client in
      // one form or another (e.g. a timeout on the client side). We
      // explicitly *don't* want to kill the whole server though, just
      // beause a single connection fails.
      http::serve(
          socket.get(),
          defer(self(), &Self::handler, lambda::_1));

      // Use `dispatch` to limit the size of the call stack.
      dispatch(self(), &Self::acceptLoop);
    }));
}


Future<http::Response> IOSwitchboardServerProcess::handler(
    const http::Request& request)
{
  CHECK_EQ("POST", request.method);

  if (request.url.path == "/acknowledge_container_input_response") {
    return acknowledgeContainerInputResponse();
  }

  Option<string> contentType_ = request.headers.get("Content-Type");
  CHECK_SOME(contentType_);

  ContentType contentType;
  if (contentType_.get() == APPLICATION_JSON) {
    contentType = ContentType::JSON;
  } else if (contentType_.get() == APPLICATION_PROTOBUF) {
    contentType = ContentType::PROTOBUF;
  } else if (contentType_.get() == APPLICATION_RECORDIO) {
    contentType = ContentType::RECORDIO;
  } else {
    LOG(FATAL) << "Unexpected 'Content-Type' header: " << contentType_.get();
  }

  Option<ContentType> messageContentType;
  Option<string> messageContentType_ =
    request.headers.get(MESSAGE_CONTENT_TYPE);

  if (streamingMediaType(contentType)) {
    if (messageContentType_.isNone()) {
      return http::BadRequest(
          "Expecting '" + stringify(MESSAGE_CONTENT_TYPE) + "' to be" +
          " set for streaming requests");
    }

    if (messageContentType_.get() == APPLICATION_JSON) {
      messageContentType = Option<ContentType>(ContentType::JSON);
    } else if (messageContentType_.get() == APPLICATION_PROTOBUF) {
      messageContentType = Option<ContentType>(ContentType::PROTOBUF);
    } else {
      return http::UnsupportedMediaType(
          string("Expecting '") + MESSAGE_CONTENT_TYPE + "' of " +
          APPLICATION_JSON + " or " + APPLICATION_PROTOBUF);
    }
  } else {
    // The 'Message-Content-Type' header should not be set
    // for non-streaming requests.
    CHECK_NONE(messageContentType);
  }

  ContentType acceptType;
  if (request.acceptsMediaType(APPLICATION_JSON)) {
    acceptType = ContentType::JSON;
  } else if (request.acceptsMediaType(APPLICATION_PROTOBUF)) {
    acceptType = ContentType::PROTOBUF;
  } else if (request.acceptsMediaType(APPLICATION_RECORDIO)) {
    acceptType = ContentType::RECORDIO;
  } else {
    Option<string> acceptType_ = request.headers.get("Accept");
    CHECK_SOME(acceptType_);

    LOG(FATAL) << "Unexpected 'Accept' header: " << acceptType_.get();
  }

  Option<ContentType> messageAcceptType;
  if (streamingMediaType(acceptType)) {
    if (request.acceptsMediaType(MESSAGE_ACCEPT, APPLICATION_JSON)) {
      messageAcceptType = ContentType::JSON;
    } else if (request.acceptsMediaType(MESSAGE_ACCEPT, APPLICATION_PROTOBUF)) {
      messageAcceptType = ContentType::PROTOBUF;
    } else {
      Option<string> messageAcceptType_ = request.headers.get(MESSAGE_ACCEPT);
      CHECK_SOME(messageAcceptType_);

      LOG(FATAL) << "Unexpected '" << MESSAGE_ACCEPT << "' header: "
                 << messageAcceptType_.get();
    }
  } else {
    // The 'Message-Accept' header should not be set
    // for a non-streaming response.
    CHECK_NONE(request.headers.get(MESSAGE_ACCEPT));
  }

  CHECK_EQ(http::Request::PIPE, request.type);
  CHECK_SOME(request.reader);

  if (streamingMediaType(contentType)) {
    CHECK_EQ(ContentType::RECORDIO, contentType);
    CHECK_SOME(messageContentType);

    Owned<recordio::Reader<agent::Call>> reader(
        new recordio::Reader<agent::Call>(
            lambda::bind(
                deserialize<agent::Call>,
                messageContentType.get(),
                lambda::_1),
            request.reader.get()));

    return reader->read()
      .then(defer(
          self(),
          [=](const Result<agent::Call>& call) -> Future<http::Response> {
            if (call.isNone()) {
              return http::BadRequest(
                  "IOSwitchboard received EOF while reading request body");
            }

            if (call.isError()) {
              return Failure(call.error());
            }

            // Should have already been validated by the agent.
            CHECK(call->has_type());
            CHECK_EQ(agent::Call::ATTACH_CONTAINER_INPUT, call->type());
            CHECK(call->has_attach_container_input());
            CHECK_EQ(mesos::agent::Call::AttachContainerInput::CONTAINER_ID,
                     call->attach_container_input().type());
            CHECK(call->attach_container_input().has_container_id());
            CHECK(call->attach_container_input().container_id().has_value());

            return attachContainerInput(reader);
          }));
  } else {
    http::Pipe::Reader reader = request.reader.get();  // Remove const.

    return reader.readAll()
      .then(defer(
          self(),
          [=](const string& body) -> Future<http::Response> {
            Try<agent::Call> call = deserialize<agent::Call>(contentType, body);
            if (call.isError()) {
              return http::BadRequest(call.error());
            }

            // Should have already been validated by the agent.
            CHECK(call->has_type());
            CHECK_EQ(agent::Call::ATTACH_CONTAINER_OUTPUT, call->type());

            return attachContainerOutput(acceptType, messageAcceptType);
          }));
  }
}


Option<Error> IOSwitchboardServerProcess::validate(
    const agent::Call::AttachContainerInput& call)
{
  switch (call.type()) {
    case agent::Call::AttachContainerInput::UNKNOWN:
    case agent::Call::AttachContainerInput::CONTAINER_ID: {
        return Error(
            "Expecting 'attach_container_input.type' to be 'PROCESS_IO'"
             " instead of: '" + stringify(call.type()) + "'");
    }
    case agent::Call::AttachContainerInput::PROCESS_IO: {
      if (!call.has_process_io()) {
        return Error(
            "Expecting 'attach_container_input.process_io' to be present");
      }

      const agent::ProcessIO& message = call.process_io();

      if (!message.has_type()) {
        return Error("Expecting 'process_io.type' to be present");
      }

      switch (message.type()) {
        case agent::ProcessIO::UNKNOWN: {
          return Error("'process_io.type' is unknown");
        }
        case agent::ProcessIO::CONTROL: {
          if (!message.has_control()) {
            return Error("Expecting 'process_io.control' to be present");
          }

          if (!message.control().has_type()) {
            return Error("Expecting 'process_io.control.type' to be present");
          }

          switch (message.control().type()) {
            case agent::ProcessIO::Control::UNKNOWN: {
              return Error("'process_io.control.type' is unknown");
            }
            case agent::ProcessIO::Control::TTY_INFO: {
              if (!message.control().has_tty_info()) {
                return Error(
                    "Expecting 'process_io.control.tty_info' to be present");
              }

              const TTYInfo& ttyInfo = message.control().tty_info();

              if (!ttyInfo.has_window_size()) {
                return Error("Expecting 'tty_info.window_size' to be present");
              }

              return None();
            }
            case agent::ProcessIO::Control::HEARTBEAT: {
              if (!message.control().has_heartbeat()) {
                return Error(
                    "Expecting 'process_io.control.heartbeat' to be present");
              }

              return None();
            }
          }

          UNREACHABLE();
        }
        case agent::ProcessIO::DATA: {
          if (!message.has_data()) {
            return Error("Expecting 'process_io.data' to be present");
          }

          if (!message.data().has_type()) {
            return Error("Expecting 'process_io.data.type' to be present");
          }

          if (message.data().type() != agent::ProcessIO::Data::STDIN) {
            return Error("Expecting 'process_io.data.type' to be 'STDIN'");
          }

          if (!message.data().has_data()) {
            return Error("Expecting 'process_io.data.data' to be present");
          }

          return None();
        }
      }
    }
  }

  UNREACHABLE();
}


Future<http::Response>
IOSwitchboardServerProcess::acknowledgeContainerInputResponse()
{
  // Check if this is an acknowledgment sent by the agent. This acknowledgment
  // means that response for `ATTACH_CONTAINER_INPUT` call has been received by
  // the agent.
  CHECK_GT(numPendingAcknowledgments, 0u);
  if (--numPendingAcknowledgments == 0) {
    // If IO redirects are finished or writing to `stdin` failed we want to
    // terminate ourselves (after flushing any outstanding messages from our
    // message queue).
    if (!redirectFinished.future().isPending() || failure.isSome()) {
      terminate(self(), false);
    }
  }
  return http::OK();
}


Future<http::Response> IOSwitchboardServerProcess::attachContainerInput(
    const Owned<recordio::Reader<agent::Call>>& reader)
{
  ++numPendingAcknowledgments;

  // Only allow a single input connection at a time.
  if (inputConnected) {
    return http::Conflict("Multiple input connections are not allowed");
  }

  // We set `inputConnected` to true here and then reset it to false
  // at the bottom of this function once our asynchronous loop has
  // terminated. This way another connection can be established once
  // the current one is complete.
  inputConnected = true;

  // Loop through each record and process it. Return a proper
  // response once the last record has been fully processed.
  auto readLoop = loop(
      self(),
      [=]() {
        return reader->read();
      },
      [=](const Result<agent::Call>& record)
          -> Future<ControlFlow<http::Response>> {
        if (record.isNone()) {
          return Break(http::OK());
        }

        if (record.isError()) {
          return Break(http::BadRequest(record.error()));
        }

        // Should have already been validated by the agent.
        CHECK(record->has_type());
        CHECK_EQ(mesos::agent::Call::ATTACH_CONTAINER_INPUT, record->type());
        CHECK(record->has_attach_container_input());

        // Validate the rest of the `AttachContainerInput` message.
        Option<Error> error = validate(record->attach_container_input());
        if (error.isSome()) {
          return Break(http::BadRequest(error->message));
        }

        const agent::ProcessIO& message =
          record->attach_container_input().process_io();

        switch (message.type()) {
          case agent::ProcessIO::CONTROL: {
            switch (message.control().type()) {
              case agent::ProcessIO::Control::TTY_INFO: {
                // TODO(klueska): Return a failure if the container we are
                // attaching to does not have a tty associated with it.

                // Update the window size.
                Try<Nothing> window = os::setWindowSize(
                    stdinToFd,
                    message.control().tty_info().window_size().rows(),
                    message.control().tty_info().window_size().columns());

                if (window.isError()) {
                  return Break(http::BadRequest(
                      "Unable to set the window size: " + window.error()));
                }

                return Continue();
              }
              case agent::ProcessIO::Control::HEARTBEAT: {
                // For now, we ignore any interval information
                // sent along with the heartbeat.
                return Continue();
              }
              default: {
                UNREACHABLE();
              }
            }
          }
          case agent::ProcessIO::DATA: {
            // Receiving a `DATA` message with length 0 indicates
            // `EOF`, so we should close `stdinToFd` if there is no tty.
            // If tty is enabled, the client is expected to send `EOT` instead.
            if (!tty && message.data().data().length() == 0) {
              os::close(stdinToFd);
              return Continue();
            }

            // Write the STDIN data to `stdinToFd`. If there is a
            // failure, we set the `failure` member variable and exit
            // the loop. In the resulting `.then()` callback, we then
            // terminate the process. We don't terminate the process
            // here because we want to propagate an `InternalServerError`
            // back to the client.
            return process::io::write(stdinToFd, message.data().data())
              .then(defer(self(), [=](const Nothing&)
                  -> ControlFlow<http::Response> {
                return Continue();
              }))
              .recover(defer(self(), [=](
                  const Future<ControlFlow<http::Response>>& future)
                  -> ControlFlow<http::Response> {
                failure = Failure(
                    "Failed writing to stdin: " + stringify(future));
                return Break(http::InternalServerError(failure->message));
              }));
          }
          default: {
            UNREACHABLE();
          }
        }
      });

  // We create a new promise, which is transitioned to `READY` when either
  // the read loop finishes or IO redirects finish. Once this promise is set,
  // we return a final response to the client.
  //
  // We use `defer(self(), ...)` to use this process as a synchronization point
  // when changing state of the promise.
  Owned<Promise<http::Response>> promise(new Promise<http::Response>());

  readLoop.onAny(
      defer(self(), [promise](const Future<http::Response>& response) {
        promise->set(response);
      }));

  // Since IOSwitchboard might receive an acknowledgment for the
  // `ATTACH_CONTAINER_INPUT` request before reading a final message from
  // the corresponding connection, we need to give IOSwitchboard a chance to
  // read the final message. Otherwise, the agent might get `HTTP 500`
  // "broken pipe" while attempting to write the final message.
  redirectFinished.future().onAny(
      defer(self(), [=](const Future<http::Response>& response) {
        // TODO(abudnik): Ideally, we would have used `process::delay()` to
        // delay a dispatch of the lambda to this process.
        after(Seconds(1))
          .onAny(defer(self(), [promise, response](const Future<Nothing>&) {
            promise->set(response);
          }));
      }));

  // We explicitly specify the return type to avoid a type deduction
  // issue in some versions of clang. See MESOS-2943.
  return promise->future().then(
      defer(self(), [=](const http::Response& response) -> http::Response {
        // Reset `inputConnected` to allow future input connections.
        inputConnected = false;

        return response;
      }));
}


Future<http::Response> IOSwitchboardServerProcess::attachContainerOutput(
    ContentType acceptType,
    Option<ContentType> messageAcceptType)
{
  http::Pipe pipe;
  http::OK ok;

  ok.headers["Content-Type"] = stringify(acceptType);

  // If a client sets the 'Accept' header expecting a streaming response,
  // `messageAcceptType` would always be set and we use it as the value of
  // 'Message-Content-Type' response header.
  ContentType messageContentType = acceptType;
  if (streamingMediaType(acceptType)) {
    CHECK_SOME(messageAcceptType);
    ok.headers[MESSAGE_CONTENT_TYPE] = stringify(messageAcceptType.get());
    messageContentType = messageAcceptType.get();
  }

  ok.type = http::Response::PIPE;
  ok.reader = pipe.reader();

  // We store the connection in a list and wait for asynchronous
  // calls to `receiveOutput()` to actually push data out over the
  // connection. If we ever detect a connection has been closed,
  // we remove it from this list.
  HttpConnection connection(pipe.writer(), messageContentType);
  auto iterator = outputConnections.insert(outputConnections.end(), connection);

  // We use the `startRedirect` promise to indicate when we should
  // begin reading data from our `stdoutFromFd` and `stderrFromFd`
  // file descriptors. If we were started with the `waitForConnection`
  // parameter set to `true`, only set this promise here once the
  // first connection has been established.
  if (!startRedirect.future().isReady()) {
    startRedirect.set(Nothing());
  }

  connection.closed()
    .then(defer(self(), [this, iterator]() {
      // Erasing from a `std::list` only invalidates the iterator of
      // the object being erased. All other iterators remain valid.
      outputConnections.erase(iterator);
      return Nothing();
    }));

  return ok;
}


void IOSwitchboardServerProcess::outputHook(
    const string& data,
    const agent::ProcessIO::Data::Type& type)
{
  // Break early if there are no connections to send the data to.
  if (outputConnections.size() == 0) {
    return;
  }

  // Build a `ProcessIO` message from the data.
  agent::ProcessIO message;
  message.set_type(agent::ProcessIO::DATA);
  message.mutable_data()->set_type(type);
  message.mutable_data()->set_data(data);

  // Walk through our list of connections and write the message to
  // them. It's possible that a write might fail if the writer has
  // been closed. That's OK because we already take care of removing
  // closed connections from our list via the future returned by
  // the `HttpConnection::closed()` call above. We might do a few
  // unnecessary writes if we have a bunch of messages queued up,
  // but that shouldn't be a problem.
  foreach (HttpConnection& connection, outputConnections) {
    connection.send(message);
  }
}
#endif // __WINDOWS__

} // namespace slave {
} // namespace internal {
} // namespace mesos {
