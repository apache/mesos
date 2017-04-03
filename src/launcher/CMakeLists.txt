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

# THE MESOS LAUNCHER EXECUTOR SOURCE.
#####################################
set(DEFAULT_EXECUTOR_SRC
  ${DEFAULT_EXECUTOR_SRC}
  default_executor.cpp
  )

set(EXECUTOR_EXECUTABLE_SRC
  ${EXECUTOR_EXECUTABLE_SRC}
  executor.cpp
  )

# THE MESOS LAUNCHER FETCHER SOURCE.
####################################
set(FETCHER_EXECUTABLE_SRC fetcher.cpp)

# THE LAUNCHER EXECUTOR AND FETCHER EXECUTABLES.
################################################
add_executable(${DEFAULT_EXECUTOR_TARGET} ${DEFAULT_EXECUTOR_SRC})
add_executable(${MESOS_EXECUTOR} ${EXECUTOR_EXECUTABLE_SRC})

if (NOT WIN32)
  add_executable(${MESOS_FETCHER} ${FETCHER_EXECUTABLE_SRC})
endif (NOT WIN32)

# ADD LINKER FLAGS (generates, e.g., -lglog on Linux).
######################################################
target_link_libraries(${DEFAULT_EXECUTOR_TARGET} ${MESOS_LIBS_TARGET})
target_link_libraries(${MESOS_EXECUTOR} ${MESOS_LIBS_TARGET})

if (NOT WIN32)
  target_link_libraries(${MESOS_FETCHER} ${MESOS_LIBS_TARGET})
endif (NOT WIN32)

# ADD BINARY DEPENDENCIES (tells CMake what to compile/build first).
####################################################################
add_dependencies(
  ${MESOS_TARGET}
  ${DEFAULT_EXECUTOR_TARGET}
  ${MESOS_EXECUTOR}
  )

add_dependencies(${DEFAULT_EXECUTOR_TARGET} ${MESOS_LIBS_TARGET})
add_dependencies(${MESOS_EXECUTOR}          ${MESOS_LIBS_TARGET})

if (NOT WIN32)
  add_dependencies(${MESOS_TARGET} ${MESOS_FETCHER})
  add_dependencies(${MESOS_FETCHER} ${MESOS_LIBS_TARGET})
endif (NOT WIN32)
