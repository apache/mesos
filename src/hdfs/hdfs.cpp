/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License
 */

#include <string>
#include <vector>

#include <stout/bytes.hpp>
#include <stout/check.hpp>
#include <stout/error.hpp>
#include <stout/foreach.hpp>
#include <stout/numify.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/strings.hpp>

#include <stout/os/exists.hpp>
#include <stout/os/shell.hpp>

#include "hdfs/hdfs.hpp"

using std::string;
using std::vector;


HDFS::HDFS(const string& _hadoop)
  : hadoop(os::exists(_hadoop)
           ? _hadoop
           : (os::getenv("HADOOP_HOME").isSome()
              ? path::join(os::getenv("HADOOP_HOME").get(), "bin/hadoop")
              : "hadoop")) {}


HDFS::HDFS()
    : hadoop(os::getenv("HADOOP_HOME").isSome()
             ? path::join(os::getenv("HADOOP_HOME").get(), "bin/hadoop")
             : "hadoop") {}


Try<bool> HDFS::available()
{
  Try<string> command = strings::format("%s version", hadoop);

  CHECK_SOME(command);

  // We are piping stderr to stdout so that we can see the error (if
  // any) in the logs emitted by `os::shell()` in case of failure.
  Try<string> out = os::shell(command.get() + " 2>&1");

  if (out.isError()) {
    return Error(out.error());
  }

  return true;
}


Try<bool> HDFS::exists(const string& path)
{
  Try<string> command = strings::format(
      "%s fs -test -e '%s'", hadoop, absolutePath(path));

  CHECK_SOME(command);

  // We are piping stderr to stdout so that we can see the error (if
  // any) in the logs emitted by `os::shell()` in case of failure.
  Try<string> out = os::shell(command.get() + " 2>&1");

  if (out.isError()) {
    return Error(out.error());
  }

  return true;
}


Try<Bytes> HDFS::du(const string& _path)
{
  const string path = absolutePath(_path);

  Try<string> command = strings::format(
      "%s fs -du '%s'", hadoop, path);

  CHECK_SOME(command);

  // We are piping stderr to stdout so that we can see the error (if
  // any) in the logs emitted by `os::shell()` in case of failure.
  //
  // TODO(marco): this was the existing logic, but not sure it is
  // actually needed.
  Try<string> out = os::shell(command.get() + " 2>&1");

  if (out.isError()) {
    return Error("HDFS du failed: " + out.error());
  }

  // We expect 2 space-separated output fields; a number of bytes then
  // the name of the path we gave. The 'hadoop' command can emit
  // various WARN or other log messages, so we make an effort to scan
  // for the field we want.
  foreach (const string& line, strings::tokenize(out.get(), "\n")) {
    // Note that we use tokenize() rather than split() since fields
    // can be delimited by multiple spaces.
    vector<string> fields = strings::tokenize(line, " ");

    if (fields.size() == 2 && fields[1] == path) {
      Result<size_t> size = numify<size_t>(fields[0]);
      if (size.isError()) {
        return Error("HDFS du returned unexpected format: " + size.error());
      } else if (size.isNone()) {
        return Error("HDFS du returned unexpected format");
      }

      return Bytes(size.get());
    }
  }

  return Error("HDFS du returned an unexpected format: '" + out.get() + "'");
}


Try<Nothing> HDFS::rm(const string& path)
{
  Try<string> command = strings::format(
      "%s fs -rm '%s'", hadoop, absolutePath(path));

  CHECK_SOME(command);

  Try<string> out = os::shell(command.get());

  if (out.isError()) {
    return Error(out.error());
  }

  return Nothing();
}


Try<Nothing> HDFS::copyFromLocal(const string& from, const string& _to)
{
  if (!os::exists(from)) {
    return Error("Failed to find " + from);
  }

  const string to = absolutePath(_to);

  Try<string> command = strings::format(
      "%s fs -copyFromLocal '%s' '%s'", hadoop, from, to);

  CHECK_SOME(command);

  Try<string> out = os::shell(command.get());

  if (out.isError()) {
    return Error(out.error());
  }

  return Nothing();
}


Try<Nothing> HDFS::copyToLocal(const string& _from, const string& to)
{
  const string from = absolutePath(_from);

  Try<string> command = strings::format(
      "%s fs -copyToLocal '%s' '%s'", hadoop, from, to);

  CHECK_SOME(command);

  Try<string> out = os::shell(command.get());

  if (out.isError()) {
    return Error(out.error());
  }

  return Nothing();
}


string HDFS::absolutePath(const string& hdfsPath)
{
  if (strings::startsWith(hdfsPath, "hdfs://") ||
      strings::startsWith(hdfsPath, "/")) {
    return hdfsPath;
  }

  return path::join("", hdfsPath);
}
