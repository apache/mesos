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
# GROUP_SOURCE takes a directory root and recursively groups source files by
# folder, so they can be displayed hierarchically in IDEs such as XCode or
# Visual Studio. NOTE: Source files are only displayed if they are added for
# compilation. If a file is not compiled, it will not show up in the IDE.
#
# Parameters:
#   GROUP_NAME     Specifies group name of this directory tree. For example,
#                  the directory tree `src/slave/` would fall under the group
#                  name "Agent Source Files".
#   ROOT_DIRECTORY Root of the directory tree to generate source groups for.
#   RELATIVE_TO    Directory to create source group tree relative to. For
#                  example, if you want to the code in `src/slave` to show up
#                  in the directory tree under a folder called `slave/` (i.e.,
#                  omitting the `src/` component), then you would provite a
#                  ROOT_DIRECTORY `src/slave` RELATIVE_TO `slave/`.
#   SOURCE_PATTERN Pattern (glob) of source files to match (e.g, *.cpp).
function(GROUP_SOURCE GROUP_NAME ROOT_DIRECTORY RELATIVE_TO SOURCE_PATTERN)
  file(
    GLOB_RECURSE
    SOURCE_FILES
    RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}"
    "${ROOT_DIRECTORY}/${SOURCE_PATTERN}"
    )

  # Extremely inefficient method of recursively creating a source group for
  # every subdirectory that contains a source file. In other words, if we have
  # a folder structure like `slave/containerizer/`, we want a source group for
  # both `slave/` and `slave/containerizer/`, so that the IDE structures the
  # source tree hierarchically. This loop is so inefficient because CMake does
  # not support a set data structure, it is hard to do better than this and
  # still be cross-platform.
  #
  # The way this loop functions is to:
  # (1) Loop over every source file found in any subdirectory rooted at
  #     `ROOT_DIRECTORY`
  # (2) For each source file, obtain the directory it exists in. For example if
  #     the source file is `slave/containerizer/containerizer.cpp`, then the
  #     directory `slave/containerizer/`.
  # (3) Create a source group for the directory. The source group for the
  #     directory consists of any file that is in the directory, not including
  #     files that are in subdirectories. In the example above, this would be
  #     something like `slave/containerizer/*.cpp`.
  #
  #     NOTE: This operation is so inefficient because it occurs for every
  #     source file, rather than every unique directory. So, every time we
  #     encounter a source file in any directory, we will add a source group
  #     for its source directory.
  #
  #     NOTE: This operation will also add every source file to the source
  #     group, but only the source files that both appear in this source group,
  #     and which users actually pass to the target for compilation will appear
  #     in the IDE's source group.
  foreach (SOURCE_FILE ${SOURCE_FILES})
    # Get relative directory of source file, use to generate a source group
    # that resembles the directory tree.
    get_filename_component(SOURCE_FILE_REALPATH ${SOURCE_FILE} REALPATH)

    file(RELATIVE_PATH SOURCE_FILE_RELATIVE
      ${RELATIVE_TO} ${SOURCE_FILE_REALPATH})

    get_filename_component(
      SOURCE_DIRECTORY_RELATIVE
      ${SOURCE_FILE_RELATIVE}
      PATH)

    set(SOURCE_GROUP "${GROUP_NAME}\\${SOURCE_DIRECTORY_RELATIVE}")

    # This is necessary because source group names require '\' rather than '/'
    # as separators, in order to make a hierarchical source tree. This is
    # primarily because the source group construct was originally designed for
    # Visual Studio projects.
    string(REGEX REPLACE "/" "\\\\" SOURCE_GROUP ${SOURCE_GROUP})

    # Get all files in `SOURCE_DIRECTORY`, and create a source group for it.
    # NOTE: It is important concatinate `SOURCE_FILE` with the relative-to root
    # we passed to `file` above; if we don't, we will get an empty string when
    # `RELATIVE_TO` is the same as the current source directory, which in turn
    # will cause the call to `file` below to return no source files.
    get_filename_component(
      SOURCE_DIRECTORY
      "${CMAKE_CURRENT_SOURCE_DIR}/${SOURCE_FILE}"
      PATH)

    file(
      GLOB
      SOURCE_GROUP_FILES
      RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}"
      "${SOURCE_DIRECTORY}/${SOURCE_PATTERN}"
      )

    source_group(${SOURCE_GROUP} FILES ${SOURCE_GROUP_FILES})
  endforeach (SOURCE_FILE)
endfunction(GROUP_SOURCE)
