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

# TODO(josephw): Enable this on Windows after sorting out the dependencies.
if (NOT WIN32)

set(MESOS_LOCAL_TARGET mesos-local
  CACHE STRING "Used to start a local mesos cluster for testing purposes.")

# THE MESOS-LOCAL SOURCE.
#########################
set(MESOS_LOCAL_SRC
  ${MESOS_LOCAL_SRC}
  main.cpp
  )

# THE MESOS LOCAL EXECUTABLE.
#############################
add_executable(${MESOS_LOCAL_TARGET} ${MESOS_LOCAL_SRC})

# ADD LINKER FLAGS (generates, e.g., -lglog on Linux).
######################################################
target_link_libraries(${MESOS_LOCAL_TARGET} ${MESOS_LIBS_TARGET})

# ADD BINARY DEPENDENCIES (tells CMake what to compile/build first).
####################################################################
add_dependencies(${MESOS_TARGET} ${MESOS_LOCAL_TARGET})
add_dependencies(${MESOS_LOCAL_TARGET} ${MESOS_LIBS_TARGET})

endif ()
