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
set(STOUT ${MESOS_3RDPARTY_SRC}/stout)

EXTERNAL("boost"       ${BOOST_VERSION}       "${MESOS_3RDPARTY_BIN}")
EXTERNAL("picojson"    ${PICOJSON_VERSION}    "${MESOS_3RDPARTY_BIN}")
EXTERNAL("http_parser" ${HTTP_PARSER_VERSION} "${MESOS_3RDPARTY_BIN}")
EXTERNAL("libev"       ${LIBEV_VERSION}       "${MESOS_3RDPARTY_BIN}")
EXTERNAL("libevent"    ${LIBEVENT_VERSION}    "${MESOS_3RDPARTY_BIN}")
EXTERNAL("libapr"      ${LIBAPR_VERSION}      "${MESOS_3RDPARTY_BIN}")
EXTERNAL("protobuf"    ${PROTOBUF_VERSION}    "${MESOS_3RDPARTY_BIN}")

if (NOT WIN32)
  EXTERNAL("glog" ${GLOG_VERSION} "${MESOS_3RDPARTY_BIN}")
elseif (WIN32)
  # Glog 0.3.3 does not compile out of the box on Windows. Therefore, we
  # require 0.3.4.
  EXTERNAL("glog" "0.3.4" "${MESOS_3RDPARTY_BIN}")

  # NOTE: We expect cURL and zlib exist on Unix (usually pulled in with a
  # package manager), but Windows has no package manager, so we have to go
  # get it.
  EXTERNAL("curl" ${CURL_VERSION} "${MESOS_3RDPARTY_BIN}")

  EXTERNAL("zlib" ${ZLIB_VERSION} "${MESOS_3RDPARTY_BIN}")
endif (NOT WIN32)

# Intermediate convenience variables for oddly-structured directories.
set(GLOG_LIB_ROOT     ${GLOG_ROOT}-lib/lib)
set(PROTOBUF_LIB_ROOT ${PROTOBUF_ROOT}-lib/lib)
set(LIBEV_LIB_ROOT    ${LIBEV_ROOT}-lib/lib)
set(LIBEVENT_LIB_ROOT ${LIBEVENT_ROOT}-lib/lib)

# Convenience variables for include directories of third-party dependencies.
set(PROCESS_INCLUDE_DIR     ${MESOS_3RDPARTY_SRC}/libprocess/include)
set(STOUT_INCLUDE_DIR       ${STOUT}/include)

set(BOOST_INCLUDE_DIR       ${BOOST_ROOT})
set(GPERFTOOLS_INCLUDE_DIR  ${GPERFTOOLS}/src)
set(HTTP_PARSER_INCLUDE_DIR ${HTTP_PARSER_ROOT})
set(LIBEV_INCLUDE_DIR       ${LIBEV_ROOT})
set(PICOJSON_INCLUDE_DIR    ${PICOJSON_ROOT})

if (WIN32)
  set(CURL_INCLUDE_DIR     ${CURL_ROOT}/include)
  set(GLOG_INCLUDE_DIR     ${GLOG_ROOT}/src/windows)
  set(PROTOBUF_INCLUDE_DIR ${PROTOBUF_ROOT}/src)
  set(LIBEVENT_INCLUDE_DIR
    ${LIBEVENT_ROOT}/include
    ${LIBEVENT_ROOT}-build/include)
  set(ZLIB_INCLUDE_DIR     ${ZLIB_ROOT} ${ZLIB_ROOT}-build)
else (WIN32)
  set(GLOG_INCLUDE_DIR     ${GLOG_LIB_ROOT}/include)
  set(PROTOBUF_INCLUDE_DIR ${PROTOBUF_LIB_ROOT}/include)
  set(LIBEVENT_INCLUDE_DIR ${LIBEVENT_LIB_ROOT}/include)
endif (WIN32)

# Convenience variables for `lib` directories of built third-party dependencies.
set(LIBEV_LIB_DIR       ${LIBEV_ROOT}-build/.libs)

if (WIN32)
  set(HTTP_PARSER_LIB_DIR ${HTTP_PARSER_ROOT}-build/${CMAKE_BUILD_TYPE})
  set(CURL_LIB_DIR        ${CURL_ROOT}-build/lib/${CMAKE_BUILD_TYPE})
  set(GLOG_LIB_DIR        ${GLOG_ROOT}-build/${CMAKE_BUILD_TYPE})
  set(LIBEVENT_LIB_DIR    ${LIBEVENT_ROOT}-build/lib)
  set(PROTOBUF_LIB_DIR    ${PROTOBUF_ROOT}-build/${CMAKE_BUILD_TYPE})
  set(ZLIB_LIB_DIR        ${ZLIB_ROOT}-build/${CMAKE_BUILD_TYPE})
else (WIN32)
  set(HTTP_PARSER_LIB_DIR ${HTTP_PARSER_ROOT}-build)
  set(GLOG_LIB_DIR        ${GLOG_LIB_ROOT}/lib)
  set(LIBEVENT_LIB_DIR    ${LIBEVENT_LIB_ROOT}/lib)
  set(PROTOBUF_LIB_DIR    ${PROTOBUF_LIB_ROOT}/lib)
endif (WIN32)

# Convenience variables for "lflags", the symbols we pass to CMake to generate
# things like `-L/path/to/glog` or `-lglog`.
set(HTTP_PARSER_LFLAG http_parser)
set(LIBEV_LFLAG       ev)
set(LIBEVENT_LFLAG    event)
set(GLOG_LFLAG        glog)

if (WIN32)
  # Necessary because the lib names for (e.g.) curl are generated incorrectly
  # on Windows. That is, on *nix, the curl binary should be (e.g.) libcurl.so,
  # and on Windows it should be curl.lib. But on Windows, it's actually
  # libcurl.lib. Hence, we have to special case it here because CMake assumes
  # the library names are generated correctly.
  set(CURL_LFLAG     libcurl)
  set(PROTOBUF_LFLAG libprotobufd)

  # Windows requires a static build of zlib.
  set(ZLIB_LFLAG     zlibstaticd)
else (WIN32)
  set(CURL_LFLAG     curl)
  set(DL_LFLAG       dl)
  set(PROTOBUF_LFLAG protobuf)
  set(SASL_LFLAG     sasl2)
endif (WIN32)

# Convenience variable for `protoc`, the Protobuf compiler.
if (NOT WIN32)
  set(PROTOC ${PROTOBUF_LIB_ROOT}/bin/protoc)
else (NOT WIN32)
  set(PROTOC ${PROTOBUF_ROOT}-build/${CMAKE_BUILD_TYPE}/protoc.exe)
endif (NOT WIN32)

# Configure the process library, the last of our third-party libraries.
#######################################################################
include(StoutConfigure)
include(ProcessConfigure)

# Define target for AGENT.
##########################
set(
  AGENT_TARGET mesos-agent
  CACHE STRING "Target we use to refer to agent executable")
