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

#include <signal.h>
#include <stdio.h>
#include <string.h>
#ifndef __WINDOWS__
#include <unistd.h>
#endif // __WINDOWS__

#include <iostream>
#include <string>
#include <vector>

#include <mesos/mesos.hpp>

#include <process/future.hpp>
#include <process/pid.hpp>
#include <process/process.hpp>
#include <process/protobuf.hpp>

#include <stout/protobuf.hpp>

#include "health-check/health_checker.hpp"

using namespace mesos;

using process::UPID;

namespace mesos {
namespace internal {

using namespace process;

Try<Owned<HealthChecker>> HealthChecker::create(
    const HealthCheck& check,
    const UPID& executor,
    const TaskID& taskID)
{
  // Validate the 'HealthCheck' protobuf.
  if (check.has_http() && check.has_command()) {
    return Error("Both 'http' and 'command' health check requested");
  }

  if (!check.has_http() && !check.has_command()) {
    return Error("Expecting one of 'http' or 'command' health check");
  }

  Owned<HealthCheckerProcess> process(new HealthCheckerProcess(
      check,
      executor,
      taskID));

  return Owned<HealthChecker>(new HealthChecker(process));
}


HealthChecker::HealthChecker(
    Owned<HealthCheckerProcess> _process)
  : process(_process)
{
  spawn(CHECK_NOTNULL(process.get()));
}


HealthChecker::~HealthChecker()
{
  terminate(process.get());
  wait(process.get());
}


Future<Nothing> HealthChecker::healthCheck()
{
  return dispatch(process.get(), &HealthCheckerProcess::healthCheck);
}

} // namespace internal {
} // namespace mesos {
