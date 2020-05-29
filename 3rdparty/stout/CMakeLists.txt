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

find_package(Threads REQUIRED)

add_library(stout INTERFACE)
target_include_directories(stout INTERFACE include)

target_link_libraries(
  stout INTERFACE
  apr
  boost
  elfio
  glog
  libarchive
  libcurl
  picojson
  protobuf
  rapidjson
  Threads::Threads
  zlib
  $<$<PLATFORM_ID:Linux>:rt dl svn>
  $<$<PLATFORM_ID:Darwin>:svn>
  $<$<PLATFORM_ID:Windows>:Ws2_32 IPHlpAPI Mswsock Secur32 Userenv>
  $<$<PLATFORM_ID:FreeBSD>:svn>)

if (ENABLE_INSTALL_MODULE_DEPENDENCIES)
  install(
    DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include
    DESTINATION .)
endif ()

add_subdirectory(tests)
