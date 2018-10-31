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

# THE PYTHON CLI EXECUTABLE.
#################################

if (ENABLE_NEW_CLI)
  set(CLI_FILES
    bin/main.py
    bin/settings.py
    lib/cli/__init__.py
    lib/cli/config.py
    lib/cli/constants.py
    lib/cli/docopt.py
    lib/cli/exceptions.py
    lib/cli/http.py
    lib/cli/mesos.py
    lib/cli/util.py
    lib/cli/plugins/__init__.py
    lib/cli/plugins/base.py
    lib/cli/plugins/agent/__init__.py
    lib/cli/plugins/agent/main.py
    lib/cli/plugins/config/__init__.py
    lib/cli/plugins/config/main.py
    lib/cli/plugins/task/__init__.py
    lib/cli/plugins/task/main.py)

  # We find the hidden imports in the CLI and save them in a file.
  add_custom_command(
    OUTPUT cli_hidden_imports.txt
    COMMAND find ${CMAKE_CURRENT_SOURCE_DIR}/lib/cli/plugins
      -mindepth 1 -maxdepth 1 -type d | xargs -I{} basename {}
      > cli_hidden_imports.txt
    COMMAND sed -i -e 's/^/--hidden-import cli.plugins./' cli_hidden_imports.txt
    DEPENDS lib/cli/plugins/)

  add_custom_target(cli_hidden_imports DEPENDS cli_hidden_imports.txt)

  # Creating the virtual environment required by the CLI.
  add_custom_command(
    OUTPUT .virtualenv/bin/activate
    COMMAND
      VIRTUALENV_DIRECTORY=${CMAKE_CURRENT_BINARY_DIR}/.virtualenv
      PYTHON=${PYTHON_3}
      ${CMAKE_CURRENT_SOURCE_DIR}/bootstrap
    DEPENDS bootstrap pip-requirements.txt)

  add_custom_target(cli_virtualenv DEPENDS .virtualenv/bin/activate)

  # We create a file to make sure that the commands run in the same environment.
  # The commands run are running the bootstrap script for the CLI, loading the
  # CLI hidden imports, creating a version file, activating the virtual
  # environment, and using pyinstaller to create the binary.
  file(
    WRITE ${CMAKE_CURRENT_BINARY_DIR}/build_cli.sh
    "set -e
    MESOS_CLI_HIDDEN_IMPORTS=`cat cli_hidden_imports.txt`
    echo \"VERSION = \\\"${PACKAGE_VERSION}\\\"\"                           \
      > ${CMAKE_BINARY_DIR}/src/cli/version.py
    source .virtualenv/bin/activate
    ${PYTHON_3} -m PyInstaller -p ${CMAKE_CURRENT_SOURCE_DIR}/lib           \
      $MESOS_CLI_HIDDEN_IMPORTS --specpath ${CMAKE_BINARY_DIR}/src/cli      \
      --workpath ${CMAKE_BINARY_DIR}/src/cli/work                           \
      --distpath ${CMAKE_BINARY_DIR}/src\ --name mesos --noconfirm --onefile\
      ${CMAKE_CURRENT_SOURCE_DIR}/bin/main.py")

  add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/src/mesos
    COMMAND bash build_cli.sh
    DEPENDS cli_hidden_imports cli_virtualenv ${CLI_FILES})

  add_custom_target(cli ALL DEPENDS ${CMAKE_BINARY_DIR}/src/mesos)
endif ()
