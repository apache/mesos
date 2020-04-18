# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

add_subdirectory(container_loggers)
add_subdirectory(qos_controllers)
add_subdirectory(resource_estimators)


# THE AGENT EXECUTABLE.
#######################
add_executable(mesos-agent main.cpp)
target_link_libraries(mesos-agent PRIVATE mesos)
install(TARGETS mesos-agent RUNTIME DESTINATION ${MESOS_INSTALL_RUNTIME})

if (ENABLE_JEMALLOC_ALLOCATOR)
  target_link_libraries(mesos-agent PRIVATE jemalloc)
endif ()

# Add binary dependencies
add_dependencies(
  mesos-agent
  mesos-containerizer
  mesos-default-executor
  mesos-docker-executor
  mesos-executor
  mesos-fetcher
  mesos-tcp-connect
  mesos-usage)

if (WIN32)
  # NOTE: The agent depends on `curl` on non-Windows platforms too,
  # but not as part of the build. This is necessary because we use
  # CMake to deploy `curl.exe` on Windows for use in the HTTP health
  # checks. Similarly we depend on `wclayer` to be deployed.
  add_dependencies(
    mesos-agent
    curl
    wclayer)
else ()
  add_dependencies(
    mesos-agent
    mesos-cni-port-mapper
    mesos-io-switchboard)
endif ()
