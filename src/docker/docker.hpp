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

#ifndef __DOCKER_HPP__
#define __DOCKER_HPP__

#include <list>
#include <string>

#include <process/future.hpp>
#include <process/subprocess.hpp>

#include <stout/json.hpp>
#include <stout/none.hpp>
#include <stout/nothing.hpp>
#include <stout/option.hpp>

#include "mesos/resources.hpp"


// Abstraction for working with Docker (modeled on CLI).
//
// TODO(benh): Make futures returned by functions be discardable.
class Docker
{
public:
  // Create Docker abstraction and optionally validate docker.
  static Try<Docker> create(const std::string& path, bool validate = true);

  class Container
  {
  public:
    static Try<Container> create(const JSON::Object& json);

    // Returns the ID of the container.
    std::string id;

    // Returns the name of the container.
    std::string name;

    // Returns the pid of the container, or None if the container is
    // not running.
    Option<pid_t> pid;

  private:
    Container(
        const std::string& _id,
        const std::string& _name,
        const Option<pid_t>& _pid)
      : id(_id), name(_name), pid(_pid) {}
  };

  // Performs 'docker run IMAGE'.
  process::Future<Nothing> run(
      const mesos::ContainerInfo& containerInfo,
      const mesos::CommandInfo& commandInfo,
      const std::string& name,
      const std::string& sandboxDirectory,
      const std::string& mappedDirectory,
      const Option<mesos::Resources>& resources = None(),
      const Option<std::map<std::string, std::string> >& env = None()) const;

  // Performs 'docker kill CONTAINER'. If remove is true then a rm -f
  // will be called when kill failed, otherwise a failure is returned.
  process::Future<Nothing> kill(
      const std::string& container,
      bool remove = false) const;

  // Performs 'docker rm (-f) CONTAINER'.
  process::Future<Nothing> rm(
      const std::string& container,
      bool force = false) const;

  // Performs 'docker inspect CONTAINER'.
  process::Future<Container> inspect(
      const std::string& container) const;

  // Performs 'docker ps (-a)'.
  process::Future<std::list<Container> > ps(
      bool all = false,
      const Option<std::string>& prefix = None()) const;

  // Performs a 'docker logs --follow' and sends the output into a
  // 'stderr' and 'stdout' file in the specfied directory.
  //
  // TODO(benh): Return the file descriptors, or some struct around
  // them so others can do what they want with stdout/stderr.
  process::Future<Nothing> logs(
      const std::string& container,
      const std::string& directory);

private:
  // Uses the specified path to the Docker CLI tool.
  Docker(const std::string& _path) : path(_path) {};

  static process::Future<Nothing> _kill(
      const Docker& docker,
      const std::string& container,
      const std::string& cmd,
      const process::Subprocess& s,
      bool remove);

  static process::Future<Container> _inspect(
      const std::string& cmd,
      const process::Subprocess& s);

  static process::Future<Container> __inspect(
      const std::string& output);

  static process::Future<std::list<Container> > _ps(
      const Docker& docker,
      const std::string& cmd,
      const process::Subprocess& s,
      const Option<std::string>& prefix);

  static process::Future<std::list<Container> > __ps(
      const Docker& docker,
      const Option<std::string>& prefix,
      const std::string& output);

  const std::string path;
};

#endif // __DOCKER_HPP__
