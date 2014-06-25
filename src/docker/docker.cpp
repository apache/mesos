#include <map>
#include <vector>

#include <stout/lambda.hpp>
#include <stout/strings.hpp>

#include <stout/result.hpp>

#include <stout/os/read.hpp>

#include <process/check.hpp>
#include <process/collect.hpp>

#include "docker/docker.hpp"

using namespace process;

using std::list;
using std::map;
using std::string;
using std::vector;


Try<Nothing> Docker::validateDocker(const Docker &docker)
{
  Future<list<Docker::Container> > containers = docker.ps();

  if (!containers.await(Seconds(3))) {
    return Error("Failed to use Docker: Timed out");
  } else if (containers.isFailed()) {
    return Error("Failed to use Docker: " + containers.failure());
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

pid_t Docker::Container::pid() const
{
  map<string, JSON::Value>::const_iterator state =
    json.values.find("State");
  CHECK(state != json.values.end());
  JSON::Value value = state->second;
  CHECK(value.is<JSON::Object>());

  map<string, JSON::Value>::const_iterator entry =
    value.as<JSON::Object>().values.find("Pid");
  CHECK(entry != json.values.end());
  JSON::Value pid = entry->second;
  CHECK(pid.is<JSON::Number>());
  return pid_t(pid.as<JSON::Number>().value);
}

Future<Option<int> > Docker::run(
    const string& image,
    const string& command,
    const string& name) const
{
  VLOG(1) << "Running " << path << " run --name=" << name << " "
          << image << " " << command;

  Try<Subprocess> s = subprocess(
      path + " run --name=" + name + " " + image + " " + command,
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


Future<list<Docker::Container> > Docker::ps(const bool all) const
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
    .then(lambda::bind(&Docker::_ps, Docker(path), s.get()));
}


Future<list<Docker::Container> > Docker::_ps(
    const Docker& docker,
    const Subprocess& s)
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
  CHECK_NE(0, lines.size());
  lines.erase(lines.begin());

  list<Future<Docker::Container> > futures;

  foreach (const string& line, lines) {
    // Inspect the container.
    futures.push_back(docker.inspect(strings::split(line, " ")[0]));
  }

  return collect(futures);
}
