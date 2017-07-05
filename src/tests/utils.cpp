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

#include "tests/utils.hpp"

#include <gtest/gtest.h>

#include <mesos/http.hpp>

#include <process/address.hpp>
#include <process/future.hpp>
#include <process/gtest.hpp>
#include <process/http.hpp>
#include <process/pid.hpp>
#include <process/process.hpp>
#include <process/socket.hpp>

#include <stout/gtest.hpp>

#include "tests/flags.hpp"

namespace http = process::http;
namespace inet = process::network::inet;
namespace inet4 = process::network::inet4;

using std::set;
using std::string;

using process::Future;
using process::UPID;

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
  UPID upid("metrics", process::address());

  // TODO(neilc): This request might timeout if the current value of a
  // metric cannot be determined. In tests, a common cause for this is
  // MESOS-6231 when multiple scheduler drivers are in use.
  Future<http::Response> response = http::get(upid, "snapshot");

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(http::OK().status, response);
  AWAIT_EXPECT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response);

  Try<JSON::Object> parse = JSON::parse<JSON::Object>(response->body);
  CHECK_SOME(parse);

  return parse.get();
}


Try<uint16_t> getFreePort()
{
  // Bind to port=0 to obtain a random unused port.
  Try<inet::Socket> socket = inet::Socket::create();

  if (socket.isError()) {
    return Error(socket.error());
  }

  Try<inet::Address> address = socket->bind(inet4::Address::ANY_ANY());

  if (address.isError()) {
    return Error(address.error());
  }

  return address->port;

  // No explicit cleanup of `socket` as we rely on the implementation
  // of `Socket` to close the socket on destruction.
}


string getModulePath(const string& name)
{
  string path = path::join(tests::flags.build_dir, "src", ".libs");

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
    path = path::join(LIBDIR, os::libraries::expandName("mesos-" VERSION));
  }

  return path;
}


string getLauncherDir()
{
  string path = path::join(tests::flags.build_dir, "src");

  if (!os::exists(path) && searchInstallationDirectory) {
    path = PKGLIBEXECDIR;
  }

  return path;
}


string getTestHelperPath(const string& name)
{
  string path = path::join(tests::flags.build_dir, "src", name);

  if (!os::exists(path) && searchInstallationDirectory) {
    path = path::join(TESTLIBEXECDIR, name);
  }

  return path;
}


string getTestHelperDir()
{
  string path = path::join(tests::flags.build_dir, "src");

  if (!os::exists(path) && searchInstallationDirectory) {
    path = TESTLIBEXECDIR;
  }

  return path;
}


string getTestScriptPath(const string& name)
{
  string path = path::join(flags.source_dir, "src", "tests", name);

  if (!os::exists(path) && searchInstallationDirectory) {
    path = path::join(TESTLIBEXECDIR, name);
  }

  return path;
}


string getSbinDir()
{
  string path = path::join(tests::flags.build_dir, "src");

  if (!os::exists(path) && searchInstallationDirectory) {
    path = SBINDIR;
  }

  return path;
}


string getWebUIDir()
{
  string path = path::join(flags.source_dir, "src", "webui");

  if (!os::exists(path) && searchInstallationDirectory) {
    path = path::join(PKGDATADIR, "webui");
  }

  return path;
}


Try<net::IP::Network> getNonLoopbackIP()
{
  Try<set<string>> links = net::links();
  if (links.isError()) {
    return Error(
        "Unable to retrieve interfaces on this host: " +
        links.error());
  }

  foreach (const string& link, links.get()) {
    Result<net::IP::Network> hostNetwork =
      net::IP::Network::fromLinkDevice(link, AF_INET);

    if (hostNetwork.isError()) {
      return Error(
          "Unable to find a non-loopback address: " +
          hostNetwork.error());
    }

    if (hostNetwork.isSome() &&
        (hostNetwork.get() != net::IP::Network::LOOPBACK_V4())) {
      return hostNetwork.get();
    }
  }

  return Error("No non-loopback addresses available on this host");
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
