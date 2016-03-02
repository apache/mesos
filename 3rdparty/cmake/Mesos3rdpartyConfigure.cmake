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

# DEFINE DIRECTORY STRUCTURE FOR THIRD-PARTY LIBS.
##################################################
set(MESOS_3RDPARTY_SRC ${CMAKE_SOURCE_DIR}/3rdparty)
set(MESOS_3RDPARTY_BIN ${CMAKE_BINARY_DIR}/3rdparty)

if (NOT WIN32)
  EXTERNAL("leveldb"   ${LEVELDB_VERSION}   "${MESOS_3RDPARTY_BIN}")
  EXTERNAL("zookeeper" ${ZOOKEEPER_VERSION} "${MESOS_3RDPARTY_BIN}")
elseif (WIN32)
  # The latest release of ZK, 3.4.7, does not compile on Windows. Therefore, we
  # pick a recent commit that does until the next release stabilizes.
  EXTERNAL("zookeeper" "06d3f3f" "${MESOS_3RDPARTY_BIN}")
endif (NOT WIN32)

# Intermediate convenience variables for oddly-structured directories.
set(ZOOKEEPER_C_ROOT ${ZOOKEEPER_ROOT}/src/c)
set(ZOOKEEPER_LIB    ${ZOOKEEPER_ROOT}/src/c)

# Convenience variables for include directories of third-party dependencies.
set(LEVELDB_INCLUDE_DIR ${LEVELDB_ROOT}/include)
set(ZOOKEEPER_INCLUDE_GENDIR ${ZOOKEEPER_C_ROOT}/generated)
set(ZOOKEEPER_INCLUDE_DIR ${ZOOKEEPER_C_ROOT}/include)

# Convenience variables for `lib` directories of built third-party dependencies.
if (NOT WIN32)
  set(ZOOKEEPER_LIB_DIR ${ZOOKEEPER_LIB})
else (NOT WIN32)
  set(ZOOKEEPER_LIB_DIR ${ZOOKEEPER_LIB}/x64/${CMAKE_BUILD_TYPE})
endif (NOT WIN32)

# Convenience variables for "lflags", the symbols we pass to CMake to generate
# things like `-L/path/to/glog` or `-lglog`.
if (NOT WIN32)
  set(LEVELDB_LFLAG   ${LEVELDB_ROOT}/libleveldb.a)
  set(ZOOKEEPER_LFLAG ${ZOOKEEPER_LIB}/lib/libzookeeper_mt.a)
else (NOT WIN32)
  set(ZOOKEEPER_LFLAG zookeeper)
endif (NOT WIN32)

# Configure Windows use of the GNU patch utility;
# we attempt to find it in its default location,
# but this path may be customized.
#################################################
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
      "Mesos for Windows requires GnuWin32 patch.exe "
      "to apply updates. You may get it from "
      "http://gnuwin32.sourceforge.net/packages/patch.htm"
      )
  else (NOT EXISTS ${GNUWIN32_PATCH_EXECUTABLE})
    message(
      STATUS
      "GnuWin32 patch.exe exists at: "
      ${GNUWIN32_PATCH_EXECUTABLE})

    # Since Windows Vista patch.exe has been reqesting elevation to work
    # eventhough it is not required to apply patches. So to avoid a prompt
    # for elevation a manifest will be applied to patch.exe in the current
    # user temp directory.

    # First: Copy patch.exe and patch.exe.manifest to the user temp dir where
    # elevation is not required. 'Set \Users\<user>\AppData\Local\Temp' dir.
    set(USER_TMP_DIR "TMP")

    # Set full path for temp location of patch.exe.
    set(PATCHEXE_LOCATION $ENV{${USER_TMP_DIR}}/patch.exe)

    #set full path for patch.exe.manifest.
    set(PATCHMANIFEST_LOCATION ${MESOS_3RDPARTY_SRC}/patch.exe.manifest)

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
  endif (NOT EXISTS ${GNUWIN32_PATCH_EXECUTABLE})
endif (WIN32)
