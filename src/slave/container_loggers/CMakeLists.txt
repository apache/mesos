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

# Logrotate container logger sources.
#####################################
set(LOGROTATE_CONTAINER_LOGGER_SRC lib_logrotate.cpp)
set(MESOS_LOGROTATE_LOGGER_SRC logrotate.cpp)

# Build the container logger module.
####################################
# NOTE: Modules are not supported on Windows.
if (NOT WIN32)
  add_library(${LOGROTATE_CONTAINER_LOGGER_TARGET} SHARED ${LOGROTATE_CONTAINER_LOGGER_SRC})
  add_executable(${MESOS_LOGROTATE_LOGGER_TARGET} ${MESOS_LOGROTATE_LOGGER_SRC})
endif ()

# ADD LINKER FLAGS (generates, e.g., -lglog on Linux).
######################################################
if (NOT WIN32)
  target_link_libraries(${LOGROTATE_CONTAINER_LOGGER_TARGET} ${MESOS_LIBS_TARGET})
  target_link_libraries(${MESOS_LOGROTATE_LOGGER_TARGET} ${MESOS_LIBS_TARGET})
endif ()

# ADD BINARY DEPENDENCIES (tells CMake what to compile/build first).
####################################################################
if (NOT WIN32)
  add_dependencies(${MESOS_TARGET} ${LOGROTATE_CONTAINER_LOGGER_TARGET})

  add_dependencies(
    ${LOGROTATE_CONTAINER_LOGGER_TARGET}
    ${MESOS_LIBS_TARGET}
    ${MESOS_LOGROTATE_LOGGER_TARGET}
    )

  add_dependencies(${MESOS_LOGROTATE_LOGGER_TARGET} ${MESOS_LIBS_TARGET})
endif ()
