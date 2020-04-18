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

# THE LOAD QOS CONTROLLER LIBRARY.
##################################
# NOTE: This library uses underscores to be consistent with other modules.
add_library(load_qos_controller load.cpp)
target_link_libraries(load_qos_controller PRIVATE mesos)
install(
  TARGETS load_qos_controller
  RUNTIME DESTINATION ${MESOS_INSTALL_RUNTIME}
  LIBRARY DESTINATION ${MESOS_INSTALL_LIBRARIES}
  ARCHIVE DESTINATION ${MESOS_INSTALL_LIBRARIES})
