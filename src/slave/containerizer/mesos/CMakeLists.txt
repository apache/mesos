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

# THE CONTAINERIZER EXECUTABLE.
###############################
add_executable(mesos-containerizer main.cpp)
target_link_libraries(mesos-containerizer PRIVATE mesos)
install(
  TARGETS mesos-containerizer
  RUNTIME DESTINATION ${MESOS_INSTALL_LAUNCHER})

if (NOT WIN32)
  add_executable(mesos-io-switchboard io/switchboard_main.cpp)
  target_link_libraries(mesos-io-switchboard PRIVATE mesos)

  add_executable(
    mesos-cni-port-mapper
    isolators/network/cni/plugins/port_mapper/main.cpp
    isolators/network/cni/plugins/port_mapper/port_mapper.cpp)

  target_link_libraries(mesos-cni-port-mapper PRIVATE mesos)

  install(
    TARGETS mesos-io-switchboard mesos-cni-port-mapper
    RUNTIME DESTINATION ${MESOS_INSTALL_LAUNCHER})
endif ()

if (ENABLE_PORT_MAPPING_ISOLATOR)
  add_executable(mesos-network-helper isolators/network/helper.cpp)
  target_link_libraries(mesos-network-helper PRIVATE mesos)
  install(
    TARGETS mesos-network-helper
    RUNTIME DESTINATION ${MESOS_INSTALL_LAUNCHER})
endif ()
