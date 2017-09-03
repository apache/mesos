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

# Builds nodejs/http-parser project.
#########################################
# NOTE: at build time, this file will be copied into the directory of the
# http-parser project. This will allow any CMake-based build system consuming
# the process library to trivially build this dependency and link to it.
cmake_minimum_required(VERSION 2.8)

project(http-parser)

include_directories(.)

set(HTTP_PARSER_SRC
  http_parser.c
  http_parser.h)

add_definitions(-D HTTP_PARSER_STRICT=0)
add_definitions(-fPIC)
add_library(http_parser STATIC ${HTTP_PARSER_SRC})
