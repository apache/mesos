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

# THE MESOS TCP CONNECT SOURCE.
###############################
set(MESOS_TCP_CONNECT_SRC
  ${MESOS_TCP_CONNECT_SRC}
  tcp_connect.cpp
  )

# INCLUDE DIRECTIVES FOR MESOS TCP CONNECT EXECUTABLE (generates, e.g.,
# -I/path/to/thing on Linux).
#######################################################################
include_directories(${AGENT_INCLUDE_DIRS})

# LINKING LIBRARIES BY DIRECTORY (might generate, e.g., -L/path/to/thing on
# Linux).
###########################################################################
link_directories(${AGENT_LIB_DIRS})

# THE MESOS TCP CONNECT EXECUTABLE.
###################################
add_executable(${MESOS_TCP_CONNECT} ${MESOS_TCP_CONNECT_SRC})

# ADD LINKER FLAGS (generates, e.g., -lglog on Linux).
######################################################
target_link_libraries(${MESOS_TCP_CONNECT} ${AGENT_LIBS} ${MESOS_LIBS_TARGET})

# ADD BINARY DEPENDENCIES (tells CMake what to compile/build first).
####################################################################
add_dependencies(${MESOS_TARGET} ${MESOS_TCP_CONNECT})
add_dependencies(${MESOS_TCP_CONNECT} ${MESOS_LIBS_TARGET})
