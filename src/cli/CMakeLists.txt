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

# MESOS EXECUTE.
# Utility used to schedule and run a command in a mesos cluster.
################################################################
add_executable(mesos-execute execute.cpp)
target_link_libraries(mesos-execute PRIVATE mesos)

# MESOS RESOLVE.
# Utility used to determine the current leading master.
#######################################################
add_executable(mesos-resolve resolve.cpp)
target_link_libraries(mesos-resolve PRIVATE mesos)

install(
  TARGETS mesos-execute mesos-resolve
  RUNTIME DESTINATION ${MESOS_INSTALL_RUNTIME})

# MESOS CLI.
# Soon to be deprecated old version of the Mesos CLI.
#####################################################
# TODO(andschwa): Remove this when fully deprecated.
# This is not ported to Windows.
if (NOT ENABLE_NEW_CLI AND NOT WIN32)
  # NOTE: This can be built at the same time as the new CLI, except
  # there is a name collision. To enable them both, change this not to
  # be named `mesos`.
  add_executable(mesos-cli mesos.cpp)
  target_link_libraries(mesos-cli PRIVATE mesos)
  set_target_properties(mesos-cli PROPERTIES OUTPUT mesos)
endif ()
