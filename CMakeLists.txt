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

# THE MESOS PROJECT.
####################
cmake_minimum_required(VERSION 2.8)

project(Mesos)
set(MESOS_PACKAGE_VERSION 0.22.1)
set(MESOS_PACKAGE_SOVERSION 0)

# MESOS BUILD CONFIGURATION OPTIONS.
####################################
option(_DEBUG "Set default build configuration to debug" ON)
option(VERBOSE "Enable verbose CMake statements and compilation output" TRUE)
option(
  REBUNDLED
  "Build dependencies from tar.gz files in 3rdparty folder instead of downloading"
  TRUE
  )
set(CMAKE_VERBOSE_MAKEFILE ${VERBOSE})

# CMAKE MODULE SETUP.
#####################
list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake)
list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/3rdparty/libprocess/cmake)
list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/3rdparty/libprocess/cmake/macros)

# Macros.
include(PatchCommand)
include(External)

# Configuration.
include(MesosConfigure)

# SUBDIRECTORIES.
#################
add_subdirectory(3rdparty/libprocess)
