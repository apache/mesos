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
 * limitations under the License.
 */

#include <map>
#include <vector>

#include <stout/lambda.hpp>
#include <stout/strings.hpp>

#include <stout/result.hpp>

#include <stout/os/read.hpp>

#include <process/check.hpp>
#include <process/collect.hpp>

#include "docker/docker.hpp"

#include "linux/cgroups.hpp"

#include "slave/containerizer/isolators/cgroups/cpushare.hpp"
#include "slave/containerizer/isolators/cgroups/mem.hpp"

using namespace mesos::internal::slave;

using namespace process;

using std::list;
using std::map;
using std::string;
using std::vector;


Try<Nothing> Docker::validate(const Docker &docker)
{
  // Make sure that cgroups are mounted, and at least the 'cpu'
  // subsystem is attached.
  Result<string> hierarchy = cgroups::hierarchy("cpu");

  if (hierarchy.isNone()) {
    return Error("Failed to find a mounted cgroups hierarchy "
                 "for the 'cpu' subsystem, you probably need "
                 "to mount cgroups manually!");
  }

  Future<std::string> info = docker.info();

  if (!info.await(Seconds(3))) {
    return Error("Failed to use Docker: Timed out");
  } else if (info.isFailed()) {
    return Error("Failed to use Docker: " + info.failure());
  }

  return Nothing();
}


string Docker::Container::id() const
{
  map<string, JSON::Value>::const_iterator entry =
    json.values.find("Id");
  CHECK(entry != json.values.end());
  JSON::Value value = entry->second;
  CHECK(value.is<JSON::String>());
  return value.as<JSON::String>().value;
}

string Docker::Container::name() const
{
  map<string, JSON::Value>::const_iterator entry =
    json.values.find("Name");
  CHECK(entry != json.values.end());
  JSON::Value value = entry->second;
  CHECK(value.is<JSON::String>());
  return value.as<JSON::String>().value;
}

Option<pid_t> Docker::Container::pid() const
{
  map<string, JSON::Value>::const_iterator state =
    json.values.find("State");
  CHECK(state != json.values.end());
  JSON::Value value = state->second;
  CHECK(value.is<JSON::Object>());

  map<string, JSON::Value>::const_iterator entry =
    value.as<JSON::Object>().values.find("Pid");
  CHECK(entry != json.values.end());
  // TODO(yifan) reload operator '=' to reuse the value variable above.
  JSON::Value pidValue = entry->second;
  CHECK(pidValue.is<JSON::Number>());

  pid_t pid = pid_t(pidValue.as<JSON::Number>().value);
  if (pid == 0) {
    return None();
  }
  return pid;
}

Future<Option<int> > Docker::run(
    const string& image,
    const string& command,
    const string& name,
    const mesos::Resources& resources) const
{
  CHECK(resources.size() != 0);

  string cmd = " run -d";

  // TODO(yifan): Support other resources (e.g. disk, ports).
  Option<double> cpus = resources.cpus();
  if (cpus.isSome()) {
    uint64_t cpuShare =
      std::max((uint64_t) (CPU_SHARES_PER_CPU * cpus.get()), MIN_CPU_SHARES);
    cmd += " -c " + stringify(cpuShare);
  }

  Option<Bytes> mem = resources.mem();
  if (mem.isSome()) {
    Bytes memLimit = std::max(mem.get(), MIN_MEMORY);
    cmd += " -m " + stringify(memLimit.bytes());
  }

  cmd += " --name=" + name + " " + image + " " + command;

  VLOG(1) << "Running " << path << cmd;

  Try<Subprocess> s = subprocess(
      path + cmd,
      Subprocess::PIPE(),
      Subprocess::PIPE(),
      Subprocess::PIPE());

  if (s.isError()) {
    return Failure(s.error());
  }
  return s.get().status();
}


Future<Option<int> > Docker::kill(const string& container) const
{
  VLOG(1) << "Running " << path << " kill " << container;

  Try<Subprocess> s = subprocess(
      path + " kill " + container,
      Subprocess::PIPE(),
      Subprocess::PIPE(),
      Subprocess::PIPE());

  if (s.isError()) {
    return Failure(s.error());
  }

  return s.get().status();
}


Future<Option<int> > Docker::rm(
    const string& container,
    const bool force) const
{
  string cmd = force ? " rm -f " : " rm ";

  VLOG(1) << "Running " << path << cmd << container;

  Try<Subprocess> s = subprocess(
      path + cmd + container,
      Subprocess::PIPE(),
      Subprocess::PIPE(),
      Subprocess::PIPE());

  if (s.isError()) {
    return Failure(s.error());
  }

  return s.get().status();
}


Future<Option<int> > Docker::killAndRm(const string& container) const
{
  return kill(container)
    .then(lambda::bind(Docker::_killAndRm, *this, container, lambda::_1));
}


Future<Option<int> > Docker::_killAndRm(
    const Docker& docker,
    const string& container,
    const Option<int>& status)
{
  // If 'kill' fails, then do a 'rm -f'.
  if (status.isNone()) {
    return docker.rm(container, true);
  }
  return docker.rm(container);
}


Future<Docker::Container> Docker::inspect(const string& container) const
{
  VLOG(1) << "Running " << path << " inspect " << container;

  Try<Subprocess> s = subprocess(
      path + " inspect " + container,
      Subprocess::PIPE(),
      Subprocess::PIPE(),
      Subprocess::PIPE());

  if (s.isError()) {
    return Failure(s.error());
  }

  return s.get().status()
    .then(lambda::bind(&Docker::_inspect, s.get()));
}


namespace os {

inline Result<std::string> read(
    int fd,
    Option<size_t> size = None(),
    size_t chunk = 16 * 4096)
{
  std::string result;

  while (size.isNone() || result.size() < size.get()) {
    char buffer[chunk];
    ssize_t length = ::read(fd, buffer, chunk);

    if (length < 0) {
      // TODO(bmahler): Handle a non-blocking fd? (EAGAIN, EWOULDBLOCK)
      if (errno == EINTR) {
        continue;
      }
      return ErrnoError();
    } else if (length == 0) {
      // Reached EOF before expected! Only return as much data as
      // available or None if we haven't read anything yet.
      if (result.size() > 0) {
        return result;
      }
      return None();
    }

    result.append(buffer, length);
  }

  return result;
}

} // namespace os {


Future<Docker::Container> Docker::_inspect(const Subprocess& s)
{
  // Check the exit status of 'docker inspect'.
  CHECK_READY(s.status());

  Option<int> status = s.status().get();

  if (status.isSome() && status.get() != 0) {
    // TODO(benh): Include stderr in error message.
    Result<string> read = os::read(s.err().get());
    return Failure("Failed to do 'docker inspect': " +
                   (read.isSome()
                    ? read.get()
                    : " exited with status " + stringify(status.get())));
  }

  // Read to EOF.
  // TODO(benh): Read output asynchronously.
  CHECK_SOME(s.out());
  Result<string> output = os::read(s.out().get());

  if (output.isError()) {
    // TODO(benh): Include stderr in error message.
    return Failure("Failed to read output: " + output.error());
  } else if (output.isNone()) {
    // TODO(benh): Include stderr in error message.
    return Failure("No output available");
  }

  Try<JSON::Array> parse = JSON::parse<JSON::Array>(output.get());

  if (parse.isError()) {
    return Failure("Failed to parse JSON: " + parse.error());
  }

  JSON::Array array = parse.get();

  // Skip the container if it no longer exists.
  if (array.values.size() == 1) {
    CHECK(array.values.front().is<JSON::Object>());
    return Docker::Container(array.values.front().as<JSON::Object>());
  }

  // TODO(benh): Handle the case where the short container ID was
  // not sufficiently unique and 'array.values.size() > 1'.

  return Failure("Failed to find container");
}


Future<list<Docker::Container> > Docker::ps(
    const bool all,
    const Option<string>& prefix) const
{
  string cmd = all ? " ps -a" : " ps";

  VLOG(1) << "Running " << path << cmd;

  Try<Subprocess> s = subprocess(
      path + cmd,
      Subprocess::PIPE(),
      Subprocess::PIPE(),
      Subprocess::PIPE());

  if (s.isError()) {
    return Failure(s.error());
  }

  return s.get().status()
    .then(lambda::bind(&Docker::_ps, *this, s.get(), prefix));
}


Future<list<Docker::Container> > Docker::_ps(
    const Docker& docker,
    const Subprocess& s,
    const Option<string>& prefix)
{
  // Check the exit status of 'docker ps'.
  CHECK_READY(s.status());

  Option<int> status = s.status().get();

  if (status.isSome() && status.get() != 0) {
    // TODO(benh): Include stderr in error message.
    return Failure("Failed to do 'docker ps'");
  }

  // Read to EOF.
  // TODO(benh): Read output asynchronously.
  CHECK_SOME(s.out());
  Result<string> output = os::read(s.out().get());

  if (output.isError()) {
    // TODO(benh): Include stderr in error message.
    return Failure("Failed to read output: " + output.error());
  } else if (output.isNone()) {
    // TODO(benh): Include stderr in error message.
    return Failure("No output available");
  }

  vector<string> lines = strings::tokenize(output.get(), "\n");

  // Skip the header.
  CHECK(!lines.empty());
  lines.erase(lines.begin());

  list<Future<Docker::Container> > futures;

  foreach (const string& line, lines) {
    // Inspect the containers that we are interested in depending on
    // whether or not a 'prefix' was specified.
    vector<string> columns = strings::split(strings::trim(line), " ");
    string name = columns[columns.size() - 1];
    if (prefix.isNone()) {
      futures.push_back(docker.inspect(name));
    } else if (strings::startsWith(name, prefix.get())) {
      futures.push_back(docker.inspect(name));
    }
  }

  return collect(futures);
}


Future<std::string> Docker::info() const
{
  std::string cmd = path + " info";

  VLOG(1) << "Running " << cmd;

  Try<Subprocess> s = subprocess(
      cmd,
      Subprocess::PIPE(),
      Subprocess::PIPE(),
      Subprocess::PIPE());

  if (s.isError()) {
    return Failure(s.error());
  }

  Result<string> output = os::read(s.get().out().get());

  if (output.isError()) {
    // TODO(benh): Include stderr in error message.
    return Failure("Failed to read output: " + output.error());
  } else if (output.isNone()) {
    // TODO(benh): Include stderr in error message.
    return Failure("No output available");
  }

  return output.get();
}
