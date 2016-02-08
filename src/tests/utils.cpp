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

#include <gtest/gtest.h>

#include <mesos/http.hpp>

#include <process/future.hpp>
#include <process/gtest.hpp>
#include <process/http.hpp>
#include <process/pid.hpp>
#include <process/process.hpp>

#include <stout/gtest.hpp>

#include "tests/flags.hpp"
#include "tests/utils.hpp"

using std::string;

namespace mesos {
namespace internal {
namespace tests {

#if MESOS_INSTALL_TESTS
const bool searchInstallationDirectory = true;
#else
const bool searchInstallationDirectory = false;
#endif

JSON::Object Metrics()
{
  process::UPID upid("metrics", process::address());

  process::Future<process::http::Response> response =
      process::http::get(upid, "snapshot");

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(process::http::OK().status, response);
  AWAIT_EXPECT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response);

  Try<JSON::Object> parse = JSON::parse<JSON::Object>(response.get().body);
  CHECK_SOME(parse);

  return parse.get();
}

string getModulePath(const string& name)
{
  string path = path::join(
      tests::flags.build_dir,
      "src",
      ".libs");

  if (!os::exists(path) && searchInstallationDirectory) {
    path = PKGMODULEDIR;
  }

  return path::join(path, os::libraries::expandName(name));
}

string getLibMesosPath()
{
  string path = path::join(
      tests::flags.build_dir,
      "src",
      ".libs",
      os::libraries::expandName("mesos-" VERSION));

  if (!os::exists(path) && searchInstallationDirectory) {
    path = path::join(
        LIBDIR,
        os::libraries::expandName("mesos-" VERSION));
  }

  return path;
}

string getLauncherDir()
{
  string path = path::join(
      tests::flags.build_dir,
      "src");

  if (!os::exists(path) && searchInstallationDirectory) {
    path = PKGLIBEXECDIR;
  }

  return path;
}

string getTestHelperPath(const string& name)
{
  string path = path::join(
      tests::flags.build_dir,
      "src",
      name);

  if (!os::exists(path) && searchInstallationDirectory) {
    path = path::join(
        TESTLIBEXECDIR,
        name);
  }

  return path;
}

string getTestHelperDir()
{
  string path = path::join(
      tests::flags.build_dir,
      "src");

  if (!os::exists(path) && searchInstallationDirectory) {
      return TESTLIBEXECDIR;
  }

  return path;
}

string getTestScriptPath(const string& script)
{
  string path = path::join(
      flags.source_dir,
      "src",
      "tests",
      script);

  if (!os::exists(path) && searchInstallationDirectory) {
    path = path::join(
        TESTLIBEXECDIR,
        script);
  }

  return path;
}

string getSbinDir()
{
  string path = path::join(
      tests::flags.build_dir,
      "src");

  if (!os::exists(path) && searchInstallationDirectory) {
      return SBINDIR;
  }

  return path;
}

string getWebUIDir()
{
  string path = path::join(
      flags.source_dir,
      "src",
      "webui");

  if (!os::exists(path) && searchInstallationDirectory) {
    path = path::join(
        PKGDATADIR,
        "webui");
  }

  return path;
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
