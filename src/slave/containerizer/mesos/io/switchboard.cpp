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

#include <map>
#include <string>
#include <vector>

#include <process/address.hpp>
#include <process/collect.hpp>
#include <process/defer.hpp>
#include <process/dispatch.hpp>
#include <process/future.hpp>
#include <process/http.hpp>
#include <process/io.hpp>
#include <process/owned.hpp>

#include <process/process.hpp>
#include <process/reap.hpp>
#include <process/subprocess.hpp>

#include <stout/hashmap.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>

#include <mesos/type_utils.hpp>

#include <mesos/agent/agent.hpp>

#include <mesos/slave/container_logger.hpp>

#include "slave/flags.hpp"
#include "slave/state.hpp"

#include "slave/containerizer/mesos/paths.hpp"

#include "slave/containerizer/mesos/io/switchboard.hpp"

namespace http = process::http;

#ifndef __WINDOWS__
namespace unix = process::network::unix;
#endif // __WINDOWS__

using std::string;

using process::Failure;
using process::Future;
using process::Owned;
using process::PID;
using process::Process;
using process::Promise;
using process::Subprocess;

using std::map;
using std::string;
using std::vector;

using mesos::slave::ContainerConfig;
using mesos::slave::ContainerIO;
using mesos::slave::ContainerLaunchInfo;
using mesos::slave::ContainerLogger;
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


Future<Option<ContainerLaunchInfo>> IOSwitchboard::prepare(
    const ContainerID& containerId,
    const ContainerConfig& containerConfig)
{
  // In local mode, the container will inherit agent's stdio.
  if (local) {
    return None();
  }

  // TODO(jieyu): Currently, if the agent fails over after the
  // executor is launched, but before its nested containers are
  // launched, the nested containers launched later might not have
  // access to the root parent container's ExecutorInfo (i.e.,
  // 'containerConfig.executor_info()' will be empty).
  return logger->prepare(
      containerConfig.executor_info(),
      containerConfig.directory(),
      containerConfig.has_user()
        ? Option<string>(containerConfig.user())
        : None())
    .then(defer(
        PID<IOSwitchboard>(this),
        &IOSwitchboard::_prepare,
        containerId,
        lambda::_1));
}


Future<Option<ContainerLaunchInfo>> IOSwitchboard::_prepare(
    const ContainerID& containerId,
    const ContainerLogger::SubprocessInfo& loggerInfo)
{
  // On windows, we do not yet support running an io switchboard
  // server, so we must error out if the agent
  // `io_switchboard_enable_server` flag is enabled.
#ifdef __WINDOWS__
  if (flags.io_switchboard_enable_server) {
      return Failure(
          "Setting the agent flag"
          " '--io_switchboard_enable_server=true'"
          " is not supported on windows");
  }
#endif

  if (!flags.io_switchboard_enable_server) {
    ContainerLaunchInfo launchInfo;

    ContainerIO* out = launchInfo.mutable_out();
    ContainerIO* err = launchInfo.mutable_err();

    switch (loggerInfo.out.type()) {
      case ContainerLogger::SubprocessInfo::IO::Type::FD:
        out->set_type(ContainerIO::FD);
        out->set_fd(loggerInfo.out.fd().get());
        break;
      case ContainerLogger::SubprocessInfo::IO::Type::PATH:
        out->set_type(ContainerIO::PATH);
        out->set_path(loggerInfo.out.path().get());
        break;
      default:
        UNREACHABLE();
    }

    switch (loggerInfo.err.type()) {
      case ContainerLogger::SubprocessInfo::IO::Type::FD:
        err->set_type(ContainerIO::FD);
        err->set_fd(loggerInfo.err.fd().get());
        break;
      case ContainerLogger::SubprocessInfo::IO::Type::PATH:
        err->set_type(ContainerIO::PATH);
        err->set_path(loggerInfo.err.path().get());
        break;
      default:
        UNREACHABLE();
    }

    return launchInfo;
  }

#ifndef __WINDOWS__
  // First make sure that we haven't already spawned an io
  // switchboard server for this container.
  if (infos.contains(containerId)) {
    return Failure("Already prepared io switchboard server for container"
                   " '" + stringify(containerId) + "'");
  }

  // Manually construct pipes instead of using `Subprocess::PIPE`
  // so that the ownership of the FDs is properly represented. The
  // `Subprocess` spawned below owns one end of each pipe and will
  // be solely responsible for closing that end. The ownership of
  // the other end will be passed to the caller of this function
  // and eventually passed to the container being launched.
  int infds[2];
  int outfds[2];
  int errfds[2];

  // A list of file decriptors we've opened so far.
  vector<int> fds = {};

  // Helper for closing the list of file
  // descriptors we've opened so far.
  auto close = [](const vector<int>& fds) {
    foreach (int fd, fds) {
      os::close(fd);
    }
  };

  Try<Nothing> pipe = os::pipe(infds);
  if (pipe.isError()) {
    close(fds);
    return Failure("Failed to create stdin pipe: " + pipe.error());
  }

  fds.push_back(infds[0]);
  fds.push_back(infds[1]);

  pipe = os::pipe(outfds);
  if (pipe.isError()) {
    close(fds);
    return Failure("Failed to create stdout pipe: " + pipe.error());
  }

  fds.push_back(outfds[0]);
  fds.push_back(outfds[1]);

  pipe = os::pipe(errfds);
  if (pipe.isError()) {
    close(fds);
    return Failure("Failed to create stderr pipe: " + pipe.error());
  }

  fds.push_back(errfds[0]);
  fds.push_back(errfds[1]);

  Try<Nothing> cloexec = os::cloexec(infds[0]);
  if (cloexec.isError()) {
    close(fds);
    return Failure("Failed to cloexec infds.read: " + cloexec.error());
  }

  cloexec = os::cloexec(outfds[1]);
  if (cloexec.isError()) {
    close(fds);
    return Failure("Failed to cloexec outfds.write: " + cloexec.error());
  }

  cloexec = os::cloexec(errfds[1]);
  if (cloexec.isError()) {
    close(fds);
    return Failure("Failed to cloexec errfds.write: " + cloexec.error());
  }

  // Set up our flags to send to the io switchboard server process.
  IOSwitchboardServerFlags switchboardFlags;
  switchboardFlags.stdin_to_fd = infds[1];
  switchboardFlags.stdout_from_fd = outfds[0];
  switchboardFlags.stdout_to_fd = STDOUT_FILENO;
  switchboardFlags.stderr_from_fd = errfds[0];
  switchboardFlags.stderr_to_fd = STDERR_FILENO;
  switchboardFlags.socket_path = path::join(
      stringify(os::PATH_SEPARATOR),
      "tmp",
      "mesos-io-switchboard-" + UUID::random().toString());

  // Launch the io switchboard server process.
  // We `dup()` the `stdout` and `stderr` passed to us by the
  // container logger over the `stdout` and `stderr` of the io
  // switchboard process itself. In this way, the io switchboard
  // process simply needs to write to its own `stdout` and
  // `stderr` in order to send output to the logger files.
  Try<Subprocess> child = subprocess(
      path::join(flags.launcher_dir, IOSwitchboardServer::NAME),
      {IOSwitchboardServer::NAME},
      Subprocess::PATH("/dev/null"),
      loggerInfo.out,
      loggerInfo.err,
      &switchboardFlags,
      map<string, string>(),
      None(),
      {},
      {Subprocess::ChildHook::SETSID()});

  if (child.isError()) {
    close(fds);
    return Failure("Failed to create io switchboard"
                   " server process: " + child.error());
  }

  os::close(infds[1]);
  os::close(outfds[0]);
  os::close(errfds[0]);

  // Now that the child has come up, we checkpoint the socket
  // address we told it to bind to so we can access it later.
  const string path =
    containerizer::paths::getContainerIOSwitchboardSocketPath(
        flags.runtime_dir, containerId);

  Try<Nothing> checkpointed = slave::state::checkpoint(
      path, switchboardFlags.socket_path);

  if (checkpointed.isError()) {
    close(fds);
    return Failure("Failed to checkpoint container's socket path to"
                   " '" + path + "': " + checkpointed.error());
  }

  // Build an info struct for this container.
  infos[containerId] = Owned<Info>(new Info(
    child->pid(),
    process::reap(child->pid())));

  // Return the set of fds that should be sent to the
  // container and dup'd onto its stdin/stdout/stderr.
  ContainerLaunchInfo launchInfo;

  launchInfo.mutable_in()->set_type(ContainerIO::FD);
  launchInfo.mutable_in()->set_fd(infds[0]);

  launchInfo.mutable_out()->set_type(ContainerIO::FD);
  launchInfo.mutable_out()->set_fd(outfds[1]);

  launchInfo.mutable_err()->set_type(ContainerIO::FD);
  launchInfo.mutable_err()->set_fd(errfds[1]);

  return launchInfo;
#endif // __WINDOWS__
}


Future<http::Connection> IOSwitchboard::connect(
    const ContainerID& containerId)
{
#ifdef __WINDOWS__
  return Failure("Not supported on Windows");
#else
  if (!flags.io_switchboard_enable_server) {
    return Failure("Support for running an io switchboard"
                   " server was disabled by the agent");
  }

  // Get the io switchboard address from the `containerId`.
  //
  // NOTE: We explicitly don't want to check for the existence of
  // `containerId` in our `infos` struct. Otherwise we wouldn't be
  // able to reconnect to the io switchboard after agent restarts.
  Result<unix::Address> address =
    containerizer::paths::getContainerIOSwitchboardAddress(
        flags.runtime_dir, containerId);

  if (!address.isSome()) {
    return Failure("Failed to get the io switchboard address"
                   " " + (address.isError()
                          ? address.error()
                          : "No address found"));
  }

  return http::connect(address.get());
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
  // We don't particularly care if the process gets reaped or not (it
  // will clean itself up automatically upon process exit). We just
  // try to wait for it to exit if we can. For now there is no need
  // to recover info about a container's IOSwitchboard across agent
  // restarts (so it's OK to simply return `Nothing()` if we don't
  // know about a containerId).
  //
  // TODO(klueska): Add the ability to recover the `IOSwitchboard`'s
  // pid and reap it so we can properly return its status here.
  if (local || !infos.contains(containerId)) {
    return Nothing();
  }

  Future<Option<int>> status = infos[containerId]->status;

  infos.erase(containerId);

  return status
    .then([]() { return Nothing(); });
#endif // __WINDOWS__
}


#ifndef __WINDOWS__
constexpr char IOSwitchboardServer::NAME[];


class IOSwitchboardServerProcess : public Process<IOSwitchboardServerProcess>
{
public:
  IOSwitchboardServerProcess(
      int _stdinToFd,
      int _stdoutFromFd,
      int _stdoutToFd,
      int _stderrFromFd,
      int _stderrToFd,
      const unix::Socket& _socket);

  Future<Nothing> run();

private:
  // Sit in an accept loop forever.
  void acceptLoop();

  // Parse the request and look for `ATTACH_CONTAINER_INPUT` and
  // `ATTACH_CONTAINER_OUTPUT` calls. We call their corresponding
  // handler functions once we have parsed them. We accept calls as
  // both `APPLICATION_PROTOBUF` and `APPLICATION_JSON` and respond
  // with the same format we receive them in.
  Future<http::Response> handler(const http::Request& request);

  // Asynchronously receive data as we read it from our
  // `stdoutFromFd` and `stdoutFromFd` file descriptors.
  void outputHook(
      const string& data,
      const agent::ProcessIO::Data::Type& type);

  int stdinToFd;
  int stdoutFromFd;
  int stdoutToFd;
  int stderrFromFd;
  int stderrToFd;
  unix::Socket socket;
  Promise<Nothing> promise;
};


Try<Owned<IOSwitchboardServer>> IOSwitchboardServer::create(
    int stdinToFd,
    int stdoutFromFd,
    int stdoutToFd,
    int stderrFromFd,
    int stderrToFd,
    const string& socketPath)
{
  Try<unix::Socket> socket = unix::Socket::create();
  if (socket.isError()) {
    return Error("Failed to create socket: " + socket.error());
  }

  Try<unix::Address> address = unix::Address::create(socketPath);
  if (address.isError()) {
    return Error("Failed to build address from '" + socketPath + "':"
                 " " + address.error());
  }

  Try<unix::Address> bind = socket->bind(address.get());
  if (bind.isError()) {
    return Error("Failed to bind to address '" + socketPath + "':"
                 " " + bind.error());
  }

  Try<Nothing> listen = socket->listen(64);
  if (listen.isError()) {
    return Error("Failed to listen on socket at address"
                 " '" + socketPath + "': " + listen.error());
  }

  return new IOSwitchboardServer(
      stdinToFd,
      stdoutFromFd,
      stdoutToFd,
      stderrFromFd,
      stderrToFd,
      socket.get());
}


IOSwitchboardServer::IOSwitchboardServer(
    int stdinToFd,
    int stdoutFromFd,
    int stdoutToFd,
    int stderrFromFd,
    int stderrToFd,
    const unix::Socket& socket)
  : process(new IOSwitchboardServerProcess(
        stdinToFd,
        stdoutFromFd,
        stdoutToFd,
        stderrFromFd,
        stderrToFd,
        socket))
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


IOSwitchboardServerProcess::IOSwitchboardServerProcess(
    int _stdinToFd,
    int _stdoutFromFd,
    int _stdoutToFd,
    int _stderrFromFd,
    int _stderrToFd,
    const unix::Socket& _socket)
  : stdinToFd(_stdinToFd),
    stdoutFromFd(_stdoutFromFd),
    stdoutToFd(_stdoutToFd),
    stderrFromFd(_stderrFromFd),
    stderrToFd(_stderrToFd),
    socket(_socket) {}


Future<Nothing> IOSwitchboardServerProcess::run()
{
  Future<Nothing> stdoutRedirect = process::io::redirect(
      stdoutFromFd,
      stdoutToFd,
      4096,
      {defer(self(),
             &Self::outputHook,
             lambda::_1,
             agent::ProcessIO::Data::STDOUT)});

  Future<Nothing> stderrRedirect = process::io::redirect(
      stderrFromFd,
      stderrToFd,
      4096,
      {defer(self(),
             &Self::outputHook,
             lambda::_1,
             agent::ProcessIO::Data::STDERR)});

  // Set the future once our IO redirects finish. On failure,
  // fail the future.
  //
  // For now we simply assume that whenever both `stdoutRedirect`
  // and `stderrRedirect` have completed then it is OK to exit the
  // switchboard process. We assume this because `stdoutRedirect`
  // and `stderrRedirect` will only complete after both the read end
  // of the `stdout` stream and the read end of the `stderr` stream
  // have been drained. Since draining these `fds` represents having
  // read everything possible from a container's `stdout` and
  // `stderr` this is likely sufficient termination criteria.
  // However, there's a non-zero chance that *some* containers may
  // decide to close their `stdout` and `stderr` while expecting to
  // continue reading from `stdin`. For now we don't support
  // containers with this behavior and we will exit out of the
  // switchboard process early.
  //
  // TODO(klueska): Add support to asynchronously detect when
  // `stdinToFd` has become invalid before deciding to terminate.
  stdoutRedirect
    .onFailed(defer(self(), [this](const string& message) {
       promise.fail("Failed redirecting stdout: " + message);
    }));

  stderrRedirect
    .onFailed(defer(self(), [this](const string& message) {
       promise.fail("Failed redirecting stderr: " + message);
    }));

  collect(stdoutRedirect, stderrRedirect)
    .then(defer(self(), [this]() {
      promise.set(Nothing());
      return Nothing();
    }));

  acceptLoop();

  return promise.future();
}


void IOSwitchboardServerProcess::acceptLoop()
{
  socket.accept()
    .onAny(defer(self(), [this](const Future<unix::Socket>& socket) {
      if (!socket.isReady()) {
        promise.fail("Failed trying to accept connection");
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
  return http::BadRequest("Unsupported");
}


void IOSwitchboardServerProcess::outputHook(
    const string& data,
    const agent::ProcessIO::Data::Type& type)
{
}
#endif // __WINDOWS__

} // namespace slave {
} // namespace internal {
} // namespace mesos {
