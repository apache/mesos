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

#include "checks/checker.hpp"

#include <cstdint>
#include <string>
#include <vector>

#include <glog/logging.h>

#include <mesos/mesos.hpp>
#include <mesos/type_utils.hpp>

#include <process/future.hpp>

#include <stout/exit.hpp>
#include <stout/option.hpp>
#include <stout/protobuf.hpp>
#include <stout/stopwatch.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>
#include <stout/uuid.hpp>

#include "checks/checker_process.hpp"

#include "common/http.hpp"
#include "common/status_utils.hpp"
#include "common/validation.hpp"

namespace http = process::http;

using process::Future;
using process::Owned;

using std::string;
using std::vector;

namespace mesos {
namespace internal {
namespace checks {


// Creates a valid instance of `CheckStatusInfo` with the `type` set in
// accordance to the associated `CheckInfo`.
static CheckStatusInfo createEmptyCheckStatusInfo(const CheckInfo& checkInfo) {
  CheckStatusInfo checkStatusInfo;
  checkStatusInfo.set_type(checkInfo.type());

  switch (checkInfo.type()) {
    case CheckInfo::COMMAND: {
      checkStatusInfo.mutable_command();
      break;
    }
    case CheckInfo::HTTP: {
      checkStatusInfo.mutable_http();
      break;
    }
    case CheckInfo::TCP: {
      checkStatusInfo.mutable_tcp();
      break;
    }
    case CheckInfo::UNKNOWN: {
      LOG(FATAL) << "Received UNKNOWN check type";
      break;
    }
  }

  return checkStatusInfo;
}


Try<Owned<Checker>> Checker::create(
    const CheckInfo& check,
    const string& launcherDir,
    const lambda::function<void(const CheckStatusInfo&)>& callback,
    const TaskID& taskId,
    const Option<pid_t>& taskPid,
    const vector<string>& namespaces)
{
  // Validate the `CheckInfo` protobuf.
  Option<Error> error = validation::checkInfo(check);
  if (error.isSome()) {
    return error.get();
  }

  return Owned<Checker>(
      new Checker(
          check,
          launcherDir,
          callback,
          taskId,
          taskPid,
          namespaces,
          None(),
          None(),
          None(),
          false));
}


Try<Owned<Checker>> Checker::create(
    const CheckInfo& check,
    const string& launcherDir,
    const lambda::function<void(const CheckStatusInfo&)>& callback,
    const TaskID& taskId,
    const ContainerID& taskContainerId,
    const http::URL& agentURL,
    const Option<string>& authorizationHeader)
{
  // Validate the `CheckInfo` protobuf.
  Option<Error> error = validation::checkInfo(check);
  if (error.isSome()) {
    return error.get();
  }

  return Owned<Checker>(
      new Checker(
          check,
          launcherDir,
          callback,
          taskId,
          None(),
          {},
          taskContainerId,
          agentURL,
          authorizationHeader,
          true));
}


Checker::Checker(
    const CheckInfo& _check,
    const string& _launcherDir,
    const lambda::function<void(const CheckStatusInfo&)>& _callback,
    const TaskID& _taskId,
    const Option<pid_t>& _taskPid,
    const vector<string>& _namespaces,
    const Option<ContainerID>& _taskContainerId,
    const Option<http::URL>& _agentURL,
    const Option<string>& _authorizationHeader,
    bool _commandCheckViaAgent)
  : check(_check),
    callback(_callback),
    name(CheckInfo::Type_Name(check.type()) + " check"),
    taskId(_taskId),
    previousCheckStatus(createEmptyCheckStatusInfo(_check))
{
  VLOG(1) << "Check configuration for task '" << taskId << "':"
          << " '" << jsonify(JSON::Protobuf(check)) << "'";

  process.reset(
      new CheckerProcess(
          _check,
          _launcherDir,
          std::bind(&Checker::processCheckResult, this, lambda::_1),
          _taskId,
          _taskPid,
          _namespaces,
          _taskContainerId,
          _agentURL,
          _authorizationHeader,
          None(),
          name,
          _commandCheckViaAgent));

  spawn(process.get());
}


Checker::~Checker()
{
  terminate(process.get());
  wait(process.get());
}


void Checker::pause()
{
  dispatch(process.get(), &CheckerProcess::pause);
}


void Checker::resume()
{
  dispatch(process.get(), &CheckerProcess::resume);
}


void Checker::processCheckResult(const Try<CheckStatusInfo>& result) {
  CheckStatusInfo checkStatusInfo;

  if (result.isError()) {
    LOG(WARNING) << name << " for task '" << taskId << "'"
                 << " failed: " << result.error();

    checkStatusInfo = createEmptyCheckStatusInfo(check);
  } else {
    checkStatusInfo = result.get();
  }

  // Trigger the callback if check info changes.
  if (checkStatusInfo != previousCheckStatus) {
    // We assume this is a local send, i.e., the checker library is not used
    // in a binary external to the executor and hence can not exit before
    // the data is sent to the executor.
    callback(checkStatusInfo);

    previousCheckStatus = checkStatusInfo;
  }
}


namespace validation {

Option<Error> checkInfo(const CheckInfo& checkInfo)
{
  if (!checkInfo.has_type()) {
    return Error("CheckInfo must specify 'type'");
  }

  switch (checkInfo.type()) {
    case CheckInfo::COMMAND: {
      if (!checkInfo.has_command()) {
        return Error("Expecting 'command' to be set for COMMAND check");
      }

      const CommandInfo& command = checkInfo.command().command();

      if (!command.has_value()) {
        string commandType =
          (command.shell() ? "'shell command'" : "'executable path'");

        return Error("Command check must contain " + commandType);
      }

      Option<Error> error =
        common::validation::validateCommandInfo(command);
      if (error.isSome()) {
        return Error(
            "Check's `CommandInfo` is invalid: " + error->message);
      }

      // TODO(alexr): Make sure irrelevant fields, e.g., `uris` are not set.

      break;
    }
    case CheckInfo::HTTP: {
      if (!checkInfo.has_http()) {
        return Error("Expecting 'http' to be set for HTTP check");
      }

      const CheckInfo::Http& http = checkInfo.http();

      if (http.has_path() && !strings::startsWith(http.path(), '/')) {
        return Error(
            "The path '" + http.path() + "' of HTTP check must start with '/'");
      }

      break;
    }
    case CheckInfo::TCP: {
      if (!checkInfo.has_tcp()) {
        return Error("Expecting 'tcp' to be set for TCP check");
      }

      break;
    }
    case CheckInfo::UNKNOWN: {
      return Error(
          "'" + CheckInfo::Type_Name(checkInfo.type()) + "'"
          " is not a valid check type");
    }
  }

  if (checkInfo.has_delay_seconds() && checkInfo.delay_seconds() < 0.0) {
    return Error("Expecting 'delay_seconds' to be non-negative");
  }

  if (checkInfo.has_interval_seconds() && checkInfo.interval_seconds() < 0.0) {
    return Error("Expecting 'interval_seconds' to be non-negative");
  }

  if (checkInfo.has_timeout_seconds() && checkInfo.timeout_seconds() < 0.0) {
    return Error("Expecting 'timeout_seconds' to be non-negative");
  }

  return None();
}


Option<Error> checkStatusInfo(const CheckStatusInfo& checkStatusInfo)
{
  if (!checkStatusInfo.has_type()) {
    return Error("CheckStatusInfo must specify 'type'");
  }

  switch (checkStatusInfo.type()) {
    case CheckInfo::COMMAND: {
      if (!checkStatusInfo.has_command()) {
        return Error(
            "Expecting 'command' to be set for COMMAND check's status");
      }
      break;
    }
    case CheckInfo::HTTP: {
      if (!checkStatusInfo.has_http()) {
        return Error("Expecting 'http' to be set for HTTP check's status");
      }
      break;
    }
    case CheckInfo::TCP: {
      if (!checkStatusInfo.has_tcp()) {
        return Error("Expecting 'tcp' to be set for TCP check's status");
      }
      break;
    }
    case CheckInfo::UNKNOWN: {
      return Error(
          "'" + CheckInfo::Type_Name(checkStatusInfo.type()) + "'"
          " is not a valid check's status type");
    }
  }

  return None();
}

} // namespace validation {

} // namespace checks {
} // namespace internal {
} // namespace mesos {
