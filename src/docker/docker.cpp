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
#include <stout/os.hpp>
#include <stout/result.hpp>
#include <stout/strings.hpp>

#include <stout/os/read.hpp>

#include <process/check.hpp>
#include <process/collect.hpp>
#include <process/io.hpp>

#include "common/status_utils.hpp"

#include "docker/docker.hpp"

#ifdef __linux__
#include "linux/cgroups.hpp"
#endif // __linux__

#include "slave/containerizer/isolators/cgroups/cpushare.hpp"
#include "slave/containerizer/isolators/cgroups/mem.hpp"

#include "slave/constants.hpp"

using namespace mesos;

using namespace mesos::internal::slave;

using namespace process;

using std::list;
using std::map;
using std::string;
using std::vector;


Nothing _nothing() { return Nothing(); }

template <typename T>
static Future<T> failure(
    const string& cmd,
    int status,
    const string& err)
{
  return Failure(
      "Failed to '" + cmd + "': exit status = " +
      WSTRINGIFY(status) + " stderr = " + err);
}


static Future<Nothing> _checkError(const string& cmd, const Subprocess& s)
{
  Option<int> status = s.status().get();
  if (status.isNone()) {
    return Failure("No status found for '" + cmd + "'");
  }

  if (status.get() != 0) {
    // TODO(tnachen): Consider returning stdout as well.
    CHECK_SOME(s.err());
    return io::read(s.err().get())
      .then(lambda::bind(failure<Nothing>, cmd, status.get(), lambda::_1));
  }

  return Nothing();
}


// Returns a failure if no status or non-zero status returned from
// subprocess.
static Future<Nothing> checkError(const string& cmd, const Subprocess& s)
{
  return s.status()
    .then(lambda::bind(_checkError, cmd, s));
}


Try<Nothing> Docker::validateVersion(const Version& minVersion) const
{
  // Validate the version (and that we can use Docker at all).
  Future<Version> version = this->version();

  if (!version.await(DOCKER_VERSION_WAIT_TIMEOUT)) {
    return Error("Timed out getting docker version");
  }

  if (version.isFailed()) {
    return Error("Failed to get docker version: " + version.failure());
  }

  if (version.get() < minVersion) {
    string msg = "Insufficient version '" + stringify(version.get()) +
                 "' of Docker. Please upgrade to >=' " +
                 stringify(minVersion) + "'";
    return Error(msg);
  }

  return Nothing();
}


Try<Docker*> Docker::create(const string& path, bool validate)
{
  Docker* docker = new Docker(path);
  if (!validate) {
    return docker;
  }

#ifdef __linux__
  // Make sure that cgroups are mounted, and at least the 'cpu'
  // subsystem is attached.
  Result<string> hierarchy = cgroups::hierarchy("cpu");

  if (hierarchy.isNone()) {
    delete docker;
    return Error("Failed to find a mounted cgroups hierarchy "
                 "for the 'cpu' subsystem; you probably need "
                 "to mount cgroups manually");
  }
#endif // __linux__

  Try<Nothing> validateVersion = docker->validateVersion(Version(1, 0, 0));
  if (validateVersion.isError()) {
    delete docker;
    return Error(validateVersion.error());
  }

  return docker;
}


void commandDiscarded(const Subprocess& s, const string& cmd)
{
  VLOG(1) << "'" << cmd << "' is being discarded";
  os::killtree(s.pid(), SIGKILL);
}


Future<Version> Docker::version() const
{
  string cmd = path + " --version";

  Try<Subprocess> s = subprocess(
      cmd,
      Subprocess::PATH("/dev/null"),
      Subprocess::PIPE(),
      Subprocess::PIPE());

  if (s.isError()) {
    return Failure(s.error());
  }

  return s.get().status()
    .then(lambda::bind(&Docker::_version, cmd, s.get()));
}


Future<Version> Docker::_version(const string& cmd, const Subprocess& s)
{
  const Option<int>& status = s.status().get();
  if (status.isNone() || status.get() != 0) {
    string msg = "Failed to execute '" + cmd + "': ";
    if (status.isSome()) {
      msg += WSTRINGIFY(status.get());
    } else {
      msg += "unknown exit status";
    }
    return Failure(msg);
  }

  CHECK_SOME(s.out());

  return io::read(s.out().get())
    .then(lambda::bind(&Docker::__version, lambda::_1));
}


Future<Version> Docker::__version(const Future<string>& output)
{
  vector<string> parts = strings::split(output.get(), ",");

  if (!parts.empty()) {
    vector<string> subParts = strings::split(parts.front(), " ");

    if (!subParts.empty()) {
      // Docker version output in Fedora 22 is "x.x.x.fc22" which does not match
      // the Semantic Versioning specification(<major>[.<minor>[.<patch>]]). We
      // remove the overflow components here before parsing the docker version
      // output to a Version struct.
      string versionString = subParts.back();
      vector<string> components = strings::split(versionString, ".");
      if (components.size() > 3) {
        components.erase(components.begin() + 3, components.end());
      }
      versionString = strings::join(".", components);

      Try<Version> version = Version::parse(versionString);

      if (version.isError()) {
        return Failure("Failed to parse docker version: " +
                       version.error());
      }

      return version;
    }
  }

  return Failure("Unable to find docker version in output");
}


Try<Docker::Container> Docker::Container::create(const string& output)
{
  Try<JSON::Array> parse = JSON::parse<JSON::Array>(output);
  if (parse.isError()) {
    return Error("Failed to parse JSON: " + parse.error());
  }

  // TODO(benh): Handle the case where the short container ID was
  // not sufficiently unique and 'array.values.size() > 1'.
  JSON::Array array = parse.get();
  if (array.values.size() != 1) {
    return Error("Failed to find container");
  }

  CHECK(array.values.front().is<JSON::Object>());

  JSON::Object json = array.values.front().as<JSON::Object>();

  Result<JSON::String> idValue = json.find<JSON::String>("Id");
  if (idValue.isNone()) {
    return Error("Unable to find Id in container");
  } else if (idValue.isError()) {
    return Error("Error finding Id in container: " + idValue.error());
  }

  string id = idValue.get().value;

  Result<JSON::String> nameValue = json.find<JSON::String>("Name");
  if (nameValue.isNone()) {
    return Error("Unable to find Name in container");
  } else if (nameValue.isError()) {
    return Error("Error finding Name in container: " + nameValue.error());
  }

  string name = nameValue.get().value;

  Result<JSON::Object> stateValue = json.find<JSON::Object>("State");
  if (stateValue.isNone()) {
    return Error("Unable to find State in container");
  } else if (stateValue.isError()) {
    return Error("Error finding State in container: " + stateValue.error());
  }

  Result<JSON::Number> pidValue = stateValue.get().find<JSON::Number>("Pid");
  if (pidValue.isNone()) {
    return Error("Unable to find Pid in State");
  } else if (pidValue.isError()) {
    return Error("Error finding Pid in State: " + pidValue.error());
  }

  pid_t pid = pid_t(pidValue.get().value);

  Option<pid_t> optionalPid;
  if (pid != 0) {
    optionalPid = pid;
  }

  Result<JSON::String> startedAtValue =
    stateValue.get().find<JSON::String>("StartedAt");
  if (startedAtValue.isNone()) {
    return Error("Unable to find StartedAt in State");
  } else if (startedAtValue.isError()) {
    return Error("Error finding StartedAt in State: " + startedAtValue.error());
  }

  bool started = startedAtValue.get().value != "0001-01-01T00:00:00Z";

  Result<JSON::String> ipAddressValue =
    json.find<JSON::String>("NetworkSettings.IPAddress");
  if (ipAddressValue.isNone()) {
    return Error("Unable to find NetworkSettings.IPAddress in container");
  } else if (ipAddressValue.isError()) {
    return Error(
        "Error finding NetworkSettings.Name in container: " +
        ipAddressValue.error());
  }

  string ipAddress = ipAddressValue.get().value;

  return Docker::Container(output, id, name, optionalPid, started, ipAddress);
}


Try<Docker::Image> Docker::Image::create(const JSON::Object& json)
{
  Result<JSON::Value> entrypoint =
    json.find<JSON::Value>("ContainerConfig.Entrypoint");

  if (entrypoint.isError()) {
    return Error("Failed to find 'ContainerConfig.Entrypoint': " +
                 entrypoint.error());

  } else if (entrypoint.isNone()) {
    return Error("Unable to find 'ContainerConfig.Entrypoint'");
  }

  if (entrypoint.get().is<JSON::Null>()) {
    return Docker::Image(None());
  }

  if (!entrypoint.get().is<JSON::Array>()) {
    return Error("Unexpected type found for 'ContainerConfig.Entrypoint'");
  }

  const vector<JSON::Value>& values = entrypoint.get().as<JSON::Array>().values;
  if (values.size() == 0) {
    return Docker::Image(None());
  }

  vector<string> result;

  foreach (const JSON::Value& value, values) {
    if (!value.is<JSON::String>()) {
      return Error("Expecting 'ContainerConfig.EntryPoint' array of strings");
    }
    result.push_back(value.as<JSON::String>().value);
  }

  return Docker::Image(result);
}


Future<Nothing> Docker::run(
    const ContainerInfo& containerInfo,
    const CommandInfo& commandInfo,
    const string& name,
    const string& sandboxDirectory,
    const string& mappedDirectory,
    const Option<Resources>& resources,
    const Option<map<string, string>>& env,
    const Option<string>& stdoutPath,
    const Option<string>& stderrPath) const
{
  if (!containerInfo.has_docker()) {
    return Failure("No docker info found in container info");
  }

  const ContainerInfo::DockerInfo& dockerInfo = containerInfo.docker();

  vector<string> argv;
  argv.push_back(path);
  argv.push_back("run");

  if (dockerInfo.privileged()) {
    argv.push_back("--privileged");
  }

  if (resources.isSome()) {
    // TODO(yifan): Support other resources (e.g. disk).
    Option<double> cpus = resources.get().cpus();
    if (cpus.isSome()) {
      uint64_t cpuShare =
        std::max((uint64_t) (CPU_SHARES_PER_CPU * cpus.get()), MIN_CPU_SHARES);
      argv.push_back("--cpu-shares");
      argv.push_back(stringify(cpuShare));
    }

    Option<Bytes> mem = resources.get().mem();
    if (mem.isSome()) {
      Bytes memLimit = std::max(mem.get(), MIN_MEMORY);
      argv.push_back("--memory");
      argv.push_back(stringify(memLimit.bytes()));
    }
  }

  if (env.isSome()) {
    foreachpair (string key, string value, env.get()) {
      argv.push_back("-e");
      argv.push_back(key + "=" + value);
    }
  }

  foreach (const Environment::Variable& variable,
           commandInfo.environment().variables()) {
    if (env.isSome() &&
        env.get().find(variable.name()) != env.get().end()) {
      // Skip to avoid duplicate environment variables.
      continue;
    }
    argv.push_back("-e");
    argv.push_back(variable.name() + "=" + variable.value());
  }

  argv.push_back("-e");
  argv.push_back("MESOS_SANDBOX=" + mappedDirectory);

  foreach (const Volume& volume, containerInfo.volumes()) {
    string volumeConfig = volume.container_path();
    if (volume.has_host_path()) {
      if (!strings::startsWith(volume.host_path(), "/")) {
        // Support mapping relative paths from the sandbox.
        volumeConfig =
          path::join(sandboxDirectory, volume.host_path()) + ":" + volumeConfig;
      } else {
        volumeConfig = volume.host_path() + ":" + volumeConfig;
      }
      if (volume.has_mode()) {
        switch (volume.mode()) {
          case Volume::RW: volumeConfig += ":rw"; break;
          case Volume::RO: volumeConfig += ":ro"; break;
          default: return Failure("Unsupported volume mode");
        }
      }
    } else if (volume.has_mode() && !volume.has_host_path()) {
      return Failure("Host path is required with mode");
    }

    argv.push_back("-v");
    argv.push_back(volumeConfig);
  }

  // Mapping sandbox directory into the container mapped directory.
  argv.push_back("-v");
  argv.push_back(sandboxDirectory + ":" + mappedDirectory);

  const string& image = dockerInfo.image();

  argv.push_back("--net");
  string network;
  switch (dockerInfo.network()) {
    case ContainerInfo::DockerInfo::HOST: network = "host"; break;
    case ContainerInfo::DockerInfo::BRIDGE: network = "bridge"; break;
    case ContainerInfo::DockerInfo::NONE: network = "none"; break;
    default: return Failure("Unsupported Network mode: " +
                            stringify(dockerInfo.network()));
  }

  argv.push_back(network);

  if (containerInfo.has_hostname()) {
    if (network == "host") {
      return Failure("Unable to set hostname with host network mode");
    }

    argv.push_back("--hostname");
    argv.push_back(containerInfo.hostname());
  }

  foreach (const Parameter& parameter, dockerInfo.parameters()) {
    argv.push_back("--" + parameter.key() + "=" + parameter.value());
  }

  if (dockerInfo.port_mappings().size() > 0) {
    if (network != "bridge") {
      return Failure("Port mappings are only supported for bridge network");
    }

    if (!resources.isSome()) {
      return Failure("Port mappings require resources");
    }

    Option<Value::Ranges> portRanges = resources.get().ports();

    if (!portRanges.isSome()) {
      return Failure("Port mappings require port resources");
    }

    foreach (const ContainerInfo::DockerInfo::PortMapping& mapping,
             dockerInfo.port_mappings()) {
      bool found = false;
      foreach (const Value::Range& range, portRanges.get().range()) {
        if (mapping.host_port() >= range.begin() &&
            mapping.host_port() <= range.end()) {
          found = true;
          break;
        }
      }

      if (!found) {
        return Failure("Port [" + stringify(mapping.host_port()) + "] not " +
                       "included in resources");
      }

      string portMapping = stringify(mapping.host_port()) + ":" +
                           stringify(mapping.container_port());

      if (mapping.has_protocol()) {
        portMapping += "/" + strings::lower(mapping.protocol());
      }

      argv.push_back("-p");
      argv.push_back(portMapping);
    }
  }

  if (commandInfo.shell()) {
    // We override the entrypoint if shell is enabled because we
    // assume the user intends to run the command within /bin/sh
    // and not the default entrypoint of the image. View MESOS-1770
    // for more details.
    argv.push_back("--entrypoint");
    argv.push_back("/bin/sh");
  }

  argv.push_back("--name");
  argv.push_back(name);
  argv.push_back(image);

  if (commandInfo.shell()) {
    if (!commandInfo.has_value()) {
      return Failure("Shell specified but no command value provided");
    }

    // Adding -c here because Docker cli only supports a single word
    // for overriding entrypoint, so adding the -c flag for /bin/sh
    // as part of the command.
    argv.push_back("-c");
    argv.push_back(commandInfo.value());
  } else {
    if (commandInfo.has_value()) {
      argv.push_back(commandInfo.value());
    }

    foreach (const string& argument, commandInfo.arguments()) {
      argv.push_back(argument);
    }
  }

  string cmd = strings::join(" ", argv);

  VLOG(1) << "Running " << cmd;

  map<string, string> environment = os::environment();

  // Currently the Docker CLI picks up dockerconfig by looking for
  // the config file in the $HOME directory. If one of the URIs
  // provided is a docker config file we want docker to be able to
  // pick it up from the sandbox directory where we store all the
  // URI downloads.
  environment["HOME"] = sandboxDirectory;

  Subprocess::IO stdoutIo = Subprocess::PIPE();
  Subprocess::IO stderrIo = Subprocess::PIPE();
  if (stdoutPath.isSome()) {
    stdoutIo = Subprocess::PATH(stdoutPath.get());
  }

  if (stderrPath.isSome()) {
    stderrIo = Subprocess::PATH(stderrPath.get());
  }

  Try<Subprocess> s = subprocess(
      path,
      argv,
      Subprocess::PATH("/dev/null"),
      stdoutIo,
      stderrIo,
      None(),
      environment);

  if (s.isError()) {
    return Failure(s.error());
  }

  // We don't call checkError here to avoid printing the stderr
  // of the docker container task as docker run with attach forwards
  // the container's stderr to the client's stderr.
  return s.get().status()
    .then(lambda::bind(
        &Docker::_run,
        lambda::_1))
    .onDiscard(lambda::bind(&commandDiscarded, s.get(), cmd));
}


Future<Nothing> Docker::_run(const Option<int>& status)
{
  if (status.isNone()) {
    return Failure("Failed to get exit status");
  } else if (status.get() != 0) {
    return Failure("Container exited on error: " + WSTRINGIFY(status.get()));
  }

  return Nothing();
}


Future<Nothing> Docker::stop(
    const string& containerName,
    const Duration& timeout,
    bool remove) const
{
  int timeoutSecs = (int) timeout.secs();
  if (timeoutSecs < 0) {
    return Failure("A negative timeout can not be applied to docker stop: " +
                   stringify(timeoutSecs));
  }

  string cmd = path + " stop -t " + stringify(timeoutSecs) +
               " " + containerName;

  VLOG(1) << "Running " << cmd;

  Try<Subprocess> s = subprocess(
      cmd,
      Subprocess::PATH("/dev/null"),
      Subprocess::PATH("/dev/null"),
      Subprocess::PIPE());

  if (s.isError()) {
    return Failure(s.error());
  }

  return s.get().status()
    .then(lambda::bind(
        &Docker::_stop,
        *this,
        containerName,
        cmd,
        s.get(),
        remove));
}

Future<Nothing> Docker::_stop(
    const Docker& docker,
    const string& containerName,
    const string& cmd,
    const Subprocess& s,
    bool remove)
{
  Option<int> status = s.status().get();

  if (remove) {
    bool force = !status.isSome() || status.get() != 0;
    return docker.rm(containerName, force);
  }

  return checkError(cmd, s);
}


Future<Nothing> Docker::rm(
    const string& containerName,
    bool force) const
{
  const string cmd = path + (force ? " rm -f " : " rm ") + containerName;

  VLOG(1) << "Running " << cmd;

  Try<Subprocess> s = subprocess(
      cmd,
      Subprocess::PATH("/dev/null"),
      Subprocess::PATH("/dev/null"),
      Subprocess::PIPE());

  if (s.isError()) {
    return Failure(s.error());
  }

  return checkError(cmd, s.get());
}


Future<Docker::Container> Docker::inspect(
    const string& containerName,
    const Option<Duration>& retryInterval) const
{
  Owned<Promise<Docker::Container>> promise(new Promise<Docker::Container>());

  const string cmd =  path + " inspect " + containerName;
  _inspect(cmd, promise, retryInterval);

  return promise->future();
}


void Docker::_inspect(
    const string& cmd,
    const Owned<Promise<Docker::Container>>& promise,
    const Option<Duration>& retryInterval)
{
  if (promise->future().hasDiscard()) {
    promise->discard();
    return;
  }

  VLOG(1) << "Running " << cmd;

  Try<Subprocess> s = subprocess(
      cmd,
      Subprocess::PATH("/dev/null"),
      Subprocess::PIPE(),
      Subprocess::PIPE());

  if (s.isError()) {
    promise->fail(s.error());
    return;
  }

  // Start reading from stdout so writing to the pipe won't block
  // to handle cases where the output is larger than the pipe
  // capacity.
  const Future<string> output = io::read(s.get().out().get());

  s.get().status()
    .onAny([=]() { __inspect(cmd, promise, retryInterval, output, s.get()); });
}


void Docker::__inspect(
    const string& cmd,
    const Owned<Promise<Docker::Container>>& promise,
    const Option<Duration>& retryInterval,
    Future<string> output,
    const Subprocess& s)
{
  if (promise->future().hasDiscard()) {
    promise->discard();
    output.discard();
    return;
  }

  // Check the exit status of 'docker inspect'.
  CHECK_READY(s.status());

  Option<int> status = s.status().get();

  if (!status.isSome()) {
    promise->fail("No status found from '" + cmd + "'");
  } else if (status.get() != 0) {
    output.discard();

    if (retryInterval.isSome()) {
      VLOG(1) << "Retrying inspect with non-zero status code. cmd: '"
              << cmd << "', interval: " << stringify(retryInterval.get());
      Clock::timer(retryInterval.get(),
                   [=]() { _inspect(cmd, promise, retryInterval); } );
      return;
    }

    CHECK_SOME(s.err());
    io::read(s.err().get())
      .then(lambda::bind(
                failure<Nothing>,
                cmd,
                status.get(),
                lambda::_1))
      .onAny([=](const Future<Nothing>& future) {
          CHECK_FAILED(future);
          promise->fail(future.failure());
      });
    return;
  }

  // Read to EOF.
  CHECK_SOME(s.out());
  output
    .onAny([=](const Future<string>& output) {
      ___inspect(cmd, promise, retryInterval, output);
    });
}


void Docker::___inspect(
    const string& cmd,
    const Owned<Promise<Docker::Container>>& promise,
    const Option<Duration>& retryInterval,
    const Future<string>& output)
{
  if (promise->future().hasDiscard()) {
    promise->discard();
    return;
  }

  if (!output.isReady()) {
    promise->fail(output.isFailed() ? output.failure() : "future discarded");
    return;
  }

  Try<Docker::Container> container = Docker::Container::create(
      output.get());

  if (container.isError()) {
    promise->fail("Unable to create container: " + container.error());
    return;
  }

  if (retryInterval.isSome() && !container.get().started) {
    VLOG(1) << "Retrying inspect since container not yet started. cmd: '"
            << cmd << "', interval: " << stringify(retryInterval.get());
    Clock::timer(retryInterval.get(),
                 [=]() { _inspect(cmd, promise, retryInterval); } );
    return;
  }

  promise->set(container.get());
}


Future<list<Docker::Container>> Docker::ps(
    bool all,
    const Option<string>& prefix) const
{
  string cmd = path + (all ? " ps -a" : " ps");

  VLOG(1) << "Running " << cmd;

  Try<Subprocess> s = subprocess(
      cmd,
      Subprocess::PATH("/dev/null"),
      Subprocess::PIPE(),
      Subprocess::PIPE());

  if (s.isError()) {
    return Failure(s.error());
  }

  // Start reading from stdout so writing to the pipe won't block
  // to handle cases where the output is larger than the pipe
  // capacity.
  const Future<string>& output = io::read(s.get().out().get());

  return s.get().status()
    .then(lambda::bind(&Docker::_ps, *this, cmd, s.get(), prefix, output));
}


Future<list<Docker::Container>> Docker::_ps(
    const Docker& docker,
    const string& cmd,
    const Subprocess& s,
    const Option<string>& prefix,
    Future<string> output)
{
  Option<int> status = s.status().get();

  if (!status.isSome()) {
    output.discard();
    return Failure("No status found from '" + cmd + "'");
  } else if (status.get() != 0) {
    output.discard();
    CHECK_SOME(s.err());
    return io::read(s.err().get())
      .then(lambda::bind(
                failure<list<Docker::Container>>,
                cmd,
                status.get(),
                lambda::_1));
  }

  // Read to EOF.
  return output.then(lambda::bind(&Docker::__ps, docker, prefix, lambda::_1));
}


Future<list<Docker::Container>> Docker::__ps(
    const Docker& docker,
    const Option<string>& prefix,
    const string& output)
{
  vector<string> lines = strings::tokenize(output, "\n");

  // Skip the header.
  CHECK(!lines.empty());
  lines.erase(lines.begin());

  list<Future<Docker::Container>> futures;

  foreach (const string& line, lines) {
    // Inspect the containers that we are interested in depending on
    // whether or not a 'prefix' was specified.
    vector<string> columns = strings::split(strings::trim(line), " ");
    // We expect the name column to be the last column from ps.
    string name = columns[columns.size() - 1];
    if (prefix.isNone()) {
      futures.push_back(docker.inspect(name));
    } else if (strings::startsWith(name, prefix.get())) {
      futures.push_back(docker.inspect(name));
    }
  }

  return collect(futures);
}


Future<Docker::Image> Docker::pull(
    const string& directory,
    const string& image,
    bool force) const
{
  vector<string> argv;

  string dockerImage = image;

  // Check if the specified image has a tag. Also split on "/" in case
  // the user specified a registry server (ie: localhost:5000/image)
  // to get the actual image name. If no tag was given we add a
  // 'latest' tag to avoid pulling down the repository.

  vector<string> parts = strings::split(image, "/");

  if (!strings::contains(parts.back(), ":")) {
    dockerImage += ":latest";
  }

  if (force) {
    // Skip inspect and docker pull the image.
    return Docker::__pull(*this, directory, image, path);
  }

  argv.push_back(path);
  argv.push_back("inspect");
  argv.push_back(dockerImage);

  string cmd = strings::join(" ", argv);

  VLOG(1) << "Running " << cmd;

  Try<Subprocess> s = subprocess(
      path,
      argv,
      Subprocess::PATH("/dev/null"),
      Subprocess::PIPE(),
      Subprocess::PIPE(),
      None());

  if (s.isError()) {
    return Failure("Failed to execute '" + cmd + "': " + s.error());
  }

  // Start reading from stdout so writing to the pipe won't block
  // to handle cases where the output is larger than the pipe
  // capacity.
  const Future<string> output = io::read(s.get().out().get());

  // We assume docker inspect to exit quickly and do not need to be
  // discarded.
  return s.get().status()
    .then(lambda::bind(
        &Docker::_pull,
        *this,
        s.get(),
        directory,
        dockerImage,
        path,
        output));
}


Future<Docker::Image> Docker::_pull(
    const Docker& docker,
    const Subprocess& s,
    const string& directory,
    const string& image,
    const string& path,
    Future<string> output)
{
  Option<int> status = s.status().get();
  if (status.isSome() && status.get() == 0) {
    return output
      .then(lambda::bind(&Docker::____pull, lambda::_1));
  }

  output.discard();

  return Docker::__pull(docker, directory, image, path);
}


Future<Docker::Image> Docker::__pull(
    const Docker& docker,
    const string& directory,
    const string& image,
    const string& path)
{
  vector<string> argv;
  argv.push_back(path);
  argv.push_back("pull");
  argv.push_back(image);

  string cmd = strings::join(" ", argv);

  VLOG(1) << "Running " << cmd;

  // Set HOME variable to pick up .dockercfg.
  map<string, string> environment = os::environment();

  environment["HOME"] = directory;

  Try<Subprocess> s_ = subprocess(
      path,
      argv,
      Subprocess::PATH("/dev/null"),
      Subprocess::PIPE(),
      Subprocess::PIPE(),
      None(),
      environment);

  if (s_.isError()) {
    return Failure("Failed to execute '" + cmd + "': " + s_.error());
  }

  // Docker pull can run for a long time due to large images, so
  // we allow the future to be discarded and it will kill the pull
  // process.
  return s_.get().status()
    .then(lambda::bind(
        &Docker::___pull,
        docker,
        s_.get(),
        cmd,
        directory,
        image))
    .onDiscard(lambda::bind(&commandDiscarded, s_.get(), cmd));
}


Future<Docker::Image> Docker::___pull(
    const Docker& docker,
    const Subprocess& s,
    const string& cmd,
    const string& directory,
    const string& image)
{
  Option<int> status = s.status().get();

  if (!status.isSome()) {
    return Failure("No status found from '" +  cmd + "'");
  } else if (status.get() != 0) {
    return io::read(s.err().get())
      .then(lambda::bind(&failure<Image>, cmd, status.get(), lambda::_1));
  }

  // We re-invoke Docker::pull in order to now do an 'inspect' since
  // the image should be present (see Docker::pull).
  // TODO(benh): Factor out inspect code from Docker::pull to be
  // reused rather than this (potentially infinite) recursive call.
  return docker.pull(directory, image);
}


Future<Docker::Image> Docker::____pull(
    const string& output)
{
  Try<JSON::Array> parse = JSON::parse<JSON::Array>(output);

  if (parse.isError()) {
    return Failure("Failed to parse JSON: " + parse.error());
  }

  JSON::Array array = parse.get();

  // Only return if only one image identified with name.
  if (array.values.size() == 1) {
    CHECK(array.values.front().is<JSON::Object>());

    Try<Docker::Image> image =
      Docker::Image::create(array.values.front().as<JSON::Object>());

    if (image.isError()) {
      return Failure("Unable to create image: " + image.error());
    }

    return image.get();
  }

  // TODO(tnachen): Handle the case where the short image ID was
  // not sufficiently unique and 'array.values.size() > 1'.

  return Failure("Failed to find image");
}
