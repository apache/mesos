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
// limitations under the License

#include <string>
#include <tuple>
#include <vector>

#include <process/collect.hpp>
#include <process/io.hpp>
#include <process/subprocess.hpp>

#include <stout/bytes.hpp>
#include <stout/check.hpp>
#include <stout/error.hpp>
#include <stout/foreach.hpp>
#include <stout/numify.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/strings.hpp>

#include <stout/os/constants.hpp>
#include <stout/os/exists.hpp>

#include "common/status_utils.hpp"
#include "hdfs/hdfs.hpp"

#include "uri/schemes/hdfs.hpp"

using namespace process;

using std::string;
using std::vector;


struct CommandResult
{
  Option<int> status;
  string out;
  string err;
};


static Future<CommandResult> result(const Subprocess& s)
{
  CHECK_SOME(s.out());
  CHECK_SOME(s.err());

  return await(
      s.status(),
      io::read(s.out().get()),
      io::read(s.err().get()))
    .then([](const std::tuple<
        Future<Option<int>>,
        Future<string>,
        Future<string>>& t) -> Future<CommandResult> {
      const Future<Option<int>>& status = std::get<0>(t);
      if (!status.isReady()) {
        return Failure(
            "Failed to get the exit status of the subprocess: " +
            (status.isFailed() ? status.failure() : "discarded"));
      }

      const Future<string>& output = std::get<1>(t);
      if (!output.isReady()) {
        return Failure(
            "Failed to read stdout from the subprocess: " +
            (output.isFailed() ? output.failure() : "discarded"));
      }

      const Future<string>& error = std::get<2>(t);
      if (!error.isReady()) {
        return Failure(
            "Failed to read stderr from the subprocess: " +
            (error.isFailed() ? error.failure() : "discarded"));
      }

      CommandResult result;
      result.status = status.get();
      result.out = output.get();
      result.err = error.get();

      return result;
    });
}


Try<Owned<HDFS>> HDFS::create(const Option<string>& _hadoop)
{
  // Determine the hadoop client to use. If the user has specified
  // it, use it. If not, look for environment variable HADOOP_HOME. If
  // the environment variable is not set, assume it's on the PATH.
  string hadoop;

  if (_hadoop.isSome()) {
    hadoop = _hadoop.get();
  } else {
    Option<string> hadoopHome = os::getenv("HADOOP_HOME");
    if (hadoopHome.isSome()) {
      hadoop = path::join(hadoopHome.get(), "bin", "hadoop");
    } else {
      hadoop = "hadoop";
    }
  }

  // Check if the hadoop client is available.
  Try<Subprocess> subprocess = process::subprocess(hadoop + " version 2>&1");

  if (subprocess.isError()) {
    return Error("Failed to exec hadoop subprocess: " + subprocess.error());
  }

  Option<int> status = subprocess->status().get();
  if (status.isNone()) {
    return Error("No status found for 'hadoop version' command");
  }

  // Check the final status of the command
  if (status.get() != 0) {
    return Error(
        "Hadoop client is not available, exit status: " +
        stringify(status.get()));
  }

  return Owned<HDFS>(new HDFS(hadoop));
}


Try<mesos::URI> HDFS::parse(const string& uri)
{
  size_t schemePos = uri.find("://");
  if (schemePos == string::npos) {
    return Error("Missing scheme in url string");
  }

  const string uriPath = uri.substr(schemePos + 3);

  size_t pathPos = uriPath.find_first_of('/');
  if (pathPos == 0) {
    return mesos::uri::hdfs(uriPath);
  }

  // If path is specified in the URL, try to capture the host and path
  // separately.
  string host = uriPath;
  string path = "/";
  if (pathPos != string::npos) {
    host = host.substr(0, pathPos);
    path = uriPath.substr(pathPos);
  }

  if (host.empty()) {
    return mesos::uri::hdfs(path);
  }

  const vector<string> tokens = strings::tokenize(host, ":");

  if (tokens[0].empty()) {
    return Error("Host not found in url");
  }

  if (tokens.size() > 2) {
    return Error("Found multiple ports in url");
  }

  Option<int> port;
  if (tokens.size() == 2) {
    Try<int> numifyPort = numify<int>(tokens[1]);
    if (numifyPort.isError()) {
      return Error("Failed to parse port: " + numifyPort.error());
    }

    port = numifyPort.get();
  } else {
    // Default port for HDFS.
    port = 8020;
  }

  return mesos::uri::hdfs(path, tokens[0], port.get());
}


// An HDFS client path must be either a full URI or an absolute path. If it is
// a relative path, prepend "/" to make it absolute. (Note that all URI schemes
// supported by the HDFS client contain "://" whereas file paths never do.)
static string normalize(const string& hdfsPath)
{
  if (strings::contains(hdfsPath, "://") || // A URI or a malformed path.
      path::is_absolute(hdfsPath)) { // Already an absolute path.
    return hdfsPath;
  }

  // A relative, non-URI file path. Prepend "/".
  return path::join("", hdfsPath);
}


Future<bool> HDFS::exists(const string& path)
{
  Try<Subprocess> s = subprocess(
      hadoop,
      {"hadoop", "fs", "-test", "-e", normalize(path)},
      Subprocess::PATH(os::DEV_NULL),
      Subprocess::PIPE(),
      Subprocess::PIPE());

  if (s.isError()) {
    return Failure("Failed to execute the subprocess: " + s.error());
  }

  return result(s.get())
    .then([](const CommandResult& result) -> Future<bool> {
      if (result.status.isNone()) {
        return Failure("Failed to reap the subprocess");
      }

      if (WSUCCEEDED(result.status.get())) {
        return true;
      }

      if (WIFEXITED(result.status.get()) &&
          WEXITSTATUS(result.status.get()) == 1) {
        return false;
      }

      return Failure(
          "Unexpected result from the subprocess: "
          "status='" + WSTRINGIFY(result.status.get()) + "', " +
          "stdout='" + result.out + "', " +
          "stderr='" + result.err + "'");
    });
}


Future<Bytes> HDFS::du(const string& _path)
{
  const string path = normalize(_path);

  Try<Subprocess> s = subprocess(
      hadoop,
      {"hadoop", "fs", "-du", path},
      Subprocess::PATH(os::DEV_NULL),
      Subprocess::PIPE(),
      Subprocess::PIPE());

  if (s.isError()) {
    return Failure("Failed to execute the subprocess: " + s.error());
  }

  return result(s.get())
    .then([path](const CommandResult& result) -> Future<Bytes> {
      if (result.status.isNone()) {
        return Failure("Failed to reap the subprocess");
      }

      if (result.status.get() != 0) {
        return Failure(
            "Unexpected result from the subprocess: "
            "status='" + stringify(result.status.get()) + "', " +
            "stdout='" + result.out + "', " +
            "stderr='" + result.err + "'");
      }

      // We expect 2 space-separated output fields; a number of bytes
      // then the name of the path we gave. The 'hadoop' command can
      // emit various WARN or other log messages, so we make an effort
      // to scan for the field we want.
      foreach (const string& line, strings::tokenize(result.out, "\n")) {
        // Note that we use tokenize() rather than split() since
        // fields can be delimited by multiple spaces.
        vector<string> fields = strings::tokenize(line, " \t");

        // There might be 2 or 3 fields, see HADOOP-6857. The 2-field
        // version contains object size and path, the 3-field version
        // contains object size, object storage size and path.
        if ((fields.size() == 2 || fields.size() == 3) &&
            fields.back() == path) {
          Result<size_t> size = numify<size_t>(fields[0]);
          if (size.isSome()) {
            return Bytes(size.get());
          }
        }
      }

      return Failure("Unexpected output format: '" + result.out + "'");
    });
}


Future<Nothing> HDFS::rm(const string& path)
{
  Try<Subprocess> s = subprocess(
      hadoop,
      {"hadoop", "fs", "-rm", normalize(path)},
      Subprocess::PATH(os::DEV_NULL),
      Subprocess::PIPE(),
      Subprocess::PIPE());

  if (s.isError()) {
    return Failure("Failed to execute the subprocess: " + s.error());
  }

  return result(s.get())
    .then([](const CommandResult& result) -> Future<Nothing> {
      if (result.status.isNone()) {
        return Failure("Failed to reap the subprocess");
      }

      if (result.status.get() != 0) {
        return Failure(
            "Unexpected result from the subprocess: "
            "status='" + stringify(result.status.get()) + "', " +
            "stdout='" + result.out + "', " +
            "stderr='" + result.err + "'");
      }

      return Nothing();
    });
}


Future<Nothing> HDFS::copyFromLocal(const string& from, const string& to)
{
  if (!os::exists(from)) {
    return Failure("Failed to find '" + from + "'");
  }

  Try<Subprocess> s = subprocess(
      hadoop,
      {"hadoop", "fs", "-copyFromLocal", from, normalize(to)},
      Subprocess::PATH(os::DEV_NULL),
      Subprocess::PIPE(),
      Subprocess::PIPE());

  if (s.isError()) {
    return Failure("Failed to execute the subprocess: " + s.error());
  }

  return result(s.get())
    .then([](const CommandResult& result) -> Future<Nothing> {
      if (result.status.isNone()) {
        return Failure("Failed to reap the subprocess");
      }

      if (result.status.get() != 0) {
        return Failure(
            "Unexpected result from the subprocess: "
            "status='" + stringify(result.status.get()) + "', " +
            "stdout='" + result.out + "', " +
            "stderr='" + result.err + "'");
      }

      return Nothing();
    });
}


Future<Nothing> HDFS::copyToLocal(const string& from, const string& to)
{
  Try<Subprocess> s = subprocess(
      hadoop,
      {hadoop, "fs", "-copyToLocal", normalize(from), to},
      Subprocess::PATH(os::DEV_NULL),
      Subprocess::PIPE(),
      Subprocess::PIPE());

  if (s.isError()) {
    return Failure("Failed to execute the subprocess: " + s.error());
  }

  return result(s.get())
    .then([](const CommandResult& result) -> Future<Nothing> {
      if (result.status.isNone()) {
        return Failure("Failed to reap the subprocess");
      }

      if (result.status.get() != 0) {
        return Failure(
            "Unexpected result from the subprocess: "
            "status='" + stringify(result.status.get()) + "', " +
            "stdout='" + result.out + "', " +
            "stderr='" + result.err + "'");
      }

      return Nothing();
    });
}
