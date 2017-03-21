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

#include <list>
#include <map>
#include <string>

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

    // Returns the IPAddress of the container, or None if no IP has
    // been not been assigned.
    const Option<std::string> ipAddress;

    const std::vector<Device> devices;

  private:
    Container(
        const std::string& output,
        const std::string& id,
        const std::string& name,
        const Option<pid_t>& pid,
        bool started,
        const Option<std::string>& ipAddress,
        const std::vector<Device>& devices)
      : output(output),
        id(id),
        name(name),
        pid(pid),
        started(started),
        ipAddress(ipAddress),
        devices(devices) {}
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
        const Option<mesos::Resources>& resources = None(),
        bool enableCfsQuota = false,
        const Option<std::map<std::string, std::string>>& env = None(),
        const Option<std::vector<Device>>& devices = None());

    // "--privileged" option.
    bool privileged;

    // "--cpu-shares" option.
    Option<uint64_t> cpuShares;

    // "--cpu-quota" option.
    Option<uint64_t> cpuQuota;

    // "--memory" option.
    Option<Bytes> memory;

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
  virtual process::Future<std::list<Container>> ps(
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
      const std::string& cmd,
      const process::Owned<process::Promise<Container>>& promise,
      const Option<Duration>& retryInterval);

  static void __inspect(
      const std::string& cmd,
      const process::Owned<process::Promise<Container>>& promise,
      const Option<Duration>& retryInterval,
      process::Future<std::string> output,
      const process::Subprocess& s);

  static void ___inspect(
      const std::string& cmd,
      const process::Owned<process::Promise<Container>>& promise,
      const Option<Duration>& retryInterval,
      const process::Future<std::string>& output);

  static process::Future<std::list<Container>> _ps(
      const Docker& docker,
      const std::string& cmd,
      const process::Subprocess& s,
      const Option<std::string>& prefix,
      process::Future<std::string> output);

  static process::Future<std::list<Container>> __ps(
      const Docker& docker,
      const Option<std::string>& prefix,
      const std::string& output);

  static void inspectBatches(
      process::Owned<std::list<Docker::Container>> containers,
      process::Owned<std::vector<std::string>> lines,
      process::Owned<process::Promise<std::list<Docker::Container>>> promise,
      const Docker& docker,
      const Option<std::string>& prefix);

  static std::list<process::Future<Docker::Container>> createInspectBatch(
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
