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

CurlFetcherPlugin::Flags::Flags()
{
  add(&Flags::curl_stall_timeout,
      "curl_stall_timeout",
      "Amount of time for the fetcher to wait before considering a download\n"
      "being too slow and abort it when the download stalls (i.e., the speed\n"
      "keeps below one byte per second).\n");
}


const char CurlFetcherPlugin::NAME[] = "curl";


Try<Owned<Fetcher::Plugin>> CurlFetcherPlugin::create(const Flags& flags)
{
  // TODO(jieyu): Make sure curl is available.

  return Owned<Fetcher::Plugin>(new CurlFetcherPlugin(flags));
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
    const string& directory,
    const Option<string>& data,
    const Option<string>& outputFileName) const
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

  string output;
  if (outputFileName.isSome()) {
    output = path::join(directory, outputFileName.get());
  } else {
    output = path::join(directory, Path(path::from_uri(uri.path())).basename());
  }

#ifndef __WINDOWS__
  const string curl = "curl";
#else
  const string curl = "curl.exe";
#endif // __WINDOWS__

  vector<string> argv = {
    curl,
    "-s",                 // Don't show progress meter or error messages.
    "-S",                 // Makes curl show an error message if it fails.
    "-L",                 // Follow HTTP 3xx redirects.
    "-w", "%{http_code}", // Display HTTP response code on stdout.
    "-o", output,         // Write output to the file.
    strings::trim(stringify(uri))
  };

  // Add a timeout for curl to abort when the download speed keeps low
  // (1 byte per second by default) for the specified duration. See:
  // https://curl.haxx.se/docs/manpage.html#-y
  if (flags.curl_stall_timeout.isSome()) {
    argv.push_back("-y");
    argv.push_back(
        std::to_string(static_cast<long>(flags.curl_stall_timeout->secs())));
  }

  Try<Subprocess> s = subprocess(
      curl,
      argv,
      Subprocess::PATH(os::DEV_NULL),
      Subprocess::PIPE(),
      Subprocess::PIPE());

  if (s.isError()) {
    return Failure("Failed to exec the curl subprocess: " + s.error());
  }

  return await(
      s->status(),
      io::read(s->out().get()),
      io::read(s->err().get()))
    .then([uri](const tuple<
        Future<Option<int>>,
        Future<string>,
        Future<string>>& t) -> Future<Nothing> {
      const Future<Option<int>>& status = std::get<0>(t);
      if (!status.isReady()) {
        return Failure(
            "Failed to get the exit status of the curl subprocess for '" +
            stringify(uri) + "': " +
            (status.isFailed() ? status.failure() : "discarded"));
      }

      if (status->isNone()) {
        return Failure("Failed to reap the curl subprocess for '" +
                       stringify(uri) + "'");
      }

      if (status->get() != 0) {
        const Future<string>& error = std::get<2>(t);
        if (!error.isReady()) {
          return Failure(
              "Failed to perform 'curl' for '" + stringify(uri) +
              "'. Reading stderr failed: " +
              (error.isFailed() ? error.failure() : "discarded"));
        }

        return Failure("Failed to perform 'curl' for '" + stringify(uri) +
                       "': " + error.get());
      }

      const Future<string>& output = std::get<1>(t);
      if (!output.isReady()) {
        return Failure(
            "Failed to read stdout from 'curl' for '" + stringify(uri) +
            "': " + (output.isFailed() ? output.failure() : "discarded"));
      }

      // Parse the output and get the HTTP response code.
      Try<int> code = numify<int>(output.get());
      if (code.isError()) {
        return Failure("Unexpected output from 'curl' for '" + stringify(uri) +
                       "': " + output.get());
      }

      if (code.get() != http::Status::OK) {
        return Failure("Unexpected HTTP response code for '" + stringify(uri) +
                       "': " + http::Status::string(code.get()));
      }

      return Nothing();
    });
}

} // namespace uri {
} // namespace mesos {
