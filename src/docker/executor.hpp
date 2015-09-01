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

#ifndef __DOCKER_EXECUTOR_HPP__
#define __DOCKER_EXECUTOR_HPP__

#include <stdio.h>

#include <process/process.hpp>

#include "logging/flags.hpp"

namespace mesos {
namespace internal {
namespace docker {

struct Flags : public mesos::internal::logging::Flags
{
  Flags() {
    add(&container,
        "container",
        "The name of the docker container to run.\n");

    add(&docker,
        "docker",
        "The path to the docker executable.\n");

    add(&docker_socket,
        "docker_socket",
        "The UNIX socket path to be used by docker CLI for accessing docker\n"
        "daemon.\n");

    add(&sandbox_directory,
        "sandbox_directory",
        "The path to the container sandbox holding stdout and stderr files\n"
        "into which docker container logs will be redirected.");

    add(&mapped_directory,
        "mapped_directory",
        "The sandbox directory path that is mapped in the docker container.\n");

    add(&stop_timeout,
        "stop_timeout",
        "The duration for docker to wait after stopping a running container\n"
        "before it kills that container.");
  }

  Option<std::string> container;
  Option<std::string> docker;
  Option<std::string> docker_socket;
  Option<std::string> sandbox_directory;
  Option<std::string> mapped_directory;
  Option<Duration> stop_timeout;
};

} // namespace docker {
} // namespace internal {
} // namespace mesos {

#endif // __DOCKER_EXECUTOR_HPP__
