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

#ifndef __DOCKER_HPP__
#define __DOCKER_HPP__

#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <process/future.hpp>
#include <process/owned.hpp>
#include <process/subprocess.hpp>

#include <stout/duration.hpp>
#include <stout/json.hpp>
#include <stout/none.hpp>
#include <stout/nothing.hpp>
#include <stout/option.hpp>
#include <stout/path.hpp>
#include <stout/version.hpp>

#include <stout/os/rm.hpp>

#include "mesos/resources.hpp"

#include "messages/flags.hpp"

// OS-specific default prefix to be used for the DOCKER_HOST environment
// variable. Note that on Linux, the default prefix is the only prefix
// available; only Windows supports multiple prefixes.
// TODO(hausdorff): Add support for the Windows `tcp://` prefix as well.
#ifdef __WINDOWS__
constexpr char DEFAULT_DOCKER_HOST_PREFIX[] = "npipe://";
#else
constexpr char DEFAULT_DOCKER_HOST_PREFIX[] = "unix://";
#endif // __WINDOWS__

// Abstraction for working with Docker (modeled on CLI).
//
// TODO(benh): Make futures returned by functions be discardable.
class Docker
{
public:
  // Create Docker abstraction and optionally validate docker.
  static Try<process::Owned<Docker>> create(
      const std::string& path,
      const std::string& socket,
      bool validate = true,
      const Option<JSON::Object>& config = None());

  virtual ~Docker() {}

  struct Device
  {
    Path hostPath;
    Path containerPath;

    struct Access
    {
      Access() : read(false), write(false), mknod(false) {}

      bool read;
      bool write;
      bool mknod;
    } access;
  };

  struct PortMapping
  {
    uint32_t hostPort;
    uint32_t containerPort;
    Option<std::string> protocol;
  };

  class Container
  {
  public:
    static Try<Container> create(
        const std::string& output);

    // Returns the docker inspect output.
    const std::string output;

    // Returns the ID of the container.
    const std::string id;

    // Returns the name of the container.
    const std::string name;

    // Returns the pid of the container, or None if the container is
    // not running.
    const Option<pid_t> pid;

    // Returns if the container has already started. This field is
    // needed since pid is empty when the container terminates.
    const bool started;

    // Returns the IPv4 address of the container, or `None()` if no
    // IPv4 address has been assigned.
    const Option<std::string> ipAddress;

    // Returns the IPv6 address of the container, or `None()` if no
    // IPv6 address has been assigned.
    const Option<std::string> ip6Address;

    const std::vector<Device> devices;

    // Returns the DNS nameservers set by "--dns" option.
    const std::vector<std::string> dns;

    // Returns the DNS options set by "--dns-option" option.
    const std::vector<std::string> dnsOptions;

    // Returns the DNS search domains set by "--dns-search" option.
    const std::vector<std::string> dnsSearch;

  private:
    Container(
        const std::string& _output,
        const std::string& _id,
        const std::string& _name,
        const Option<pid_t>& _pid,
        bool _started,
        const Option<std::string>& _ipAddress,
        const Option<std::string>& _ip6Address,
        const std::vector<Device>& _devices,
        const std::vector<std::string>& _dns,
        const std::vector<std::string>& _dnsOptions,
        const std::vector<std::string>& _dnsSearch)
      : output(_output),
        id(_id),
        name(_name),
        pid(_pid),
        started(_started),
        ipAddress(_ipAddress),
        ip6Address(_ip6Address),
        devices(_devices),
        dns(_dns),
        dnsOptions(_dnsOptions),
        dnsSearch(_dnsSearch) {}
  };

  class Image
  {
  public:
    static Try<Image> create(const JSON::Object& json);

    Option<std::vector<std::string>> entrypoint;

    Option<std::map<std::string, std::string>> environment;

  private:
    Image(const Option<std::vector<std::string>>& _entrypoint,
          const Option<std::map<std::string, std::string>>& _environment)
      : entrypoint(_entrypoint),
        environment(_environment) {}
  };

  // See https://docs.docker.com/engine/reference/run for a complete
  // explanation of each option.
  class RunOptions
  {
  public:
    static Try<RunOptions> create(
        const mesos::ContainerInfo& containerInfo,
        const mesos::CommandInfo& commandInfo,
        const std::string& containerName,
        const std::string& sandboxDirectory,
        const std::string& mappedDirectory,
        const Option<mesos::Resources>& resourceRequests = None(),
        bool enableCfsQuota = false,
        const Option<std::map<std::string, std::string>>& env = None(),
        const Option<std::vector<Device>>& devices = None(),
        const Option<mesos::internal::ContainerDNSInfo>&
          defaultContainerDNS = None(),
        const Option<google::protobuf::Map<std::string, mesos::Value::Scalar>>&
          resourceLimits = None());

    // "--privileged" option.
    bool privileged;

    // "--cpu-shares" option.
    Option<uint64_t> cpuShares;

    // "--cpu-quota" option.
    Option<uint64_t> cpuQuota;

    // "--memory-reservation" options.
    Option<Bytes> memoryReservation;

    // "--memory" option.
    Option<Bytes> memory;

    // "--oom-score-adj" option.
    Option<int> oomScoreAdj;

    // Environment variable overrides. These overrides will be passed
    // to docker container through "--env-file" option.
    std::map<std::string, std::string> env;

    // "--volume" option.
    std::vector<std::string> volumes;

    // "--volume-driver" option.
    Option<std::string> volumeDriver;

    // "--network" option.
    Option<std::string> network;

    // "--hostname" option.
    Option<std::string> hostname;

    // "--dns" option.
    std::vector<std::string> dns;

    // "--dns-search" option.
    std::vector<std::string> dnsSearch;

    // "--dns-opt" option.
    std::vector<std::string> dnsOpt;

    // Port mappings for "-p" option.
    std::vector<PortMapping> portMappings;

    // "--device" option.
    std::vector<Device> devices;

    // "--entrypoint" option.
    Option<std::string> entrypoint;

    // "--name" option.
    Option<std::string> name;

    // Additional docker options passed through containerizer.
    std::vector<std::string> additionalOptions;

    // "IMAGE[:TAG|@DIGEST]" part of docker run.
    std::string image;

    // Arguments for docker run.
    std::vector<std::string> arguments;
  };

  // Performs 'docker run IMAGE'. Returns the exit status of the
  // container. Note that currently the exit status may correspond
  // to the exit code from a failure of the docker client or daemon
  // rather than the container. Docker >= 1.10 [1] uses the following
  // exit statuses inherited from 'chroot':
  //     125 if the error is with Docker daemon itself.
  //     126 if the contained command cannot be invoked.
  //     127 if the contained command cannot be found.
  //     Exit code of contained command otherwise.
  //
  // [1]: https://github.com/docker/docker/pull/14012
  virtual process::Future<Option<int>> run(
      const RunOptions& options,
      const process::Subprocess::IO& _stdout =
        process::Subprocess::FD(STDOUT_FILENO),
      const process::Subprocess::IO& _stderr =
        process::Subprocess::FD(STDERR_FILENO)) const;

  // Returns the current docker version.
  virtual process::Future<Version> version() const;

  // Performs 'docker stop -t TIMEOUT CONTAINER'. If remove is true then a rm -f
  // will be called when stop failed, otherwise a failure is returned. The
  // timeout parameter will be passed through to docker and is the amount of
  // time for docker to wait after stopping a container before killing it.
  // A value of zero (the default value) is the same as issuing a
  // 'docker kill CONTAINER'.
  virtual process::Future<Nothing> stop(
      const std::string& containerName,
      const Duration& timeout = Seconds(0),
      bool remove = false) const;

  // Performs 'docker kill --signal=<signal> CONTAINER'.
  virtual process::Future<Nothing> kill(
      const std::string& containerName,
      int signal) const;

  // Performs 'docker rm (-f) CONTAINER'.
  virtual process::Future<Nothing> rm(
      const std::string& containerName,
      bool force = false) const;

  // Performs 'docker inspect CONTAINER'. If retryInterval is set,
  // we will keep retrying inspect until the container is started or
  // the future is discarded.
  virtual process::Future<Container> inspect(
      const std::string& containerName,
      const Option<Duration>& retryInterval = None()) const;

  // Performs 'docker ps (-a)'.
  virtual process::Future<std::vector<Container>> ps(
      bool all = false,
      const Option<std::string>& prefix = None()) const;

  virtual process::Future<Image> pull(
      const std::string& directory,
      const std::string& image,
      bool force = false) const;

  // Validate current docker version is not less than minVersion.
  virtual Try<Nothing> validateVersion(const Version& minVersion) const;

  virtual std::string getPath()
  {
    return path;
  }

  virtual std::string getSocket()
  {
    return socket;
  }

protected:
  // Uses the specified path to the Docker CLI tool.
  Docker(const std::string& _path,
         const std::string& _socket,
         const Option<JSON::Object>& _config)
       : path(_path),
         socket(DEFAULT_DOCKER_HOST_PREFIX + _socket),
         config(_config) {}

private:
  static process::Future<Version> _version(
      const std::string& cmd,
      const process::Subprocess& s);

  static process::Future<Version> __version(
      const process::Future<std::string>& output);

  static process::Future<Nothing> _stop(
      const Docker& docker,
      const std::string& containerName,
      const std::string& cmd,
      const process::Subprocess& s,
      bool remove);

  static void _inspect(
      const std::vector<std::string>& argv,
      const process::Owned<process::Promise<Container>>& promise,
      const Option<Duration>& retryInterval,
      std::shared_ptr<std::pair<lambda::function<void()>, std::mutex>>
        callback);

  static void __inspect(
      const std::vector<std::string>& argv,
      const process::Owned<process::Promise<Container>>& promise,
      const Option<Duration>& retryInterval,
      process::Future<std::string> output,
      const process::Subprocess& s,
      std::shared_ptr<std::pair<lambda::function<void()>, std::mutex>>
        callback);

  static void ___inspect(
      const std::vector<std::string>& argv,
      const process::Owned<process::Promise<Container>>& promise,
      const Option<Duration>& retryInterval,
      const process::Future<std::string>& output,
      std::shared_ptr<std::pair<lambda::function<void()>, std::mutex>>
        callback);

  static process::Future<std::vector<Container>> _ps(
      const Docker& docker,
      const std::string& cmd,
      const process::Subprocess& s,
      const Option<std::string>& prefix,
      process::Future<std::string> output);

  static process::Future<std::vector<Container>> __ps(
      const Docker& docker,
      const Option<std::string>& prefix,
      const std::string& output);

  static void inspectBatches(
      process::Owned<std::vector<Docker::Container>> containers,
      process::Owned<std::vector<std::string>> lines,
      process::Owned<process::Promise<std::vector<Docker::Container>>> promise,
      const Docker& docker,
      const Option<std::string>& prefix);

  static std::vector<process::Future<Docker::Container>> createInspectBatch(
      process::Owned<std::vector<std::string>> lines,
      const Docker& docker,
      const Option<std::string>& prefix);

  static process::Future<Image> _pull(
      const Docker& docker,
      const process::Subprocess& s,
      const std::string& directory,
      const std::string& image,
      const std::string& path,
      const std::string& socket,
      const Option<JSON::Object>& config,
      process::Future<std::string> output);

  static process::Future<Image> __pull(
      const Docker& docker,
      const std::string& directory,
      const std::string& image,
      const std::string& path,
      const std::string& socket,
      const Option<JSON::Object>& config);

  static process::Future<Image> ___pull(
      const Docker& docker,
      const process::Subprocess& s,
      const std::string& cmd,
      const std::string& directory,
      const std::string& image);

  static process::Future<Image> ____pull(
      const std::string& output);

  static void pullDiscarded(
      const process::Subprocess& s,
      const std::string& cmd);

  const std::string path;
  const std::string socket;
  const Option<JSON::Object> config;
};

#endif // __DOCKER_HPP__
