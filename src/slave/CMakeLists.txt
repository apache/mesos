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

# THE AGENT SOURCE.
###################
set(AGENT_EXECUTABLE_SRC
  ${AGENT_EXECUTABLE_SRC}
  main.cpp
  )

# TODO(josephw): Port this to Windows.
if (NOT WIN32)
  set(MESOS_LOGROTATE_LOGGER_SRC
    ${MESOS_LOGROTATE_LOGGER_SRC}
    container_loggers/logrotate.cpp
    )
endif (NOT WIN32)

add_subdirectory(qos_controllers)
add_subdirectory(resource_estimators)

# INCLUDE DIRECTIVES FOR AGENT EXECUTABLE (generates, e.g., -I/path/to/thing
# on Linux).
############################################################################
include_directories(${AGENT_INCLUDE_DIRS})

# LINKING LIBRARIES BY DIRECTORY (might generate, e.g., -L/path/to/thing on
# Linux).
###########################################################################
link_directories(${AGENT_LIB_DIRS})

# THE AGENT EXECUTABLE.
#######################
add_executable(${AGENT_TARGET} ${AGENT_EXECUTABLE_SRC})

if (NOT WIN32)
  add_executable(${MESOS_LOGROTATE_LOGGER_TARGET} ${MESOS_LOGROTATE_LOGGER_SRC})
endif (NOT WIN32)

# ADD LINKER FLAGS (generates, e.g., -lglog on Linux).
######################################################
target_link_libraries(${AGENT_TARGET} ${AGENT_LIBS} ${MESOS_TARGET})

if (NOT WIN32)
  target_link_libraries(${MESOS_LOGROTATE_LOGGER_TARGET}
    ${AGENT_LIBS}
    ${MESOS_TARGET}
    )
endif (NOT WIN32)

# ADD BINARY DEPENDENCIES (tells CMake what to compile/build first).
####################################################################
add_dependencies(${AGENT_TARGET} ${MESOS_TARGET})

if (NOT WIN32)
  add_dependencies(${MESOS_LOGROTATE_LOGGER_TARGET} ${MESOS_TARGET})
endif (NOT WIN32)
