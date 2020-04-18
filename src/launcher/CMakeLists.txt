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

# THE LAUNCHER EXECUTOR AND FETCHER EXECUTABLES.
################################################
add_executable(mesos-default-executor default_executor.cpp)
target_link_libraries(mesos-default-executor PRIVATE mesos)

add_executable(mesos-executor executor.cpp)
target_link_libraries(mesos-executor PRIVATE mesos)

add_executable(mesos-fetcher fetcher.cpp)
target_link_libraries(mesos-fetcher PRIVATE mesos)

# THE DOCKER EXECUTOR EXECUTABLE.
#################################
add_executable(mesos-docker-executor docker_executor.cpp)
target_link_libraries(mesos-docker-executor PRIVATE mesos)

install(
  TARGETS
    mesos-default-executor
    mesos-executor
    mesos-fetcher
    mesos-docker-executor
  RUNTIME DESTINATION ${MESOS_INSTALL_LAUNCHER})
