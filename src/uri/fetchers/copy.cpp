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

#include "uri/fetchers/copy.hpp"

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

const char CopyFetcherPlugin::NAME[] = "copy";


Try<Owned<Fetcher::Plugin>> CopyFetcherPlugin::create(const Flags& flags)
{
  return Owned<Fetcher::Plugin>(new CopyFetcherPlugin());
}


set<string> CopyFetcherPlugin::schemes() const
{
  return {"file"};
}


string CopyFetcherPlugin::name() const
{
  return NAME;
}


Future<Nothing> CopyFetcherPlugin::fetch(
    const URI& uri,
    const string& directory,
    const Option<string>& data,
    const Option<string>& outputFileName) const
{
  // TODO(jojy): Validate the given URI.

  if (!uri.has_path()) {
    return Failure("URI path is not specified");
  }

  // TODO(jojy): Verify that the path is a file.

  Try<Nothing> mkdir = os::mkdir(directory);
  if (mkdir.isError()) {
    return Failure(
        "Failed to create directory '" +
        directory + "': " + mkdir.error());
  }

  VLOG(1) << "Copying '" << uri.path() << "' to '" << directory << "'";

#ifndef __WINDOWS__
  const char* copyCommand = "cp";
  const vector<string> argv = {"cp", "-a", uri.path(), directory};
#else // __WINDOWS__
  const char* copyCommand = os::Shell::name;
  const vector<string> argv =
    {os::Shell::arg0, os::Shell::arg1, "copy", "/Y", uri.path(), directory};
#endif // __WINDOWS__

  Try<Subprocess> s = subprocess(
      copyCommand,
      argv,
      Subprocess::PATH(os::DEV_NULL),
      Subprocess::PIPE(),
      Subprocess::PIPE());

  if (s.isError()) {
    return Failure("Failed to exec the copy subprocess: " + s.error());
  }

  return await(
      s->status(),
      io::read(s->out().get()),
      io::read(s->err().get()))
    .then([](const tuple<
        Future<Option<int>>,
        Future<string>,
        Future<string>>& t) -> Future<Nothing> {
      const Future<Option<int>>& status = std::get<0>(t);
      if (!status.isReady()) {
        return Failure(
            "Failed to get the exit status of the copy subprocess: " +
            (status.isFailed() ? status.failure() : "discarded"));
      }

      if (status->isNone()) {
        return Failure("Failed to reap the copy subprocess");
      }

      if (status->get() != 0) {
        const Future<string>& error = std::get<2>(t);
        if (!error.isReady()) {
          return Failure(
              "Failed to perform 'copy'. Reading stderr failed: " +
              (error.isFailed() ? error.failure() : "discarded"));
        }

        return Failure("Failed to perform 'copy': " + error.get());
      }

      return Nothing();
    });
}

} // namespace uri {
} // namespace mesos {
