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


# MESOS HEALTH SOURCE.
######################
set(HEALTH_EXECUTABLE_SRC
  ${HEALTH_EXECUTABLE_SRC}
  main.cpp
  )

# INCLUDE DIRECTIVES FOR MESOS HEALTH CHECK EXECUTABLE (generates, e.g.,
# -I/path/to/thing on Linux).
########################################################################
include_directories(${AGENT_INCLUDE_DIRS})

# LINKING LIBRARIES BY DIRECTORY (might generate, e.g., -L/path/to/thing on
# Linux).
###########################################################################
link_directories(${AGENT_LIB_DIRS})

# THE MESOS HELATH CHECK EXECUTABLE.
####################################
add_executable(${MESOS_HEALTH_CHECK} ${HEALTH_EXECUTABLE_SRC})

# ADD LINKER FLAGS (generates, e.g., -lglog on Linux).
######################################################
target_link_libraries(${MESOS_HEALTH_CHECK} ${AGENT_LIBS} ${MESOS_TARGET})


# ADD BINARY DEPENDENCIES (tells CMake what to compile/build first).
####################################################################
add_dependencies(${MESOS_HEALTH_CHECK} ${MESOS_TARGET})
