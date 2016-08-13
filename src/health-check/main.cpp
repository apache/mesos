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

#include <mesos/mesos.hpp>

#include <process/future.hpp>
#include <process/pid.hpp>
#include <process/process.hpp>
#include <process/protobuf.hpp>

#include <stout/flags.hpp>
#include <stout/json.hpp>
#include <stout/option.hpp>
#include <stout/path.hpp>
#include <stout/protobuf.hpp>

#include "health-check/health_checker.hpp"

using namespace mesos;

using std::cout;
using std::cerr;
using std::endl;
using std::string;

using process::UPID;

class Flags : public virtual flags::FlagsBase
{
public:
  Flags()
  {
    add(&Flags::health_check_json,
        "health_check_json",
        "JSON describing health check to perform");

    add(&Flags::executor,
        "executor",
        "Executor UPID to send health check messages to");

    add(&Flags::task_id,
        "task_id",
        "Task ID that this health check process is checking");
  }

  Option<string> health_check_json;
  Option<UPID> executor;
  Option<string> task_id;
};


void usage(const char* argv0, const flags::FlagsBase& flags)
{
  cerr << "Usage: " << Path(argv0).basename() << " [...]" << endl
       << endl
       << "Supported options:" << endl
       << flags.usage();
}


int main(int argc, char** argv)
{
  GOOGLE_PROTOBUF_VERIFY_VERSION;

  Flags flags;

  Try<flags::Warnings> load = flags.load(None(), argc, argv);

  if (load.isError()) {
    cerr << flags.usage(load.error()) << endl;
    return EXIT_FAILURE;
  }

  if (flags.help) {
    cout << flags.usage() << endl;
    return EXIT_SUCCESS;
  }

  // Log any flag warnings.
  foreach (const flags::Warning& warning, load->warnings) {
    LOG(WARNING) << warning.message;
  }

  if (flags.health_check_json.isNone()) {
    cerr << flags.usage("Expected JSON with health check description") << endl;
    return EXIT_FAILURE;
  }

  Try<JSON::Object> parse =
    JSON::parse<JSON::Object>(flags.health_check_json.get());

  if (parse.isError()) {
    cerr << flags.usage("Failed to parse --health_check_json: " + parse.error())
         << endl;
    return EXIT_FAILURE;
  }

  Try<HealthCheck> check = protobuf::parse<HealthCheck>(parse.get());

  if (check.isError()) {
    cerr << flags.usage("Failed to parse --health_check_json: " + check.error())
         << endl;
    return EXIT_SUCCESS;
  }

  if (flags.executor.isNone()) {
    cerr << flags.usage("Missing required option --executor") << endl;
    return EXIT_FAILURE;
  }

  if (check.get().has_http() && check.get().has_command()) {
    cerr << flags.usage("Both 'http' and 'command' health check requested")
         << endl;
    return EXIT_FAILURE;
  }

  if (!check.get().has_http() && !check.get().has_command()) {
    cerr << flags.usage("Expecting one of 'http' or 'command' health check")
         << endl;
    return EXIT_FAILURE;
  }

  if (flags.task_id.isNone()) {
    cerr << flags.usage("Missing required option --task_id") << endl;
    return EXIT_FAILURE;
  }

  TaskID taskID;
  taskID.set_value(flags.task_id.get());

  mesos::internal::health::HealthCheckerProcess process(
    check.get(),
    flags.executor.get(),
    taskID);

  process::spawn(&process);

  process::Future<Nothing> checking =
    process::dispatch(
      process, &mesos::internal::health::HealthCheckerProcess::healthCheck);

  checking.await();

  process::terminate(process);
  process::wait(process);

  if (checking.isFailed()) {
    LOG(WARNING) << "Health check failed " << checking.failure();
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
