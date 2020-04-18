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

# Build the container logger module.
####################################
# NOTE: Modules are not supported on Windows.
if (NOT WIN32)
  # NOTE: This library uses underscores because of a hard-coded expectation of
  # `src/tests/module`.
  add_library(logrotate_container_logger SHARED lib_logrotate.cpp)
  target_link_libraries(logrotate_container_logger PRIVATE mesos)

  # NOTE: This does not link to the above library because this is a
  # light-weight companion binary which can be run independently.
  add_executable(mesos-logrotate-logger logrotate.cpp)
  target_link_libraries(mesos-logrotate-logger PRIVATE mesos)

  add_dependencies(
    logrotate_container_logger
    mesos-logrotate-logger)

  install(
    TARGETS mesos-logrotate-logger
    RUNTIME DESTINATION ${MESOS_INSTALL_LAUNCHER})

  install(
    TARGETS logrotate_container_logger
    RUNTIME DESTINATION ${MESOS_INSTALL_RUNTIME}
    LIBRARY DESTINATION ${MESOS_INSTALL_LIBRARIES}
    ARCHIVE DESTINATION ${MESOS_INSTALL_LIBRARIES})
endif ()
