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

###############################################################
# Defines the variables useful for tests, and exports them to the scope of
# whatever file includes this file. There are a few important consequence of this:
#
#   * This file MUST be included before the third-party dependencies like gmock
#     are configured/built/downloaded (if you're doing this). If this code isn't
#     run first, then we won't know (e.g.) what folders to unpack the code to.
#   * This file ONLY defines and exports variables for third-party dependencies
#     that are required by the test suite, but are not a dependency that
#     libprocess core takes. That is, this file handles the gmock dependency,
#     but not the glog dependency (which the process library itself takes a
#     dependency on).
#   * This file and the config file for the libprocess "core" dependencies (e.g.,
#     glog, boost, etc.) so that we can export the variables for the core
#     dependencies (e.g., where to find the .so/.dll files) without having to also
#     export the variables for the dependencies that only the test package has.

# DIRECTORY STRUCTURE FOR THIRD-PARTY LIBS REQUIRED FOR TEST INFRASTRUCTURE.
###########################################################################
EXTERNAL("gmock" "1.6.0" "${PROCESS_3RD_BIN}")

set(GTEST_SRC          ${GMOCK_ROOT}/gtest)
set(GPERFTOOLS_VERSION 2.0)
set(GPERFTOOLS         ${PROCESS_3RD_BIN}/gperftools-${GPERFTOOLS_VERSION})

# DEFINE PROCESS TEST LIBRARY DEPENDENCIES. Tells the process library build
# tests target download/configure/build all third-party libraries before
# attempting to build.
###########################################################################
set(PROCESS_TEST_DEPENDENCIES
  ${PROCESS_TEST_DEPENDENCIES}
  ${PROCESS_DEPENDENCIES}
  ${GMOCK_TARGET}
  )
