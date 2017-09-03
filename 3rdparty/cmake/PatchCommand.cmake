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

# Find and fix GNU Patch for Windows.
#####################################
if (WIN32)
  set(PROGRAMFILESX86 "PROGRAMFILES(X86)")
  set(PATCHEXE_DEFAULT_LOCATION $ENV{${PROGRAMFILESX86}}/GnuWin32/bin)

  set(PATCHEXE_PATH
    ${PATCHEXE_DEFAULT_LOCATION}
    CACHE PATH "Path for GnuWin32 patch.exe")

  set(
    GNUWIN32_PATCH_EXECUTABLE
    ${PATCHEXE_PATH}/patch.exe
    CACHE PATH "Full path for GnuWin32 patch.exe")

  if (NOT EXISTS ${GNUWIN32_PATCH_EXECUTABLE})
    message(
      FATAL_ERROR
      "GnuWin32 patch.exe was not found. Use -DPATCHEXE_PATH to "
      "provide the local path of GnuWin32 patch.exe. "
      "Also delete CMakeCache.txt to clear the cache. "
      "Mesos for Windows requires GnuWin32 patch.exe "
      "to apply updates. You may get it from "
      "http://gnuwin32.sourceforge.net/packages/patch.htm")
  else ()
    message(
      STATUS
      "GnuWin32 patch.exe exists at: "
      ${GNUWIN32_PATCH_EXECUTABLE})

    # Since Windows Vista patch.exe has been reqesting elevation to work
    # even though it is not required to apply patches. So to avoid a prompt
    # for elevation a manifest will be applied to patch.exe in the current
    # user temp directory.

    # First: Copy patch.exe and patch.exe.manifest to the user temp dir where
    # elevation is not required. 'Set \Users\<user>\AppData\Local\Temp' dir.
    set(USER_TMP_DIR "TMP")

    # Set full path for temp location of patch.exe.
    set(PATCHEXE_LOCATION $ENV{${USER_TMP_DIR}}/patch.exe)

    # Set full path for patch.exe.manifest.
    set(PATCHMANIFEST_LOCATION ${CMAKE_SOURCE_DIR}/3rdparty/patch.exe.manifest)

    # Set full path for temp location of patch.exe.manifest.
    set(PATCHMANIFEST_TMP_LOCATION $ENV{${USER_TMP_DIR}}/patch.exe.manifest)

    # Copy patch.exe and path.exe.manifest to temp location.
    configure_file(
      ${GNUWIN32_PATCH_EXECUTABLE}
      ${PATCHEXE_LOCATION}
      COPYONLY)
    configure_file(
      ${PATCHMANIFEST_LOCATION}
      ${PATCHMANIFEST_TMP_LOCATION}
      COPYONLY)

    # Second: Apply manifest to patch command.
    set(
      APPLY_PATCH_MANIFEST_COMMAND
      "mt.exe"
      -manifest mt ${PATCHMANIFEST_TMP_LOCATION}
      -outputresource:${PATCHEXE_LOCATION};1)

    add_custom_command(
      OUTPUT   patch.exe
      COMMAND  ${APPLY_PATCH_MANIFEST_COMMAND})
  endif ()
endif ()

# PATCH_CMD generates a patch command given a patch file. If the path is not
# absolute, it's resolved to the current source directory. It stores the command
# in the variable name supplied.
################################################################################
function(PATCH_CMD CMD_VAR PATCH_FILE)
  get_filename_component(PATCH_PATH ${PATCH_FILE} ABSOLUTE)
  if (WIN32)
    # Set the patch command which will utilize patch.exe in temp location for no elevation prompt
    # NOTE: We do not specify the `--binary` patch option here because the
    # files being modified are extracted with CRLF (Windows) line endings
    # already. The `--binary` option will instead fail to apply the patch.
    set (${CMD_VAR}
      ${PATCHEXE_LOCATION} -p1 < ${PATCH_PATH}
      PARENT_SCOPE)
  else ()
    set (${CMD_VAR}
      test ! -e ${PATCH_PATH} || patch -p1 < ${PATCH_PATH}
      PARENT_SCOPE)
  endif ()
endfunction()
