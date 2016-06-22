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
set(EXECUTOR_EXECUTABLE_SRC
  ${EXECUTOR_EXECUTABLE_SRC}
  executor.cpp
  )

if (NOT WIN32)
  set(EXECUTOR_EXECUTABLE_SRC
    ${EXECUTOR_EXECUTABLE_SRC}
    posix/executor.cpp
    )
else (NOT WIN32)
  set(EXECUTOR_EXECUTABLE_SRC
    ${EXECUTOR_EXECUTABLE_SRC}
    windows/executor.cpp
    )
endif (NOT WIN32)

# THE MESOS LAUNCHER FETCHER SOURCE.
####################################
set(FETCHER_EXECUTABLE_SRC
  fetcher.cpp
  )

# INCLUDE DIRECTIVES FOR LAUNCHER EXECUTABLES (generates, e.g., -I/path/to/thing
# on Linux).
################################################################################
include_directories(${AGENT_INCLUDE_DIRS})

# LINKING LIBRARIES BY DIRECTORY (might generate, e.g., -L/path/to/thing on
# Linux).
###########################################################################
link_directories(${AGENT_LIB_DIRS})

# THE LAUNCHER EXECUTOR AND FETCHER EXECUTABLES.
################################################
add_executable(${MESOS_EXECUTOR} ${EXECUTOR_EXECUTABLE_SRC})
if (NOT WIN32)
  add_executable(${MESOS_FETCHER} ${FETCHER_EXECUTABLE_SRC})
endif (NOT WIN32)

# ADD LINKER FLAGS (generates, e.g., -lglog on Linux).
######################################################
target_link_libraries(${MESOS_EXECUTOR} ${AGENT_LIBS} ${MESOS_TARGET})
if (NOT WIN32)
  target_link_libraries(${MESOS_FETCHER} ${AGENT_LIBS} ${MESOS_TARGET})
endif (NOT WIN32)

# ADD BINARY DEPENDENCIES (tells CMake what to compile/build first).
####################################################################
add_dependencies(${MESOS_EXECUTOR} ${AGENT_DEPENDENCIES})
if (NOT WIN32)
  add_dependencies(${MESOS_FETCHER} ${AGENT_DEPENDENCIES})
endif (NOT WIN32)