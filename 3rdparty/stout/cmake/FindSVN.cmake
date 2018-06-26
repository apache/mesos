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
  COMMAND brew --prefix subversion
  OUTPUT_VARIABLE SVN_PREFIX
  OUTPUT_STRIP_TRAILING_WHITESPACE)

set(POSSIBLE_SVN_INCLUDE_DIRS
  ${SVN_PREFIX}/include/subversion-1
  /usr/include/subversion-1
  /usr/local/include/subversion-1)

set(POSSIBLE_SVN_LIB_DIRS
  ${SVN_PREFIX}/lib
  /usr/lib
  /usr/local/lib)

set(SVN_LIBRARY_NAMES
  svn_delta-1
  svn_diff-1
  svn_subr-1)

FIND_PACKAGE_HELPER(SVN svn_client.h)
