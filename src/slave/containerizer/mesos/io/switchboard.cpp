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

#include <string>

#include <process/collect.hpp>
#include <process/defer.hpp>
#include <process/dispatch.hpp>
#include <process/future.hpp>
#include <process/io.hpp>
#include <process/owned.hpp>

#include <mesos/agent/agent.hpp>

#include <mesos/slave/container_logger.hpp>

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
        lambda::_1));
}


Future<Option<ContainerLaunchInfo>> IOSwitchboard::_prepare(
    const ContainerLogger::SubprocessInfo& loggerInfo)
{
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
