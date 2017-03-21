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

#ifndef __WINDOWS__
#include <sys/wait.h>
#endif // __WINDOWS__

#include <string>
#include <tuple>
#include <vector>

#include <process/check.hpp>
#include <process/collect.hpp>
#include <process/io.hpp>
#include <process/subprocess.hpp>

#include <stout/path.hpp>
#include <stout/strings.hpp>

#include <stout/os/constants.hpp>
#include <stout/os/mkdir.hpp>

#include "uri/fetchers/curl.hpp"

namespace http = process::http;
namespace io = process::io;

using std::set;
using std::string;
using std::tuple;
using std::vector;

using process::await;
using process::subprocess;

using process::Failure;
using process::Future;
using process::Owned;
using process::Subprocess;

namespace mesos {
namespace uri {

const char CurlFetcherPlugin::NAME[] = "curl";


Try<Owned<Fetcher::Plugin>> CurlFetcherPlugin::create(const Flags& flags)
{
  // TODO(jieyu): Make sure curl is available.

  return Owned<Fetcher::Plugin>(new CurlFetcherPlugin());
}


set<string> CurlFetcherPlugin::schemes() const
{
  return {"http", "https", "ftp", "ftps"};
}


string CurlFetcherPlugin::name() const
{
  return NAME;
}


Future<Nothing> CurlFetcherPlugin::fetch(
    const URI& uri,
    const string& directory) const
{
  // TODO(jieyu): Validate the given URI.

  if (!uri.has_path()) {
    return Failure("URI path is not specified");
  }

  Try<Nothing> mkdir = os::mkdir(directory);
  if (mkdir.isError()) {
    return Failure(
        "Failed to create directory '" +
        directory + "': " + mkdir.error());
  }

  // TODO(jieyu): Allow user to specify the name of the output file.
  const string output = path::join(directory, Path(uri.path()).basename());

  const vector<string> argv = {
    "curl",
    "-s",                 // Don't show progress meter or error messages.
    "-S",                 // Makes curl show an error message if it fails.
    "-L",                 // Follow HTTP 3xx redirects.
    "-w", "%{http_code}", // Display HTTP response code on stdout.
    "-o", output,         // Write output to the file.
    strings::trim(stringify(uri))
  };

  Try<Subprocess> s = subprocess(
      "curl",
      argv,
      Subprocess::PATH(os::DEV_NULL),
      Subprocess::PIPE(),
      Subprocess::PIPE());

  if (s.isError()) {
    return Failure("Failed to exec the curl subprocess: " + s.error());
  }

  return await(
      s.get().status(),
      io::read(s.get().out().get()),
      io::read(s.get().err().get()))
    .then([](const tuple<
        Future<Option<int>>,
        Future<string>,
        Future<string>>& t) -> Future<Nothing> {
      Future<Option<int>> status = std::get<0>(t);
      if (!status.isReady()) {
        return Failure(
            "Failed to get the exit status of the curl subprocess: " +
            (status.isFailed() ? status.failure() : "discarded"));
      }

      if (status->isNone()) {
        return Failure("Failed to reap the curl subprocess");
      }

      if (status->get() != 0) {
        Future<string> error = std::get<2>(t);
        if (!error.isReady()) {
          return Failure(
              "Failed to perform 'curl'. Reading stderr failed: " +
              (error.isFailed() ? error.failure() : "discarded"));
        }

        return Failure("Failed to perform 'curl': " + error.get());
      }

      Future<string> output = std::get<1>(t);
      if (!output.isReady()) {
        return Failure(
            "Failed to read stdout from 'curl': " +
            (output.isFailed() ? output.failure() : "discarded"));
      }

      // Parse the output and get the HTTP response code.
      Try<int> code = numify<int>(output.get());
      if (code.isError()) {
        return Failure("Unexpected output from 'curl': " + output.get());
      }

      if (code.get() != http::Status::OK) {
        return Failure(
            "Unexpected HTTP response code: " +
            http::Status::string(code.get()));
      }

      return Nothing();
    });
}

} // namespace uri {
} // namespace mesos {
