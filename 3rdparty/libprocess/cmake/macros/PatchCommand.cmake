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

##############################################################
# PATCH_CMD generates a patch command for those dependencies that need it. For
# example, when the rebundled version of GLOG needs to be patched, we would run
# this command.
function(PATCH_CMD patch_filename patch_cmd_varname)
  # CMake does not appear to have a good way for macro functions to return
  # values, so here we assign the patch command to a variable in the
  # PARENT_SCOPE. (The name of the variable to assign to in the parent scope
  # is passed in as a parameter to the macro function.)
  set (${patch_cmd_varname}
    test ! -e ${patch_filename} || patch -p1 < ${patch_filename}
    PARENT_SCOPE)
endfunction()