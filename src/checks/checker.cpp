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
#include <stout/variant.hpp>

#include "checks/checker_process.hpp"
#include "checks/checks_runtime.hpp"

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
    Variant<runtime::Plain, runtime::Docker, runtime::Nested> runtime)
{
  // Validate the `CheckInfo` protobuf.
  Option<Error> error = common::validation::validateCheckInfo(check);
  if (error.isSome()) {
    return error.get();
  }

  return Owned<Checker>(
      new Checker(
          check,
          launcherDir,
          callback,
          taskId,
          std::move(runtime)));
}


Checker::Checker(
    const CheckInfo& _check,
    const string& _launcherDir,
    const lambda::function<void(const CheckStatusInfo&)>& _callback,
    const TaskID& _taskId,
    Variant<runtime::Plain, runtime::Docker, runtime::Nested> _runtime)
  : check(_check),
    callback(_callback),
    taskId(_taskId),
    name(CheckInfo::Type_Name(check.type()) + " check"),
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
          name,
          std::move(_runtime),
          None(),
          false));

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

} // namespace checks {
} // namespace internal {
} // namespace mesos {
