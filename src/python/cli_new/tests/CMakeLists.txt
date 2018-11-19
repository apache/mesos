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

# THE PYTHON CLI TESTS.
#################################
if (ENABLE_NEW_CLI)
  # We create a file so that the commands will run in the same environment.
  file(
    WRITE ${CMAKE_CURRENT_BINARY_DIR}/run_cli_tests.sh
    "set -e
    source ${CMAKE_CURRENT_BINARY_DIR}/../.virtualenv/bin/activate
    source ${CMAKE_CURRENT_BINARY_DIR}/../.virtualenv/bin/postactivate
    PATH=${CMAKE_BINARY_DIR}/src:$PATH mesos-cli-tests")

    add_custom_target(
      cli-tests
      DEPENDS mesos-agent mesos-execute mesos-master cli_virtualenv)

    add_test(NAME CLITests COMMAND bash run_cli_tests.sh)
endif ()
