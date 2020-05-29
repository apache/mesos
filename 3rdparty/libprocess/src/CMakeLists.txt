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

# OpenSSL: Cryptography and SSL/TLS Toolkit.
# https://www.openssl.org
############################################
if (ENABLE_SSL)
  if (ENABLE_LIBEVENT)
    find_package(OpenSSL REQUIRED)
  else ()
    # SSL without Libevent requires a more recent version of OpenSSL
    # because of the APIs used by the implementation.
    # TODO(josephw): Add compatibility for OpenSSL 1.0.
    find_package(OpenSSL 1.1 REQUIRED)
  endif ()
endif ()


# SOURCE FILES FOR THE PROCESS LIBRARY.
#######################################
set(PROCESS_SRC
  authenticator.cpp
  authenticator_manager.cpp
  clock.cpp
  firewall.cpp
  grpc.cpp
  gtest_constants.cpp
  help.cpp
  http.cpp
  http_proxy.cpp
  io.cpp
  latch.cpp
  logging.cpp
  memory_profiler.cpp
  metrics/metrics.cpp
  mime.cpp
  pid.cpp
  process.cpp
  profiler.cpp
  reap.cpp
  socket.cpp
  subprocess.cpp
  time.cpp)


if (WIN32)
  list(APPEND PROCESS_SRC
    windows/subprocess.cpp)
else ()
  list(APPEND PROCESS_SRC
    posix/subprocess.cpp)
endif ()

if (ENABLE_LIBEVENT)
  list(APPEND PROCESS_SRC
    posix/libevent/libevent.cpp
    posix/libevent/libevent_poll.cpp)
elseif (WIN32)
  list(APPEND PROCESS_SRC
    windows/event_loop.cpp
    windows/libwinio.cpp)
else ()
  list(APPEND PROCESS_SRC
    posix/libev/libev.cpp
    posix/libev/libev_poll.cpp)
endif ()

if (WIN32 AND NOT ENABLE_LIBEVENT)
  list(APPEND PROCESS_SRC
    windows/io.cpp
    windows/poll_socket.cpp)
else ()
  list(APPEND PROCESS_SRC
    posix/io.cpp
    posix/poll_socket.cpp)
endif ()

if (ENABLE_SSL)
  list(APPEND PROCESS_SRC
    jwt.cpp
    jwt_authenticator.cpp
    openssl.cpp
    ssl/utilities.cpp)

  if (ENABLE_LIBEVENT)
    list(APPEND PROCESS_SRC
      posix/libevent/libevent_ssl_socket.cpp)
  else ()
    list(APPEND PROCESS_SRC
      ssl/openssl_socket.cpp)
  endif ()
endif ()

add_library(process ${PROCESS_SRC})

target_link_libraries(
  process PUBLIC
  stout
  http_parser
  grpc
  $<$<BOOL:${ENABLE_SSL}>:OpenSSL::SSL>
  $<$<BOOL:${ENABLE_SSL}>:OpenSSL::Crypto>)

target_link_libraries(
  process PRIVATE
  concurrentqueue
  $<IF:$<BOOL:${ENABLE_LIBEVENT}>,libevent,$<$<NOT:$<PLATFORM_ID:Windows>>:libev>>)

target_compile_definitions(
  process PRIVATE
  $<$<AND:$<PLATFORM_ID:Windows>,$<NOT:$<BOOL:${ENABLE_LIBEVENT}>>>:ENABLE_LIBWINIO>
  $<$<BOOL:${ENABLE_LOCK_FREE_RUN_QUEUE}>:LOCK_FREE_RUN_QUEUE>
  $<$<BOOL:${ENABLE_LOCK_FREE_EVENT_QUEUE}>:LOCK_FREE_EVENT_QUEUE>
  $<$<BOOL:${ENABLE_LAST_IN_FIRST_OUT_FIXED_SIZE_SEMAPHORE}>:LAST_IN_FIRST_OUT_FIXED_SIZE_SEMAPHORE>
  $<$<PLATFORM_ID:Linux>:LIBPROCESS_ALLOW_JEMALLOC>)

target_include_directories(process PUBLIC ../include)
target_include_directories(process PRIVATE .)

install(
  TARGETS process
  RUNTIME DESTINATION ${LIBPROCESS_INSTALL_RUNTIME}
  LIBRARY DESTINATION ${LIBPROCESS_INSTALL_LIBRARIES}
  ARCHIVE DESTINATION ${LIBPROCESS_INSTALL_LIBRARIES})
