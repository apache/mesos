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

include(FindPackageHelper)

# NOTE: If this fails, stderr is ignored, and the output variable is empty.
# This has no deleterious effect on our path search.
execute_process(
  COMMAND brew --prefix apr
  OUTPUT_VARIABLE APR_PREFIX
  OUTPUT_STRIP_TRAILING_WHITESPACE)

set(POSSIBLE_APR_INCLUDE_DIRS
  ${APR_PREFIX}/libexec/include/apr-1
  /usr/local/include/apr-1
  /usr/local/include/apr-1.0
  /usr/local/apr/include/apr-1
  /usr/include/apr-1
  /usr/include/apr-1.0)

set(POSSIBLE_APR_LIB_DIRS
  ${APR_PREFIX}/libexec/lib
  /usr/local/apr/lib
  /usr/local/lib
  /usr/lib)

set(APR_LIBRARY_NAMES apr-1)

FIND_PACKAGE_HELPER(APR apr.h)
