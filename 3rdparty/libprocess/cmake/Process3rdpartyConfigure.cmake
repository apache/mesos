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

# DEFINE DIRECTORY STRUCTURE FOR THIRD-PARTY LIBS.
##################################################
set(PROCESS_3RD_SRC ${CMAKE_SOURCE_DIR}/3rdparty/libprocess/3rdparty)
set(PROCESS_3RD_BIN ${CMAKE_BINARY_DIR}/3rdparty/libprocess/3rdparty)

set(STOUT ${PROCESS_3RD_SRC}/stout)

EXTERNAL("boost"       ${BOOST_VERSION}       "${PROCESS_3RD_BIN}")
EXTERNAL("picojson"    ${PICOJSON_VERSION}    "${PROCESS_3RD_BIN}")
EXTERNAL("http_parser" ${HTTP_PARSER_VERSION} "${PROCESS_3RD_BIN}")
EXTERNAL("libev"       ${LIBEV_VERSION}       "${PROCESS_3RD_BIN}")
EXTERNAL("libevent"    ${LIBEVENT_VERSION}    "${PROCESS_3RD_BIN}")
EXTERNAL("libapr"      ${LIBAPR_VERSION}      "${PROCESS_3RD_BIN}")
EXTERNAL("protobuf"    ${PROTOBUF_VERSION}    "${PROCESS_3RD_BIN}")

if (NOT WIN32)
  EXTERNAL("glog" ${GLOG_VERSION} "${PROCESS_3RD_BIN}")
elseif (WIN32)
  # Glog 0.3.3 does not compile out of the box on Windows. Therefore, we
  # require 0.3.4.
  EXTERNAL("glog" "0.3.4" "${PROCESS_3RD_BIN}")

  # NOTE: We expect cURL exists on Unix (usually pulled in with a package
  # manager), but Windows has no package manager, so we have to go get it.
  EXTERNAL("curl" ${CURL_VERSION} "${PROCESS_3RD_BIN}")
endif (NOT WIN32)

# Intermediate convenience variables for oddly-structured directories.
set(GLOG_LIB_ROOT     ${GLOG_ROOT}-lib/lib)
set(PROTOBUF_LIB_ROOT ${PROTOBUF_ROOT}-lib/lib)
set(LIBEV_LIB_ROOT    ${LIBEV_ROOT}-lib/lib)
set(LIBEVENT_LIB_ROOT ${LIBEVENT_ROOT}-lib/lib)

# Convenience variables for include directories of third-party dependencies.
set(PROCESS_INCLUDE_DIR     ${PROCESS_3RD_SRC}/../include)
set(STOUT_INCLUDE_DIR       ${STOUT}/include)

set(BOOST_INCLUDE_DIR       ${BOOST_ROOT})
set(GPERFTOOLS_INCLUDE_DIR  ${GPERFTOOLS}/src)
set(HTTP_PARSER_INCLUDE_DIR ${HTTP_PARSER_ROOT})
set(LIBEV_INCLUDE_DIR       ${LIBEV_ROOT})
set(LIBEVENT_INCLUDE_DIR    ${LIBEVENT_LIB_ROOT}/include)
set(PICOJSON_INCLUDE_DIR    ${PICOJSON_ROOT})

if (WIN32)
  set(CURL_INCLUDE_DIR     ${CURL_ROOT}/include)
  set(GLOG_INCLUDE_DIR     ${GLOG_ROOT}/src/windows)
  set(PROTOBUF_INCLUDE_DIR ${PROTOBUF_ROOT}/src)
else (WIN32)
  set(GLOG_INCLUDE_DIR     ${GLOG_LIB_ROOT}/include)
  set(PROTOBUF_INCLUDE_DIR ${PROTOBUF_LIB_ROOT}/include)
endif (WIN32)

# Convenience variables for `lib` directories of built third-party dependencies.
set(HTTP_PARSER_LIB_DIR ${HTTP_PARSER_ROOT}-build)
set(LIBEV_LIB_DIR       ${LIBEV_ROOT}-build/.libs)

if (WIN32)
  set(CURL_LIB_DIR     ${CURL_ROOT}/lib)
  set(GLOG_LIB_DIR     ${GLOG_ROOT}/${CMAKE_BUILD_TYPE})
  set(LIBEVENT_LIB_DIR ${LIBEVENT_ROOT}-build/lib)
  set(PROTOBUF_LIB_DIR ${PROTOBUF_ROOT}/vsprojects/${CMAKE_BUILD_TYPE})
else (WIN32)
  set(GLOG_LIB_DIR     ${GLOG_LIB_ROOT}/lib)
  set(LIBEVENT_LIB_DIR ${LIBEVENT_LIB_ROOT}/lib)
  set(PROTOBUF_LIB_DIR ${PROTOBUF_LIB_ROOT}/lib)
endif (WIN32)

# Convenience variables for "lflags", the symbols we pass to CMake to generate
# things like `-L/path/to/glog` or `-lglog`.
set(HTTP_PARSER_LFLAG http_parser)
set(LIBEV_LFLAG       ev)
set(LIBEVENT_LFLAG    event)

if (WIN32)
  # Necessary because the lib names for (e.g.) glog are generated incorrectly
  # on Windows. That is, on *nix, the glog binary should be (e.g.) libglog.so,
  # and on Windows it should be glog.lib. But on Windows, it's actually
  # libglog.lib. Hence, we have to special case it here because CMake assumes
  # the library names are generated correctly.
  set(CURL_LFLAG     libcurl_a)
  set(GLOG_LFLAG     libglog)
  set(PROTOBUF_LFLAG libprotobuf)
else (WIN32)
  set(GLOG_LFLAG     glog)
  set(PROTOBUF_LFLAG protobuf)
endif (WIN32)

# Convenience variable for `protoc`, the Protobuf compiler.
if (NOT WIN32)
  set(PROTOC ${PROTOBUF_LIB_ROOT}/bin/protoc)
else (NOT WIN32)
  set(PROTOC ${PROTOBUF_ROOT}/vsprojects/${CMAKE_BUILD_TYPE}/protoc.exe)
endif (NOT WIN32)

# Configure the process library, the last of our third-party libraries.
#######################################################################
include(ProcessConfigure)