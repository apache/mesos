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

# SOURCE FILES FOR THE PROCESS LIBRARY.
#######################################

if (WIN32)
  set(PROCESS_SRC
    ${PROCESS_SRC}
    subprocess_windows.cpp
    )
else (WIN32)
  set(PROCESS_SRC
    ${PROCESS_SRC}
    subprocess_posix.cpp
    )
endif (WIN32)

set(PROCESS_SRC
  ${PROCESS_SRC}
  authenticator_manager.cpp
  authenticator_manager.hpp
  authenticator.cpp
  clock.cpp
  config.hpp
  decoder.hpp
  encoder.hpp
  event_loop.hpp
  firewall.cpp
  gate.hpp
  help.cpp
  http.cpp
  io.cpp
  latch.cpp
  logging.cpp
  metrics/metrics.cpp
  pid.cpp
  poll_socket.cpp
  poll_socket.hpp
  profiler.cpp
  process.cpp
  process_reference.hpp
  reap.cpp
  socket.cpp
  subprocess.cpp
  time.cpp
  timeseries.cpp
  )

if (ENABLE_LIBEVENT)
  set(PROCESS_SRC
    ${PROCESS_SRC}
    libevent.hpp
    libevent.cpp
    libevent_poll.cpp)
else (ENABLE_LIBEVENT)
  set(PROCESS_SRC
    ${PROCESS_SRC}
    libev.hpp
    libev.cpp
    libev_poll.cpp
    )
endif (ENABLE_LIBEVENT)

# INCLUDE DIRECTIVES FOR PROCESS LIBRARY (generates, e.g., -I/path/to/thing
# on Linux).
###########################################################################
include_directories(${PROCESS_INCLUDE_DIRS})

# LINKING LIBRARIES BY DIRECTORY (might generate, e.g., -L/path/to/thing on
# Linux).
###########################################################################
link_directories(${PROCESS_LIB_DIRS})

# THE PROCESS LIBRARY (generates, e.g., libprocess.so, etc., on Linux).
#######################################################################
add_library(${PROCESS_TARGET} STATIC ${PROCESS_SRC})
set_target_properties(
  ${PROCESS_TARGET} PROPERTIES
  VERSION ${PROCESS_PACKAGE_VERSION}
  SOVERSION ${PROCESS_PACKAGE_SOVERSION}
  COMPILE_FLAGS "-fPIC"
  )
add_dependencies(${PROCESS_TARGET} ${PROCESS_DEPENDENCIES})

# ADD LINKER FLAGS (generates, e.g., -lglog on Linux).
######################################################
target_link_libraries(${PROCESS_TARGET} ${PROCESS_LIBS})
