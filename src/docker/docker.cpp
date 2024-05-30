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

#include <cmath>
#include <map>
#include <mutex>
#include <utility>
#include <vector>

#include <stout/error.hpp>
#include <stout/foreach.hpp>
#include <stout/json.hpp>
#include <stout/lambda.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/result.hpp>
#include <stout/strings.hpp>
#include <stout/stringify.hpp>

#include <stout/os/constants.hpp>
#include <stout/os/killtree.hpp>
#include <stout/os/read.hpp>
#include <stout/os/write.hpp>

#ifdef __WINDOWS__
#include <stout/os/windows/jobobject.hpp>
#endif // __WINDOWS__

#include <process/check.hpp>
#include <process/collect.hpp>
#include <process/io.hpp>

#ifdef __WINDOWS__
#include <process/windows/jobobject.hpp>
#endif // __WINDOWS__

#include "common/status_utils.hpp"

#include "docker/docker.hpp"

#ifdef __linux__
#include "linux/cgroups.hpp"
#endif // __linux__

#include "slave/containerizer/mesos/utils.hpp"

#include "slave/containerizer/mesos/isolators/cgroups/constants.hpp"

#include "slave/constants.hpp"

using namespace mesos;

using namespace mesos::internal::slave;

using namespace process;

using std::map;
using std::mutex;
using std::pair;
using std::shared_ptr;
using std::string;
using std::vector;

using mesos::internal::ContainerDNSInfo;


template <typename T>
static Future<T> failure(
    const string& cmd,
    int status,
    const string& err)
{
  return Failure(
      "Failed to run '" + cmd + "': " + WSTRINGIFY(status) +
      "; stderr='" + err + "'");
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


Try<Owned<Docker>> Docker::create(
    const string& path,
    const string& socket,
    bool validate,
    const Option<JSON::Object>& config)
{
#ifndef __WINDOWS__
  // TODO(hausdorff): Currently, `path::absolute` does not handle all the edge
  // cases of Windows. Revisit this when MESOS-3442 is resolved.
  //
  // NOTE: When we do come back and fix this bug, it is also worth noting that
  // on Windows an empty value of `socket` is frequently used to connect to the
  // Docker host (i.e., the user wants to connect 'npipes://', with an empty
  // socket path). A full solution should accommodate this.
  if (!path::is_absolute(socket)) {
    return Error("Invalid Docker socket path: " + socket);
  }
#endif // __WINDOWS__

  Owned<Docker> docker(new Docker(path, socket, config));
  if (!validate) {
    return docker;
  }

#ifdef __linux__
  // Make sure that cgroups are mounted, and at least the 'cpu'
  // subsystem is attached.
  Result<string> hierarchy = cgroups::hierarchy("cpu");

  if (hierarchy.isNone()) {
    return Error("Failed to find a mounted cgroups hierarchy "
                 "for the 'cpu' subsystem; you probably need "
                 "to mount cgroups manually");
  }
#endif // __linux__

  Try<Nothing> validateVersion = docker->validateVersion(Version(1, 8, 0));
  if (validateVersion.isError()) {
    return Error(validateVersion.error());
  }

  return docker;
}


void commandDiscarded(const Subprocess& s, const string& cmd)
{
  if (s.status().isPending()) {
    VLOG(1) << "'" << cmd << "' is being discarded";
    os::kill(s.pid(), SIGKILL);
  }
}


vector<Subprocess::ParentHook> createParentHooks()
{
  return {
#ifdef __WINDOWS__
  // To correctly discard the docker cli process tree in `commandDiscarded`,
  // we need to wrap the process in a job object.
  Subprocess::ParentHook::CREATE_JOB(),
  Subprocess::ParentHook(&os::set_job_kill_on_close_limit),
#endif // __WINDOWS__
  };
}


Future<Version> Docker::version() const
{
  string cmd = path + " -H " + socket + " --version";

  Try<Subprocess> s = subprocess(
      cmd,
      Subprocess::PATH(os::DEV_NULL),
      Subprocess::PIPE(),
      Subprocess::PIPE(),
      None(),
      None(),
      createParentHooks());

  if (s.isError()) {
    return Failure("Failed to create subprocess '" + cmd + "': " + s.error());
  }

  return s->status()
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


// TODO(josephw): Parse this string with a protobuf.
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

  string id = idValue->value;

  Result<JSON::String> nameValue = json.find<JSON::String>("Name");
  if (nameValue.isNone()) {
    return Error("Unable to find Name in container");
  } else if (nameValue.isError()) {
    return Error("Error finding Name in container: " + nameValue.error());
  }

  string name = nameValue->value;

  Result<JSON::Object> stateValue = json.find<JSON::Object>("State");
  if (stateValue.isNone()) {
    return Error("Unable to find State in container");
  } else if (stateValue.isError()) {
    return Error("Error finding State in container: " + stateValue.error());
  }

  Result<JSON::Number> pidValue = stateValue->find<JSON::Number>("Pid");
  if (pidValue.isNone()) {
    return Error("Unable to find Pid in State");
  } else if (pidValue.isError()) {
    return Error("Error finding Pid in State: " + pidValue.error());
  }

  pid_t pid = pid_t(pidValue->as<int64_t>());

  Option<pid_t> optionalPid;
  if (pid != 0) {
    optionalPid = pid;
  }

  Result<JSON::String> startedAtValue =
    stateValue->find<JSON::String>("StartedAt");
  if (startedAtValue.isNone()) {
    return Error("Unable to find StartedAt in State");
  } else if (startedAtValue.isError()) {
    return Error("Error finding StartedAt in State: " + startedAtValue.error());
  }

  bool started = startedAtValue->value != "0001-01-01T00:00:00Z";

  Option<string> ipAddress;
  Option<string> ip6Address;
  bool findDeprecatedIP = false;

  Result<JSON::String> networkMode =
    json.find<JSON::String>("HostConfig.NetworkMode");

  if (!networkMode.isSome()) {
    // We need to fallback to the old field as Docker added NetworkMode
    // since Docker remote API 1.15.
    VLOG(1) << "Unable to detect HostConfig.NetworkMode, "
            << "attempting deprecated IP field";
    findDeprecatedIP = true;
  } else {
    // We currently rely on the fact that we always set --net when
    // we shell out to docker run, and therefore the network mode
    // matches what --net is. Without --net, the network mode would be set
    // to 'default' and we won't be able to find the IP address as
    // it will be in 'Networks.bridge' key.
    string addressLocation = "NetworkSettings.Networks." +
                             networkMode->value + ".IPAddress";

    Result<JSON::String> ipAddressValue =
      json.find<JSON::String>(addressLocation);

    if (!ipAddressValue.isSome()) {
      // We also need to fallback to the old field as the IP Address
      // field location also changed since Docker remote API 1.20.
      VLOG(1) << "Unable to detect IP Address at '" << addressLocation << "',"
              << " attempting deprecated field";
      findDeprecatedIP = true;
    } else if (!ipAddressValue->value.empty()) {
      ipAddress = ipAddressValue->value;
    }

    // Check if the container has an IPv6 address.
    //
    // NOTE: For IPv6 we don't need to worry about the old method of
    // looking at the deprecated "NetworkSettings.IPAddress" since we
    // want to support IPv6 addresses for docker versions that support
    // USER mode networking, which is a relatively recent feature.
    string address6Location = "NetworkSettings.Networks." +
                              networkMode->value + ".GlobalIPv6Address";

    Result<JSON::String> ip6AddressValue =
      json.find<JSON::String>(address6Location);

    if (ip6AddressValue.isSome() && !ip6AddressValue->value.empty()) {
      ip6Address = ip6AddressValue->value;
    }
  }

  if (findDeprecatedIP) {
    Result<JSON::String> ipAddressValue =
      json.find<JSON::String>("NetworkSettings.IPAddress");

    if (ipAddressValue.isNone()) {
      return Error("Unable to find NetworkSettings.IPAddress in container");
    } else if (ipAddressValue.isError()) {
      return Error(
        "Error finding NetworkSettings.IPAddress in container: " +
        ipAddressValue.error());
    } else if (!ipAddressValue->value.empty()) {
      ipAddress = ipAddressValue->value;
    }
  }

  vector<Device> devices;

  Result<JSON::Array> devicesArray =
    json.find<JSON::Array>("HostConfig.Devices");

  if (devicesArray.isError()) {
    return Error("Failed to parse HostConfig.Devices: " + devicesArray.error());
  }

  if (devicesArray.isSome()) {
    foreach (const JSON::Value& entry, devicesArray->values) {
      if (!entry.is<JSON::Object>()) {
        return Error("Malformed HostConfig.Devices"
                     " entry '" + stringify(entry) + "'");
      }

      JSON::Object object = entry.as<JSON::Object>();

      Result<JSON::String> hostPath =
        object.at<JSON::String>("PathOnHost");
      Result<JSON::String> containerPath =
        object.at<JSON::String>("PathInContainer");
      Result<JSON::String> permissions =
        object.at<JSON::String>("CgroupPermissions");

      if (!hostPath.isSome() ||
          !containerPath.isSome() ||
          !permissions.isSome()) {
        return Error("Malformed HostConfig.Devices entry"
                     " '" + stringify(object) + "'");
      }

      Device device;
      device.hostPath = Path(hostPath->value);
      device.containerPath = Path(containerPath->value);
      device.access.read = strings::contains(permissions->value, "r");
      device.access.write = strings::contains(permissions->value, "w");
      device.access.mknod = strings::contains(permissions->value, "m");

      devices.push_back(device);
    }
  }

  vector<string> dns;

  Result<JSON::Array> dnsArray =
    json.find<JSON::Array>("HostConfig.Dns");

  if (dnsArray.isError()) {
    return Error("Failed to parse HostConfig.Dns: " + dnsArray.error());
  }

  if (dnsArray.isSome()) {
    foreach (const JSON::Value& entry, dnsArray->values) {
      if (!entry.is<JSON::String>()) {
        return Error("Malformed HostConfig.Dns"
                     " entry '" + stringify(entry) + "'");
      }

      dns.push_back(entry.as<JSON::String>().value);
    }
  }

  vector<string> dnsOptions;

  Result<JSON::Array> dnsOptionArray =
    json.find<JSON::Array>("HostConfig.DnsOptions");

  if (dnsOptionArray.isError()) {
    return Error("Failed to parse HostConfig.DnsOptions: " +
                 dnsOptionArray.error());
  }

  if (dnsOptionArray.isSome()) {
    foreach (const JSON::Value& entry, dnsOptionArray->values) {
      if (!entry.is<JSON::String>()) {
        return Error("Malformed HostConfig.DnsOptions"
                     " entry '" + stringify(entry) + "'");
      }

      dnsOptions.push_back(entry.as<JSON::String>().value);
    }
  }

  vector<string> dnsSearch;

  Result<JSON::Array> dnsSearchArray =
    json.find<JSON::Array>("HostConfig.DnsSearch");

  if (dnsSearchArray.isError()) {
    return Error("Failed to parse HostConfig.DnsSearch: " +
                 dnsSearchArray.error());
  }

  if (dnsSearchArray.isSome()) {
    foreach (const JSON::Value& entry, dnsSearchArray->values) {
      if (!entry.is<JSON::String>()) {
        return Error("Malformed HostConfig.DnsSearch"
                     " entry '" + stringify(entry) + "'");
      }

      dnsSearch.push_back(entry.as<JSON::String>().value);
    }
  }

  return Container(
      output,
      id,
      name,
      optionalPid,
      started,
      ipAddress,
      ip6Address,
      devices,
      dns,
      dnsOptions,
      dnsSearch);
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

  Option<vector<string>> entrypointOption = None();

  if (!entrypoint->is<JSON::Null>()) {
    if (!entrypoint->is<JSON::Array>()) {
      return Error("Unexpected type found for 'ContainerConfig.Entrypoint'");
    }

    const vector<JSON::Value>& values = entrypoint->as<JSON::Array>().values;
    if (values.size() != 0) {
      vector<string> result;

      foreach (const JSON::Value& value, values) {
        if (!value.is<JSON::String>()) {
          return Error("Expecting entrypoint value to be type string");
        }
        result.push_back(value.as<JSON::String>().value);
      }

      entrypointOption = result;
    }
  }

  Result<JSON::Value> env =
    json.find<JSON::Value>("ContainerConfig.Env");

  if (env.isError()) {
    return Error("Failed to find 'ContainerConfig.Env': " +
                 env.error());
  } else if (env.isNone()) {
    return Error("Unable to find 'ContainerConfig.Env'");
  }

  Option<map<string, string>> envOption = None();

  if (!env->is<JSON::Null>()) {
    if (!env->is<JSON::Array>()) {
      return Error("Unexpected type found for 'ContainerConfig.Env'");
    }

    const vector<JSON::Value>& values = env->as<JSON::Array>().values;
    if (values.size() != 0) {
      map<string, string> result;

      foreach (const JSON::Value& value, values) {
        if (!value.is<JSON::String>()) {
          return Error("Expecting environment value to be type string");
        }

        const vector<string> tokens =
          strings::split(value.as<JSON::String>().value, "=", 2);

        if (tokens.size() != 2) {
          return Error("Unexpected Env format for 'ContainerConfig.Env'");
        }

        if (result.count(tokens[0]) > 0) {
          return Error("Unexpected duplicate environment variables '"
                        + tokens[0] + "'");
        }

        result[tokens[0]] = tokens[1];
      }

      envOption = result;
    }
  }

  return Docker::Image(entrypointOption, envOption);
}


Try<Docker::RunOptions> Docker::RunOptions::create(
    const ContainerInfo& containerInfo,
    const CommandInfo& commandInfo,
    const string& name,
    const string& sandboxDirectory,
    const string& mappedDirectory,
    const Option<Resources>& resourceRequests,
    bool enableCfsQuota,
    const Option<map<string, string>>& env,
    const Option<vector<Device>>& devices,
    const Option<ContainerDNSInfo>& defaultContainerDNS,
    const Option<google::protobuf::Map<string, Value::Scalar>>& resourceLimits)
{
  if (!containerInfo.has_docker()) {
    return Error("No docker info found in container info");
  }

  const ContainerInfo::DockerInfo& dockerInfo = containerInfo.docker();

  RunOptions options;
  options.privileged = dockerInfo.privileged();

  Option<double> cpuRequest, cpuLimit, memLimit;
  Option<Bytes> memRequest;

  if (resourceRequests.isSome()) {
    // TODO(yifan): Support other resources (e.g. disk).
    cpuRequest = resourceRequests->cpus();
    memRequest = resourceRequests->mem();
  }

  if (resourceLimits.isSome()) {
    foreach (auto&& limit, resourceLimits.get()) {
      if (limit.first == "cpus") {
        cpuLimit = limit.second.value();
      } else if (limit.first == "mem") {
        memLimit = limit.second.value();
      }
    }
  }

  if (cpuRequest.isSome()) {
    options.cpuShares = std::max(
        static_cast<uint64_t>(CPU_SHARES_PER_CPU * cpuRequest.get()),
        MIN_CPU_SHARES);
  }

  // Set the `--cpu-quota` option to CPU limit (if it is not an infinite
  // value) or to CPU request if the flag `--cgroups_enable_cfs` is true.
  // If CPU limit is infinite, `--cpu-quota` will not be set at all which
  // means the Docker container will run with infinite CPU quota.
  if (cpuLimit.isSome()) {
    if (!std::isinf(cpuLimit.get())) {
      const Duration quota =
        std::max(CPU_CFS_PERIOD * cpuLimit.get(), MIN_CPU_CFS_QUOTA);

      options.cpuQuota = static_cast<uint64_t>(quota.us());
    }
  } else if (enableCfsQuota && cpuRequest.isSome()) {
    const Duration quota =
      std::max(CPU_CFS_PERIOD * cpuRequest.get(), MIN_CPU_CFS_QUOTA);

    options.cpuQuota = static_cast<uint64_t>(quota.us());
  }

  // Set the `--memory` option to memory limit (if it is not an infinite
  // value) or to memory request. If memory limits is infinite, `--memory`
  // will not be set at all which means the Docker container will run with
  // infinite memory limit.
  if (memLimit.isSome()) {
    if (!std::isinf(memLimit.get())) {
      options.memory =
        std::max(Megabytes(static_cast<uint64_t>(memLimit.get())), MIN_MEMORY);
    }

    if (memRequest.isSome()) {
      options.memoryReservation = std::max(memRequest.get(), MIN_MEMORY);

      if (std::isinf(memLimit.get()) ||
          memRequest.get() < Megabytes(static_cast<uint64_t>(memLimit.get()))) {
        Try<int> oomScoreAdj = calculateOOMScoreAdj(memRequest.get());
        if (oomScoreAdj.isError()) {
          return Error(
              "Failed to calculate OOM score adjustment: " +
              oomScoreAdj.error());
        }

        options.oomScoreAdj = oomScoreAdj.get();
      }
    }
  } else if (memRequest.isSome()) {
    options.memory = std::max(memRequest.get(), MIN_MEMORY);
  }

  if (env.isSome()) {
    foreachpair (const string& key, const string& value, env.get()) {
      options.env[key] = value;
    }
  }

  foreach (const Environment::Variable& variable,
           commandInfo.environment().variables()) {
    if (env.isSome() &&
        env->find(variable.name()) != env->end()) {
      // Skip to avoid duplicate environment variables.
      continue;
    }
    options.env[variable.name()] = variable.value();
  }

  options.env["MESOS_SANDBOX"] = mappedDirectory;
  options.env["MESOS_CONTAINER_NAME"] = name;

  if (resourceRequests.isSome()) {
    // Set the `MESOS_ALLOCATION_ROLE` environment variable. Please note
    // that tasks and executors are not allowed to mix resources allocated
    // to different roles, see MESOS-6636.
    const Resource resource = *resourceRequests->begin();
    options.env["MESOS_ALLOCATION_ROLE"] = resource.allocation_info().role();
  }

  Option<string> volumeDriver;
  foreach (const Volume& volume, containerInfo.volumes()) {
    // The 'container_path' can be either an absolute path or a
    // relative path. If it is a relative path, it would be prefixed
    // with the container sandbox directory.
    string volumeConfig = path::is_absolute(volume.container_path())
      ? volume.container_path()
      : path::join(mappedDirectory, volume.container_path());

    // TODO(gyliu513): Set `host_path` as source.
    if (volume.has_host_path()) {
      // If both 'host_path' and 'container_path' are relative paths,
      // return a failure because the user can just directly access the
      // volume in the sandbox.
      if (!path::is_absolute(volume.host_path()) &&
          !path::is_absolute(volume.container_path())) {
        return Error(
            "Both host_path '" + volume.host_path() + "' " +
            "and container_path '" + volume.container_path() + "' " +
            "of a volume are relative");
      }

      if (!path::is_absolute(volume.host_path()) &&
          !dockerInfo.has_volume_driver()) {
        // When volume driver is empty and host path is a relative path, mapping
        // host path from the sandbox.
        volumeConfig =
          path::join(sandboxDirectory, volume.host_path()) + ":" + volumeConfig;
      } else {
        volumeConfig = volume.host_path() + ":" + volumeConfig;
      }

      switch (volume.mode()) {
        case Volume::RW: volumeConfig += ":rw"; break;
        case Volume::RO: volumeConfig += ":ro"; break;
        default: return Error("Unsupported volume mode");
      }
    } else if (volume.has_source()) {
      if (volume.source().type() != Volume::Source::DOCKER_VOLUME) {
        VLOG(1) << "Ignored volume type '" << volume.source().type()
                << "' for container '" << name << "' as only "
                << "'DOCKER_VOLUME' was supported by docker";
        continue;
      }

      volumeConfig = volume.source().docker_volume().name() +
                     ":" + volumeConfig;

      if (volume.source().docker_volume().has_driver()) {
        const string& currentDriver = volume.source().docker_volume().driver();

        if (volumeDriver.isSome() &&
            volumeDriver.get() != currentDriver) {
          return Error("Only one volume driver is supported");
        }

        volumeDriver = currentDriver;
      }

      switch (volume.mode()) {
        case Volume::RW: volumeConfig += ":rw"; break;
        case Volume::RO: volumeConfig += ":ro"; break;
        default: return Error("Unsupported volume mode");
      }
    } else {
      return Error("Host path or volume source is required");
    }

    options.volumes.push_back(volumeConfig);
  }

  // Mapping sandbox directory into the container mapped directory.
  options.volumes.push_back(sandboxDirectory + ":" + mappedDirectory);

  // TODO(gyliu513): Deprecate this after the release cycle of 1.0.
  // It will be replaced by Volume.Source.DockerVolume.driver.
  if (dockerInfo.has_volume_driver()) {
    if (volumeDriver.isSome() &&
        volumeDriver.get() != dockerInfo.volume_driver()) {
      return Error("Only one volume driver per task is supported");
    }

    volumeDriver = dockerInfo.volume_driver();
  }

  options.volumeDriver = volumeDriver;

  ContainerInfo::DockerInfo::Network network;
  if (dockerInfo.has_network()) {
    network = dockerInfo.network();
  } else {
    // If no network was given, then use the OS specific default.
#ifdef __WINDOWS__
    network = ContainerInfo::DockerInfo::BRIDGE;
#else
    network = ContainerInfo::DockerInfo::HOST;
#endif // __WINDOWS__
  }

  // See https://docs.microsoft.com/en-us/virtualization/windowscontainers/manage-containers/container-networking // NOLINT(whitespace/line_length)
  // and https://docs.docker.com/engine/userguide/networking/ on what network
  // modes are supported for Windows and Linux docker respectively.
  switch (network) {
    case ContainerInfo::DockerInfo::HOST: {
#ifdef __WINDOWS__
      return Error("Unsupported Network mode: " + stringify(network));
#else
      options.network = "host";
      break;
#endif // __WINDOWS__
    }
    case ContainerInfo::DockerInfo::BRIDGE: {
#ifdef __WINDOWS__
      // Windows "nat" network mode is equivalent to Linux "bridge" mode.
      options.network = "nat";
#else
      options.network = "bridge";
#endif // __WINDOWS__
      break;
    }
    case ContainerInfo::DockerInfo::NONE: {
      options.network = "none";
      break;
    }
    case ContainerInfo::DockerInfo::USER: {
      if (containerInfo.network_infos_size() == 0) {
        return Error("No network info found in container info");
      }

      if (containerInfo.network_infos_size() > 1) {
        return Error("Only a single network can be defined in Docker run");
      }

      const NetworkInfo& networkInfo = containerInfo.network_infos(0);
      if (!networkInfo.has_name()) {
        return Error("No network name found in network info");
      }

      options.network = networkInfo.name();
      break;
    }
    default: return Error("Unsupported Network mode: " + stringify(network));
  }

  if (containerInfo.has_hostname()) {
    if (options.network.isSome() && options.network.get() == "host") {
      return Error("Unable to set hostname with host network mode");
    }

    options.hostname = containerInfo.hostname();
  }

  if (dockerInfo.port_mappings().size() > 0) {
    if (options.network.isSome() &&
        (options.network.get() == "host" || options.network.get() == "none")) {
      return Error("Port mappings are only supported for bridge and "
                   "user-defined networks");
    }

    if (!resourceRequests.isSome()) {
      return Error("Port mappings require resources");
    }

    Option<Value::Ranges> portRanges = resourceRequests->ports();

    if (!portRanges.isSome()) {
      return Error("Port mappings require port resources");
    }

    foreach (const ContainerInfo::DockerInfo::PortMapping& mapping,
             dockerInfo.port_mappings()) {
      bool found = false;
      foreach (const Value::Range& range, portRanges->range()) {
        if (mapping.host_port() >= range.begin() &&
            mapping.host_port() <= range.end()) {
          found = true;
          break;
        }
      }

      if (!found) {
        return Error("Port [" + stringify(mapping.host_port()) + "] not " +
                     "included in resources");
      }

      Docker::PortMapping portMapping;
      portMapping.hostPort = mapping.host_port();
      portMapping.containerPort = mapping.container_port();

      if (mapping.has_protocol()) {
        portMapping.protocol = mapping.protocol();
      }

      options.portMappings.push_back(portMapping);
    }
  }

  if (devices.isSome()) {
    options.devices = devices.get();
  }

  options.name = name;

  bool dnsSpecified = false;
  foreach (const Parameter& parameter, dockerInfo.parameters()) {
    options.additionalOptions.push_back(
        "--" + parameter.key() + "=" + parameter.value());

    // In Docker 1.13.0, `--dns-option` was added and `--dns-opt` was hidden
    // (but it can still be used), so here we need to check both of them.
    if (!dnsSpecified &&
        (parameter.key() == "dns" ||
         parameter.key() == "dns-search" ||
         parameter.key() == "dns-opt" ||
         parameter.key() == "dns-option")) {
      dnsSpecified = true;
    }
  }

  if (!dnsSpecified && defaultContainerDNS.isSome()) {
    Option<ContainerDNSInfo::DockerInfo> bridgeDNS;
    Option<ContainerDNSInfo::DockerInfo> defaultUserDNS;
    hashmap<string, ContainerDNSInfo::DockerInfo> userDNSMap;

    foreach (const ContainerDNSInfo::DockerInfo& dnsInfo,
             defaultContainerDNS->docker()) {
      // Currently we only support setting DNS for containers which join
      // Docker bridge network or user-defined network.
      if (dnsInfo.network_mode() == ContainerDNSInfo::DockerInfo::BRIDGE) {
        bridgeDNS = dnsInfo;
      } else if (dnsInfo.network_mode() == ContainerDNSInfo::DockerInfo::USER) {
        if (!dnsInfo.has_network_name()) {
          // The DNS info which has network node set as `USER` and has no
          // network name set is considered as the default DNS for all
          // user-defined networks. It applies to the Docker container which
          // joins a user-defined network but that network can not be found in
          // `defaultContainerDNS`.
          defaultUserDNS = dnsInfo;
        } else {
          userDNSMap[dnsInfo.network_name()] = dnsInfo;
        }
      }
    }

    auto setDNSInfo = [&](const ContainerDNSInfo::DockerInfo& dnsInfo) {
      options.dns.assign(
          dnsInfo.dns().nameservers().begin(),
          dnsInfo.dns().nameservers().end());

      options.dnsSearch.assign(
          dnsInfo.dns().search().begin(),
          dnsInfo.dns().search().end());

      options.dnsOpt.assign(
          dnsInfo.dns().options().begin(),
          dnsInfo.dns().options().end());
    };

    if (dockerInfo.network() == ContainerInfo::DockerInfo::BRIDGE &&
        bridgeDNS.isSome()) {
      setDNSInfo(bridgeDNS.get());
    } else if (dockerInfo.network() == ContainerInfo::DockerInfo::USER) {
      if (userDNSMap.contains(options.network.get())) {
        setDNSInfo(userDNSMap.at(options.network.get()));
      } else if (defaultUserDNS.isSome()) {
        setDNSInfo(defaultUserDNS.get());
      }
    }
  }

  options.image = dockerInfo.image();

  if (commandInfo.shell()) {
    // We override the entrypoint if shell is enabled because we
    // assume the user intends to run the command within a shell
    // and not the default entrypoint of the image. View MESOS-1770
    // for more details.
#ifdef __WINDOWS__
    options.entrypoint = "cmd";
#else
    options.entrypoint = "/bin/sh";
#endif // __WINDOWS__
  }

  if (commandInfo.shell()) {
    if (!commandInfo.has_value()) {
      return Error("Shell specified but no command value provided");
    }

    // The Docker CLI only supports a single word for overriding the
    // entrypoint, so we must specify `-c` (or `/c` on Windows)
    // for the other parts of the command.
#ifdef __WINDOWS__
    options.arguments.push_back("/c");
#else
    options.arguments.push_back("-c");
#endif // __WINDOWS__

    options.arguments.push_back(commandInfo.value());
  } else {
    if (commandInfo.has_value()) {
      options.arguments.push_back(commandInfo.value());
    }

    foreach (const string& argument, commandInfo.arguments()) {
      options.arguments.push_back(argument);
    }
  }

  return options;
}


Future<Option<int>> Docker::run(
    const Docker::RunOptions& options,
    const process::Subprocess::IO& _stdout,
    const process::Subprocess::IO& _stderr) const
{
  vector<string> argv;
  argv.push_back(path);
  argv.push_back("-H");
  argv.push_back(socket);
  argv.push_back("run");

  if (options.privileged) {
    argv.push_back("--privileged");
  }

  if (options.cpuShares.isSome()) {
    argv.push_back("--cpu-shares");
    argv.push_back(stringify(options.cpuShares.get()));
  }

  if (options.cpuQuota.isSome()) {
    argv.push_back("--cpu-quota");
    argv.push_back(stringify(options.cpuQuota.get()));
  }

  if (options.memoryReservation.isSome()) {
    argv.push_back("--memory-reservation");
    argv.push_back(stringify(options.memoryReservation->bytes()));
  }

  if (options.memory.isSome()) {
    argv.push_back("--memory");
    argv.push_back(stringify(options.memory->bytes()));
  }

  if (options.oomScoreAdj.isSome()) {
    argv.push_back("--oom-score-adj");
    argv.push_back(stringify(options.oomScoreAdj.get()));
  }

  foreachpair(const string& key, const string& value, options.env) {
    argv.push_back("-e");
    argv.push_back(key + "=" + value);
  }

  foreach(const string& volume, options.volumes) {
    argv.push_back("-v");
    argv.push_back(volume);
  }

  if (options.volumeDriver.isSome()) {
    argv.push_back("--volume-driver=" + options.volumeDriver.get());
  }

  if (options.network.isSome()) {
    const string& network = options.network.get();
    argv.push_back("--net");
    argv.push_back(network);

    if (network != "host" &&
        network != "bridge" &&
        network != "none") {
      // User defined networks require Docker version >= 1.9.0.
      Try<Nothing> validateVer = validateVersion(Version(1, 9, 0));
      if (validateVer.isError()) {
        return Failure("User defined networks require Docker "
                       "version 1.9.0 or higher");
      }
    }

    if (network == "host" && !options.dns.empty()) {
      // `--dns` option with host network requires Docker version >= 1.12.0,
      // see https://github.com/moby/moby/pull/22408 for details.
      Try<Nothing> validateVer = validateVersion(Version(1, 12, 0));
      if (validateVer.isError()) {
        return Failure("--dns option with host network requires Docker "
                       "version 1.12.0 or higher");
      }
    }
  }

  foreach (const string& dns, options.dns) {
    argv.push_back("--dns");
    argv.push_back(dns);
  }

  foreach (const string& search, options.dnsSearch) {
    argv.push_back("--dns-search");
    argv.push_back(search);
  }

  if (!options.dnsOpt.empty()) {
    // `--dns-opt` option requires Docker version >= 1.9.0,
    // see https://github.com/moby/moby/pull/16031 for details.
    Try<Nothing> validateVer = validateVersion(Version(1, 9, 0));
    if (validateVer.isError()) {
      return Failure("--dns-opt option requires Docker "
                     "version 1.9.0 or higher");
    }
  }

  foreach (const string& opt, options.dnsOpt) {
    argv.push_back("--dns-opt");
    argv.push_back(opt);
  }

  if (options.hostname.isSome()) {
    argv.push_back("--hostname");
    argv.push_back(options.hostname.get());
  }

  foreach (const Docker::PortMapping& mapping, options.portMappings) {
    argv.push_back("-p");

    string portMapping = stringify(mapping.hostPort) + ":" +
                         stringify(mapping.containerPort);

    if (mapping.protocol.isSome()) {
      portMapping += "/" + strings::lower(mapping.protocol.get());
    }

    argv.push_back(portMapping);
  }

  foreach (const Device& device, options.devices) {
    if (!device.hostPath.is_absolute()) {
      return Failure("Device path '" + device.hostPath.string() + "'"
                     " is not an absolute path");
    }

    string permissions;
    permissions += device.access.read ? "r" : "";
    permissions += device.access.write ? "w" : "";
    permissions += device.access.mknod ? "m" : "";

    // Docker doesn't handle this case (it fails by saying
    // that an absolute path is not being provided).
    if (permissions.empty()) {
      return Failure("At least one access required for --devices:"
                     " none specified for"
                     " '" + device.hostPath.string() + "'");
    }

    // Note that docker silently does not handle default devices
    // passed in with restricted permissions (e.g. /dev/null), so
    // we don't bother checking this case either.
    argv.push_back(
        "--device=" +
        device.hostPath.string() + ":" +
        device.containerPath.string() + ":" +
        permissions);
  }

  if (options.entrypoint.isSome()) {
    argv.push_back("--entrypoint");
    argv.push_back(options.entrypoint.get());
  }

  if (options.name.isSome()) {
    argv.push_back("--name");
    argv.push_back(options.name.get());
  }

  foreach (const string& option, options.additionalOptions) {
    argv.push_back(option);
  }

  argv.push_back(options.image);

  foreach(const string& argument, options.arguments) {
    argv.push_back(argument);
  }

  string cmd = strings::join(" ", argv);

  VLOG(1) << "Running " << cmd;

  Try<Subprocess> s = subprocess(
      path,
      argv,
      Subprocess::PATH(os::DEV_NULL),
      _stdout,
      _stderr,
      nullptr,
      None(),
      None(),
      createParentHooks());

  if (s.isError()) {
    return Failure("Failed to create subprocess '" + path + "': " + s.error());
  }

  s->status().onDiscard(lambda::bind(&commandDiscarded, s.get(), cmd));

  // Ideally we could capture the stderr when docker itself fails,
  // however due to the stderr redirection used here we cannot.
  //
  // TODO(bmahler): Determine a way to redirect stderr while still
  // capturing the stderr when 'docker run' itself fails. E.g. we
  // could use 'docker logs' in conjunction with a "detached" form
  // of 'docker run' to isolate 'docker run' failure messages from
  // the container stderr.
  return s->status();
}

// NOTE: A known issue in Docker 1.12/1.13 sometimes leaks its mount
// namespace, causing `docker rm` to fail. As a workaround, we do a
// best-effort `docker rm` and log the error insteaf of return a
// failure when `remove` is set to true (MESOS-7777).
Future<Nothing> Docker::stop(
    const string& containerName,
    const Duration& timeout,
    bool remove) const
{
  int timeoutSecs = (int) timeout.secs();
  if (timeoutSecs < 0) {
    return Failure("A negative timeout cannot be applied to docker stop: " +
                   stringify(timeoutSecs));
  }

  vector<string> argv;
  argv.push_back(path);
  argv.push_back("-H");
  argv.push_back(socket);
  argv.push_back("stop");
  argv.push_back("-t");
  argv.push_back(stringify(timeoutSecs));
  argv.push_back(containerName);

  const string cmd = strings::join(" ", argv);

  VLOG(1) << "Running " << cmd;

  Try<Subprocess> s = subprocess(
      path,
      argv,
      Subprocess::PATH(os::DEV_NULL),
      Subprocess::PATH(os::DEV_NULL),
      Subprocess::PIPE(),
      nullptr,
      None(),
      None(),
      createParentHooks());

  if (s.isError()) {
    return Failure("Failed to create subprocess '" + cmd + "': " + s.error());
  }

  return s->status()
    .then(lambda::bind(
        &Docker::_stop,
        *this,
        containerName,
        cmd,
        s.get(),
        remove))
    .onDiscard(lambda::bind(&commandDiscarded, s.get(), cmd));
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
    return docker.rm(containerName, force)
      .repair([=](const Future<Nothing>& future) {
        LOG(ERROR) << "Unable to remove Docker container '"
                   << containerName + "': " << future.failure();
        return Nothing();
      });
  }

  return checkError(cmd, s);
}


Future<Nothing> Docker::kill(
    const string& containerName,
    int signal) const
{
  vector<string> argv;
  argv.push_back(path);
  argv.push_back("-H");
  argv.push_back(socket);
  argv.push_back("kill");
  argv.push_back("--signal=" + stringify(signal));
  argv.push_back(containerName);

  const string cmd = strings::join(" ", argv);

  VLOG(1) << "Running " << cmd;

  Try<Subprocess> s = subprocess(
      path,
      argv,
      Subprocess::PATH(os::DEV_NULL),
      Subprocess::PATH(os::DEV_NULL),
      Subprocess::PIPE(),
      nullptr,
      None(),
      None(),
      createParentHooks());

  if (s.isError()) {
    return Failure("Failed to create subprocess '" + cmd + "': " + s.error());
  }

  return checkError(cmd, s.get());
}


Future<Nothing> Docker::rm(
    const string& containerName,
    bool force) const
{
  // The `-v` flag removes Docker volumes that may be present.
  vector<string> argv;
  argv.push_back(path);
  argv.push_back("-H");
  argv.push_back(socket);
  argv.push_back("rm");

  if (force) {
    argv.push_back("-f");
  }

  argv.push_back("-v");
  argv.push_back(containerName);

  const string cmd = strings::join(" ", argv);

  VLOG(1) << "Running " << cmd;

  Try<Subprocess> s = subprocess(
      path,
      argv,
      Subprocess::PATH(os::DEV_NULL),
      Subprocess::PATH(os::DEV_NULL),
      Subprocess::PIPE(),
      nullptr,
      None(),
      None(),
      createParentHooks());

  if (s.isError()) {
    return Failure("Failed to create subprocess '" + cmd + "': " + s.error());
  }

  return checkError(cmd, s.get());
}


Future<Docker::Container> Docker::inspect(
    const string& containerName,
    const Option<Duration>& retryInterval) const
{
  Owned<Promise<Docker::Container>> promise(new Promise<Docker::Container>());

  // Holds a callback used for cleanup in case this call to 'docker inspect' is
  // discarded, and a mutex to control access to the callback.
  auto callback = std::make_shared<pair<lambda::function<void()>, mutex>>();

  vector<string> argv;
  argv.push_back(path);
  argv.push_back("-H");
  argv.push_back(socket);
  argv.push_back("inspect");
  argv.push_back("--type=container");
  argv.push_back(containerName);

  _inspect(argv, promise, retryInterval, callback);

  return promise->future()
    .onDiscard([callback]() {
      synchronized (callback->second) {
        callback->first();
      }
    });
}


void Docker::_inspect(
    const vector<string>& argv,
    const Owned<Promise<Docker::Container>>& promise,
    const Option<Duration>& retryInterval,
    shared_ptr<pair<lambda::function<void()>, mutex>> callback)
{
  if (promise->future().hasDiscard()) {
    return;
  }

  const string cmd = strings::join(" ", argv);

  VLOG(1) << "Running " << cmd;

  Try<Subprocess> s = subprocess(
      argv[0],
      argv,
      Subprocess::PATH(os::DEV_NULL),
      Subprocess::PIPE(),
      Subprocess::PIPE(),
      nullptr,
      None(),
      None(),
      createParentHooks());

  if (s.isError()) {
    promise->fail("Failed to create subprocess '" + cmd + "': " + s.error());
    return;
  }

  // Set the `onDiscard` callback which will clean up the subprocess if the
  // caller discards the `Future` that we returned.
  synchronized (callback->second) {
    // It's possible that the caller has discarded their future while we were
    // creating a new subprocess, so we clean up here if necessary.
    if (promise->future().hasDiscard()) {
      commandDiscarded(s.get(), cmd);
      return;
    }

    callback->first = [promise, s, cmd]() {
      promise->discard();
      CHECK_SOME(s);
      commandDiscarded(s.get(), cmd);
    };
  }

  // Start reading from stdout so writing to the pipe won't block
  // to handle cases where the output is larger than the pipe
  // capacity.
  const Future<string> output = io::read(s->out().get());

  s->status()
    .onAny([=]() {
      __inspect(argv, promise, retryInterval, output, s.get(), callback);
    });
}


void Docker::__inspect(
    const vector<string>& argv,
    const Owned<Promise<Docker::Container>>& promise,
    const Option<Duration>& retryInterval,
    Future<string> output,
    const Subprocess& s,
    shared_ptr<pair<lambda::function<void()>, mutex>> callback)
{
  if (promise->future().hasDiscard()) {
    return;
  }

  // Check the exit status of 'docker inspect'.
  CHECK_READY(s.status());

  Option<int> status = s.status().get();

  const string cmd = strings::join(" ", argv);

  if (!status.isSome()) {
    promise->fail("No status found from '" + cmd + "'");
  } else if (status.get() != 0) {
    output.discard();

    if (retryInterval.isSome()) {
      VLOG(1) << "Retrying inspect with non-zero status code. cmd: '"
              << cmd << "', interval: " << stringify(retryInterval.get());
      Clock::timer(retryInterval.get(),
                   [=]() { _inspect(argv, promise, retryInterval, callback); });
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
      ___inspect(argv, promise, retryInterval, output, callback);
    });
}


void Docker::___inspect(
    const vector<string>& argv,
    const Owned<Promise<Docker::Container>>& promise,
    const Option<Duration>& retryInterval,
    const Future<string>& output,
    shared_ptr<pair<lambda::function<void()>, mutex>> callback)
{
  if (promise->future().hasDiscard()) {
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

  const string cmd = strings::join(" ", argv);

  if (retryInterval.isSome() && !container->started) {
    VLOG(1) << "Retrying inspect since container not yet started. cmd: '"
            << cmd << "', interval: " << stringify(retryInterval.get());
    Clock::timer(retryInterval.get(),
                 [=]() { _inspect(argv, promise, retryInterval, callback); });
    return;
  }

  promise->set(container.get());
}


Future<vector<Docker::Container>> Docker::ps(
    bool all,
    const Option<string>& prefix) const
{
  vector<string> argv;
  argv.push_back(path);
  argv.push_back("-H");
  argv.push_back(socket);
  argv.push_back("ps");

  if (all) {
    argv.push_back("-a");
  }

  const string cmd = strings::join(" ", argv);

  VLOG(1) << "Running " << cmd;

  Try<Subprocess> s = subprocess(
      cmd,
      Subprocess::PATH(os::DEV_NULL),
      Subprocess::PIPE(),
      Subprocess::PIPE(),
      None(),
      None(),
      createParentHooks());

  if (s.isError()) {
    return Failure("Failed to create subprocess '" + cmd + "': " + s.error());
  }

  // Start reading from stdout so writing to the pipe won't block
  // to handle cases where the output is larger than the pipe
  // capacity.
  const Future<string>& output = io::read(s->out().get());

  return s->status()
    .then(lambda::bind(&Docker::_ps, *this, cmd, s.get(), prefix, output));
}


Future<vector<Docker::Container>> Docker::_ps(
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
                failure<vector<Docker::Container>>,
                cmd,
                status.get(),
                lambda::_1));
  }

  // Read to EOF.
  return output.then(lambda::bind(&Docker::__ps, docker, prefix, lambda::_1));
}


Future<vector<Docker::Container>> Docker::__ps(
    const Docker& docker,
    const Option<string>& prefix,
    const string& output)
{
  Owned<vector<string>> lines(new vector<string>());
  *lines = strings::tokenize(output, "\n");

  // Skip the header.
  CHECK(!lines->empty());
  lines->erase(lines->begin());

  Owned<vector<Docker::Container>> containers(new vector<Docker::Container>());

  Owned<Promise<vector<Docker::Container>>> promise(
    new Promise<vector<Docker::Container>>());

  // Limit number of parallel calls to docker inspect at once to prevent
  // reaching system's open file descriptor limit.
  inspectBatches(containers, lines, promise, docker, prefix);

  return promise->future();
}


// TODO(chenlily): Generalize functionality into a concurrency limiter
// within libprocess.
void Docker::inspectBatches(
    Owned<vector<Docker::Container>> containers,
    Owned<vector<string>> lines,
    Owned<Promise<vector<Docker::Container>>> promise,
    const Docker& docker,
    const Option<string>& prefix)
{
  vector<Future<Docker::Container>> batch =
    createInspectBatch(lines, docker, prefix);

  collect(batch).onAny([=](const Future<vector<Docker::Container>>& c) {
    if (c.isReady()) {
      foreach (const Docker::Container& container, c.get()) {
        containers->push_back(container);
      }
      if (lines->empty()) {
        promise->set(*containers);
      }
      else {
        inspectBatches(containers, lines, promise, docker, prefix);
      }
    } else {
      if (c.isFailed()) {
        promise->fail("Docker ps batch failed " + c.failure());
      }
      else {
        promise->fail("Docker ps batch discarded");
      }
    }
  });
}


vector<Future<Docker::Container>> Docker::createInspectBatch(
    Owned<vector<string>> lines,
    const Docker& docker,
    const Option<string>& prefix)
{
  vector<Future<Docker::Container>> batch;

  while (!lines->empty() && batch.size() < DOCKER_PS_MAX_INSPECT_CALLS) {
    string line = lines->back();
    lines->pop_back();

    // Inspect the containers that we are interested in depending on
    // whether or not a 'prefix' was specified.
    vector<string> columns = strings::split(strings::trim(line), " ");

    // We expect the name column to be the last column from ps.
    string name = columns[columns.size() - 1];
    if (prefix.isNone() || strings::startsWith(name, prefix.get())) {
      batch.push_back(docker.inspect(name));
    }
  }

  return batch;
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
    return Docker::__pull(*this, directory, image, path, socket, config);
  }

  argv.push_back(path);
  argv.push_back("-H");
  argv.push_back(socket);
  argv.push_back("inspect");
  argv.push_back(dockerImage);

  string cmd = strings::join(" ", argv);

  VLOG(1) << "Running " << cmd;

  Try<Subprocess> s = subprocess(
      path,
      argv,
      Subprocess::PATH(os::DEV_NULL),
      Subprocess::PIPE(),
      Subprocess::PIPE(),
      nullptr,
      None(),
      None(),
      createParentHooks());

  if (s.isError()) {
    return Failure("Failed to create subprocess '" + cmd + "': " + s.error());
  }

  // Start reading from stdout so writing to the pipe won't block
  // to handle cases where the output is larger than the pipe
  // capacity.
  const Future<string> output = io::read(s->out().get());

  // We assume docker inspect to exit quickly and do not need to be
  // discarded.
  return s->status()
    .then(lambda::bind(
        &Docker::_pull,
        *this,
        s.get(),
        directory,
        dockerImage,
        path,
        socket,
        config,
        output))
    .onDiscard(lambda::bind(&commandDiscarded, s.get(), cmd));
}


Future<Docker::Image> Docker::_pull(
    const Docker& docker,
    const Subprocess& s,
    const string& directory,
    const string& image,
    const string& path,
    const string& socket,
    const Option<JSON::Object>& config,
    Future<string> output)
{
  Option<int> status = s.status().get();
  if (status.isSome() && status.get() == 0) {
    return output
      .then(lambda::bind(&Docker::____pull, lambda::_1));
  }

  output.discard();

  return Docker::__pull(docker, directory, image, path, socket, config);
}


Future<Docker::Image> Docker::__pull(
    const Docker& docker,
    const string& directory,
    const string& image,
    const string& path,
    const string& socket,
    const Option<JSON::Object>& config)
{
  vector<string> argv;
  argv.push_back(path);
  argv.push_back("-H");
  argv.push_back(socket);
  argv.push_back("pull");
  argv.push_back(image);

  string cmd = strings::join(" ", argv);

  VLOG(1) << "Running " << cmd;

  // Set the HOME path where docker config file locates.
  Option<string> home;
  if (config.isSome()) {
    Try<string> _home = os::mkdtemp();

    if (_home.isError()) {
      return Failure("Failed to create temporary directory for docker config"
                     "file: " + _home.error());
    }

    home = _home.get();

    Result<JSON::Object> auths = config->find<JSON::Object>("auths");
    if (auths.isError()) {
      return Failure("Failed to find 'auths' in docker config file: " +
                     auths.error());
    }

    const string path = auths.isSome()
      ? path::join(home.get(), ".docker")
      : home.get();

    Try<Nothing> mkdir = os::mkdir(path);
    if (mkdir.isError()) {
      return Failure("Failed to create path '" + path + "': " + mkdir.error());
    }

    const string file = path::join(path, auths.isSome()
        ? "config.json"
        : ".dockercfg");

    Try<Nothing> write = os::write(file, stringify(config.get()));
    if (write.isError()) {
      return Failure("Failed to write docker config file to '" +
                     file + "': " + write.error());
    }
  }

  // Currently the Docker CLI picks up .docker/config.json (old
  // .dockercfg by looking for the config file in the $HOME
  // directory. The docker config file can either be specified by
  // the agent flag '--docker_config', or by one of the URIs
  // provided which is a docker config file we want docker to be
  // able to pick it up from the sandbox directory where we store
  // all the URI downloads.
  //
  // NOTE: On Windows, Docker users $USERPROFILE instead of $HOME.
  // See MESOS-8619 for more details.
  //
  // TODO(gilbert): Deprecate the fetching docker config file
  // specified as URI method on 0.30.0 release.
#ifdef __WINDOWS__
  const std::string HOME = "USERPROFILE";
#else
  const std::string HOME = "HOME";
#endif // __WINDOWS__
  map<string, string> environment = os::environment();
  environment[HOME] = directory;

  bool configExisted =
    os::exists(path::join(directory, ".docker", "config.json")) ||
    os::exists(path::join(directory, ".dockercfg"));

  // We always set the sandbox as the 'HOME' directory, unless
  // there is no docker config file downloaded in the sandbox
  // and another docker config file is specified using the
  // '--docker_config' agent flag.
  if (!configExisted && home.isSome()) {
    environment[HOME] = home.get();
  }

  Try<Subprocess> s_ = subprocess(
      path,
      argv,
      Subprocess::PATH(os::DEV_NULL),
      Subprocess::PATH(os::DEV_NULL),
      Subprocess::PIPE(),
      nullptr,
      environment,
      None(),
      createParentHooks());

  if (s_.isError()) {
    return Failure("Failed to execute '" + cmd + "': " + s_.error());
  }

  // Docker pull can run for a long time due to large images, so
  // we allow the future to be discarded and it will kill the pull
  // process.
  return s_->status()
    .then(lambda::bind(
        &Docker::___pull,
        docker,
        s_.get(),
        cmd,
        directory,
        image))
    .onDiscard(lambda::bind(&commandDiscarded, s_.get(), cmd))
    .onAny([home]() {
      if (home.isSome()) {
        Try<Nothing> rmdir = os::rmdir(home.get());

        if (rmdir.isError()) {
          LOG(WARNING) << "Failed to remove docker config file temporary"
                       << "'HOME' directory '" << home.get() << "': "
                       << rmdir.error();
        }
      }
    });
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
    return Failure("No status found from '" + cmd + "'");
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
