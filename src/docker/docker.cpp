#include <map>
#include <vector>

#include <stout/lambda.hpp>
#include <stout/strings.hpp>

#include <process/check.hpp>
#include <process/collect.hpp>
#include <process/io.hpp>

#include "docker/docker.hpp"

using namespace process;

using std::list;
using std::map;
using std::string;
using std::vector;


string Docker::Container::name() const
{
  map<string, JSON::Value>::const_iterator entry =
    json.values.find("Name");
  CHECK(entry != json.values.end());
  JSON::Value value = entry->second;
  CHECK(value.is<JSON::String>());
  return value.as<JSON::String>().value;
}


Future<Option<int> > Docker::run(const string& image) const
{
  Try<Subprocess> s = subprocess(
      path + " run " + image,
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
  Try<Subprocess> s = subprocess(
      path + " inspect " + container,
      Subprocess::PIPE(),
      Subprocess::PIPE(),
      Subprocess::PIPE());

  if (s.isError()) {
    // TODO(benh): Include stdout and stderr in error message.
    return Failure(s.error());
  }

  return s.get().status()
    .then(lambda::bind(&Docker::_inspect, s.get()));
}


Future<Docker::Container> Docker::_inspect(const Subprocess& s)
{
  // Check the exit status of 'docker ps'.
  CHECK_READY(s.status());

  Option<int> status = s.status().get();

  if (status.isSome() && status.get() != 0) {
    // TODO(benh): Include stdout and stderr in error message.
    return Failure("Failed to do 'docker ps'");
  }

  // Read to EOF.
  // TODO(benh): Read output asynchronously.
  CHECK_SOME(s.out());
  string output = io::read(s.out().get()).get();

  Try<JSON::Array> parse = JSON::parse<JSON::Array>(output);

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


Future<list<Docker::Container> > Docker::ps() const
{
  Try<Subprocess> s = subprocess(
      path + " ps",
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
    // TODO(benh): Include stdout and stderr in error message.
    return Failure("Failed to do 'docker ps'");
  }

  // Read to EOF.
  // TODO(benh): Read output asynchronously.
  CHECK_SOME(s.out());
  string output = io::read(s.out().get()).get();

  vector<string> lines = strings::split(output, "\n");

  // Skip the header.
  CHECK_NE(0, lines.size());
  lines.erase(lines.begin());

  list<Future<Docker::Container> > futures;

  foreach (const string& line, lines) {
    // Inspect the container.
    futures.push_back(docker.inspect(strings::split(line, "\n")[0]));
  }

  return collect(futures);
}
