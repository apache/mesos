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

#include <tuple>
#include <vector>

#include <process/collect.hpp>
#include <process/io.hpp>
#include <process/subprocess.hpp>

#include <stout/os.hpp>
#include <stout/unreachable.hpp>

#include <stout/os/constants.hpp>

#include "common/command_utils.hpp"
#include "common/status_utils.hpp"

using std::string;
using std::tuple;
using std::vector;

using process::Failure;
using process::Future;
using process::Subprocess;

namespace mesos {
namespace internal {
namespace command {

static Future<string> launch(
    const string& path,
    const vector<string>& argv)
{
  Try<Subprocess> s = subprocess(
      path,
      argv,
      Subprocess::PATH(os::DEV_NULL),
      Subprocess::PIPE(),
      Subprocess::PIPE());

  string command = strings::join(
      ", ",
      path,
      strings::join(", ", argv));

  if (s.isError()) {
    return Failure(
        "Failed to execute the subprocess '" + command + "': " + s.error());
  }

  return await(
      s->status(),
      process::io::read(s->out().get()),
      process::io::read(s->err().get()))
    .then([command](const tuple<
        Future<Option<int>>,
        Future<string>,
        Future<string>>& t) -> Future<string> {
      const Future<Option<int>>& status = std::get<0>(t);
      if (!status.isReady()) {
        return Failure(
            "Failed to get the exit status of the subprocess: " +
            (status.isFailed() ? status.failure() : "discarded"));
      }

      if (status->isNone()) {
        return Failure("Failed to reap the subprocess");
      }

      if (status->get() != 0) {
        const Future<string>& error = std::get<2>(t);
        if (!error.isReady()) {
            return Failure(
                "Unexpected result from the subprocess: " +
                 WSTRINGIFY(status->get()) + ", stderr='" +
                 error.get() + "'");
        }

        return Failure("Subprocess '" + command + "' failed: " + error.get());
      }

      const Future<string>& output = std::get<1>(t);
      if (!output.isReady()) {
         return Failure(
            "Failed to read stdout from '" + command + "': " +
            (output.isFailed() ? output.failure() : "discarded"));
      }

      return output;
    });
}


Future<Nothing> tar(
    const Path& input,
    const Path& output,
    const Option<Path>& directory,
    const Option<Compression>& compression)
{
  vector<string> argv = {
    "tar",
    "-c",  // Create archive.
    "-f",  // Output file.
    output
  };

  // Add additional flags.
  if (directory.isSome()) {
    argv.emplace_back("-C");
    argv.emplace_back(directory.get());
  }

  if (compression.isSome()) {
    switch (compression.get()) {
      case Compression::GZIP:
        argv.emplace_back("-z");
        break;
      case Compression::BZIP2:
        argv.emplace_back("-j");
        break;
      case Compression::XZ:
        argv.emplace_back("-J");
        break;
      default:
        UNREACHABLE();
    }
  }

  argv.emplace_back(input);

  return launch("tar", argv)
    .then([]() { return Nothing(); });
}


Future<Nothing> untar(
    const Path& input,
    const Option<Path>& directory)
{
  vector<string> argv = {
    "tar",
    "-x",  // Extract/unarchive.
    "-f",  // Input file to extract/unarchive.
    input
  };

  // Add additional flags.
  if (directory.isSome()) {
    argv.emplace_back("-C");
    argv.emplace_back(directory.get());
  }

  return launch("tar", argv)
    .then([]() { return Nothing(); });
}


Future<string> sha512(const Path& input)
{
#ifdef __linux__
  const string cmd = "sha512sum";
  vector<string> argv = {
    cmd,
    input             // Input file to compute shasum.
  };
#else
  const string cmd = "shasum";
  vector<string> argv = {
    cmd,
    "-a", "512",      // Shasum type.
    input             // Input file to compute shasum.
  };
#endif // __linux__

  return launch(cmd, argv)
    .then([cmd](const string& output) -> Future<string> {
      vector<string> tokens = strings::tokenize(output, " ");
      if (tokens.size() < 2) {
        return Failure(
            "Failed to parse '" + output + "' from '" + cmd + "' command");
      }

      // TODO(jojy): Check the size of tokens[0].

      return tokens[0];
    });
}


Future<Nothing> gzip(const Path& input)
{
  vector<string> argv = {
    "gzip",
    input
  };

  return launch("gzip", argv)
    .then([]() { return Nothing(); });
}


Future<Nothing> decompress(const Path& input)
{
  vector<string> argv = {
    "gzip",
    "-d",  // Decompress.
    input
  };

  return launch("gzip", argv)
    .then([]() { return Nothing(); });
}

} // namespace command {
} // namespace internal {
} // namespace mesos {
