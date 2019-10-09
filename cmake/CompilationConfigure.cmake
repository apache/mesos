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

include(CMakePushCheckState)

# GENERAL OPTIONS.
##################
option(VERBOSE
  "Enable verbose CMake statements and compilation output."
  TRUE)
set(CMAKE_VERBOSE_MAKEFILE ${VERBOSE})

if (NOT WIN32)
  set(DEFAULT_BUILD_SHARED_LIBS TRUE)
else ()
  set(DEFAULT_BUILD_SHARED_LIBS FALSE)
endif ()

option(BUILD_SHARED_LIBS
  "Build shared libraries."
  ${DEFAULT_BUILD_SHARED_LIBS})

option(ENABLE_PRECOMPILED_HEADERS
  "Enable auto-generated precompiled headers using cotire"
  ${WIN32})

if (NOT WIN32 AND ENABLE_PRECOMPILED_HEADERS)
  message(
    FATAL_ERROR
    "Precompiled headers are only supported on Windows.  See MESOS-7322.")
endif ()

if (ENABLE_PRECOMPILED_HEADERS)
  # By default Cotire generates both precompiled headers and a "unity" build.
  # A unity build is where all the source files in a target are combined into
  # a single source file to reduce the number of files that need to be opened
  # and read. We disable "unity" builds for now.
  set(COTIRE_ADD_UNITY_BUILD FALSE)
  set(COTIRE_VERBOSE ${VERBOSE})
endif ()

if (CMAKE_GENERATOR MATCHES "Visual Studio")
  # In MSVC 1900, there are two bugs in the linker, one that causes linking
  # libmesos to occasionally take hours, and one that causes us to be able to
  # fail to open the `mesos-x.lib` file. These have been confirmed as bugs with
  # the MSVC backend team by hausdorff.
  set(PREFERRED_TOOLSET "host=x64")
  if (NOT CMAKE_GENERATOR_TOOLSET MATCHES ${PREFERRED_TOOLSET})
    message(
      FATAL_ERROR
      "The x64 toolset MUST be used. See MESOS-6720 for details. "
      "Please use `cmake -T ${PREFERRED_TOOLSET}`.")
  endif ()
endif ()


# 3RDPARTY OPTIONS.
###################
option(
  REBUNDLED
  "Use dependencies from the 3rdparty folder (instead of internet)."
  TRUE)

option(
  UNBUNDLED_LIBARCHIVE
  "Build with an installed libarchive version instead of the bundled."
  FALSE)

set(
  LIBARCHIVE_ROOT_DIR
  ""
  CACHE STRING
  "Specify the path to libarchive, e.g. \"C:\\libarchive-Win64\".")

option(
  ENABLE_LIBEVENT
  "Use libevent instead of libev as the core event loop implementation."
  FALSE)

if (ENABLE_LIBEVENT)
  option(
    UNBUNDLED_LIBEVENT
    "Build libprocess with an installed libevent version instead of the bundled."
    FALSE)

  set(
    LIBEVENT_ROOT_DIR
    ""
    CACHE STRING
    "Specify the path to libevent, e.g. \"C:\\libevent-Win64\".")
endif()

option(
  UNBUNDLED_LEVELDB
  "Build with an installed leveldb version instead of the bundled."
  FALSE)

set(
  LEVELDB_ROOT_DIR
  ""
  CACHE STRING
  "Specify the path to leveldb, e.g. \"C:\\leveldb-Win64\".")

if (ENABLE_SECCOMP_ISOLATOR)
  option(
    UNBUNDLED_LIBSECCOMP
    "Build with an installed libseccomp version instead of the bundled."
    FALSE)

  set(
    LIBSECCOMP_ROOT_DIR
    ""
    CACHE STRING
    "Specify the path to libseccomp, e.g. \"C:\\libseccomp-Win64\".")
endif ()

option(
  ENABLE_SSL
  "Build libprocess with SSL support."
  FALSE)

option(
  ENABLE_LOCK_FREE_RUN_QUEUE
  "Build libprocess with lock free run queue."
  FALSE)

option(
  ENABLE_LOCK_FREE_EVENT_QUEUE
  "Build libprocess with lock free event queue."
  FALSE)

option(
  ENABLE_LAST_IN_FIRST_OUT_FIXED_SIZE_SEMAPHORE
  "Build libprocess with LIFO fixed size semaphore."
  FALSE)

option(
  PYTHON
  "Command for the Python interpreter, set to `python` if not given."
  "python")

option(
  PYTHON_3
  "Command for the Python 3 interpreter, set to the option PYTHON if not given."
  "")

option(
  ENABLE_NEW_CLI
  "Build the new CLI instead of the old one."
  FALSE)

if (ENABLE_NEW_CLI)
  # We always want to have PYTHON_3 set as it will be used to build the CLI.
  if (NOT PYTHON_3)
    if (PYTHON)
      # Set PYTHON_3 to PYTHON if PYTHON is set but not PYTHON_3.
      set(PYTHON_3 ${PYTHON})
    else ()
      # Set PYTHON_3 to the one CMake finds if PYTHON is not set.
      # PythonInterp sets PYTHON_EXECUTABLE by looking for an interpreter
      # from newest to oldest,, we then use it to set PYTHON and PYTHON_3.
      find_package(PythonInterp)
      if (NOT PYTHONINTERP_FOUND)
        message(FATAL_ERROR "You must have Python set up in order to continue.")
      endif ()
      set(PYTHON ${PYTHON_EXECUTABLE})
      set(PYTHON_3 ${PYTHON})
    endif ()
  endif ()

  # Find `tox` for testing `src/python/lib/`.
  find_program(TOX tox)
  if (NOT TOX)
    message(FATAL_ERROR "'tox' is required in order to run Mesos Python library tests.")
  endif ()

  execute_process(
    COMMAND ${PYTHON_3} -c
      "import sys; print('%d.%d' % (sys.version_info[0], sys.version_info[1]))"
    OUTPUT_VARIABLE PYTHON_3_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE)

  if (PYTHON_3_VERSION VERSION_LESS "3.6.0")
    message(FATAL_ERROR
    "You must be running python 3.6 or newer in order to continue.\n"
    "You appear to be running Python ${PYTHON_3_VERSION}.\n"
    "Set the CMake option 'PYTHON_3' to define which interpreter to use.")
  endif ()
endif ()

option(
  ENABLE_JAVA
  "Build Java components. Warning: this is SLOW."
  FALSE)

if (ENABLE_JAVA)
  include(FindJava)
  find_package(Java COMPONENTS Development)

  if (NOT JAVA_FOUND)
    message(FATAL_ERROR "Java was not found!")
  endif ()

  include(FindJNI)
  if (NOT JNI_FOUND)
    message(FATAL_ERROR "JNI Java libraries were not found!")
  endif ()

  find_program(MVN mvn)
  if (NOT MVN)
    message(FATAL_ERROR "Maven was not found!")
  endif ()

  if (Java_FOUND AND JNI_FOUND AND MVN)
    set(HAS_JAVA TRUE)
  endif ()
endif ()

# If 'REBUNDLED' is set to FALSE, this will cause Mesos to build against the
# specified dependency repository. This is especially useful for Windows
# builds, because building on MSVC 1900 requires newer versions of some
# dependencies than the ones bundled in the Mesos repository.
set(
  3RDPARTY_DEPENDENCIES "https://github.com/mesos/3rdparty/raw/master"
  CACHE STRING
    "URL or filesystem path with a fork of the canonical 3rdparty repository")

if (WIN32 AND REBUNDLED)
  message(
    WARNING
    "On Windows, the required versions of:\n"
    "  * curl\n"
    "  * apr\n"
    "  * zlib\n"
    "do not come rebundled in the Mesos repository.  They will be downloaded from "
    "the Internet, even though the `REBUNDLED` flag was set.")
endif ()

if (WIN32 AND ENABLE_LIBEVENT)
  message(
    WARNING
    "The Windows imlementation of libevent is BUGGY. Use it at your own risk. "
    "It does NOT support async I/O. You WILL have problems. Tests WILL fail. "
    "This is NOT supported, and WILL eventually be removed. "
    "When not explicitly enabled, the build will default to the native Windows "
    "IOCP implementation, `libwinio`, built on the Windows Thread Pool API. "
    "See MESOS-8668 for context.")
endif ()


# SYSTEM CHECKS.
################

# Set the default standard to C++11 for all targets.
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Do not use, for example, `-std=gnu++11`.
set(CMAKE_CXX_EXTENSIONS OFF)

# Check that we are targeting a 64-bit architecture.
if (NOT (CMAKE_SIZEOF_VOID_P EQUAL 8))
  message(
    FATAL_ERROR
    "Mesos requires that we compile to a 64-bit target. Following are some "
    "examples of how to accomplish this on some well-used platforms:\n"
    "  * Linux: (on gcc) set `CMAKE_CXX_FLAGS` to include `-m64`:\n"
    "    `cmake -DCMAKE_CXX_FLAGS=-m64 `.\n"
    "  * Windows: use the VS win64 CMake generator:\n"
    "    `cmake -G \"Visual Studio 15 2017 Win64\"`.\n"
    "  * OS X: add `x86_64` to the `CMAKE_OSX_ARCHITECTURES`:\n"
    "    `cmake -DCMAKE_OSX_ARCHITECTURES=x86_64`.\n")
endif ()

if (WIN32)
  # Versions of Visual Studio older than 2017 do not support all core features
  # of C++14, which prevents Mesos from moving past C++11. This adds a
  # non-fatal deprecation warning.
  set(PREFERRED_GENERATOR "Visual Studio 15 2017")
  if (NOT CMAKE_GENERATOR MATCHES ${PREFERRED_GENERATOR})
    message(
      WARNING
      "Mesos does not officially support ${CMAKE_GENERATOR}. "
      "Please use ${PREFERRED_GENERATOR}.")
  endif ()

  # We don't support compilation against mingw headers (which, e.g., Clang on
  # Windows does at this point), because this is likely to cost us more effort
  # to support than it will be worth at least in the short term.
  if (NOT CMAKE_CXX_COMPILER_ID MATCHES MSVC)
    message(
      FATAL_ERROR
      "Mesos does not support compiling on Windows with "
      "${CMAKE_CXX_COMPILER_ID}. Please use MSVC.")
  endif ()

  # MSVC 1900 supports C++11; earlier versions don't. So, warn if you try to
  # use anything else.
  if (${MSVC_VERSION} LESS 1900)
    message(
      FATAL_ERROR
      "Mesos does not support compiling on MSVC versions earlier than 1900. "
      "Please use MSVC 1900 (included with Visual Studio 2015 or later).")
  endif ()
endif ()

# GLOBAL WARNINGS.
##################
if (CMAKE_CXX_COMPILER_ID MATCHES GNU
    OR CMAKE_CXX_COMPILER_ID MATCHES Clang) # Also matches AppleClang.
  # TODO(andschwa): Add `-Wextra`, `-Wpedantic`, `-Wconversion`.
  add_compile_options(
    -Wall
    -Wsign-compare)
elseif (CMAKE_CXX_COMPILER_ID MATCHES MSVC)
  # TODO(andschwa): Switch to `/W4` and re-enable possible-loss-of-data warnings.
  #
  # The last two warnings are disabled (well, put into `/W4`) because
  # there is no easy equivalent to enable them for GCC/Clang without
  # also fixing all the warnings from `-Wconversion`.
  add_compile_options(
    # Like `-Wall`; `/W4` is more like `-Wall -Wextra`.
    /W3
    # Disable permissiveness.
    /permissive-
    # C4244 is a possible loss of data warning for integer conversions.
    /w44244
    # C4267 is a possible loss of data warning when converting from `size_t`.
    /w44267)
endif ()

if (CMAKE_CXX_COMPILER_ID MATCHES Clang)
  add_compile_options(-Wno-inconsistent-missing-override)
endif ()

# POSIX CONFIGURATION.
######################
if (NOT WIN32)
  # Warn about use of format functions that can produce security issues.
  add_compile_options(-Wformat-security)

  # Protect many of the functions with stack guards. The exact flag
  # depends on compiler support.
  CHECK_CXX_COMPILER_FLAG(-fstack-protector-strong STRONG_STACK_PROTECTORS)
  CHECK_CXX_COMPILER_FLAG(-fstack-protector STACK_PROTECTORS)
  if (STRONG_STACK_PROTECTORS)
    add_compile_options(-fstack-protector-strong)
  elseif (STACK_PROTECTORS)
    add_compile_options(-fstack-protector)
  else ()
    message(
      WARNING
      "The compiler ${CMAKE_CXX_COMPILER} cannot apply stack protectors.")
  endif ()

  # Do not omit frame pointers in debug builds to ease debugging and profiling.
  if ((CMAKE_BUILD_TYPE MATCHES Debug) OR
      (CMAKE_BUILD_TYPE MATCHES RelWithDebInfo))
    add_compile_options(-fno-omit-frame-pointer)
  endif ()

  # Directory structure for some build artifacts.
  # This is defined for use in tests.
  set(EXEC_INSTALL_PREFIX  ${CMAKE_INSTALL_PREFIX})
  set(SHARE_INSTALL_PREFIX ${CMAKE_INSTALL_PREFIX}/share)
  set(DATA_INSTALL_PREFIX  ${SHARE_INSTALL_PREFIX}/mesos)

  set(LIBEXEC_INSTALL_DIR     ${EXEC_INSTALL_PREFIX}/libexec)
  set(PKG_LIBEXEC_INSTALL_DIR ${LIBEXEC_INSTALL_DIR}/mesos)
  set(LIB_INSTALL_DIR         ${EXEC_INSTALL_PREFIX}/libmesos)
endif ()

option(ENABLE_GC_UNUSED
  "Enable garbage collection of unused program segments"
  FALSE)

if (ENABLE_GC_UNUSED)
  CMAKE_PUSH_CHECK_STATE()

  set(CMAKE_REQUIRED_FLAGS "-ffunction-sections -fdata-sections -Wl,--gc-sections")
  CHECK_CXX_COMPILER_FLAG("" GC_FUNCTION_SECTIONS)
  if (GC_FUNCTION_SECTIONS)
    add_compile_options(-ffunction-sections -fdata-sections)
    string(APPEND CMAKE_EXE_LINKER_FLAGS " -Wl,--gc-sections")
    string(APPEND CMAKE_SHARED_LINKER_FLAGS " -Wl,--gc-sections")
  else ()
    message(
      FATAL_ERROR
      "The compiler ${CMAKE_CXX_COMPILER} does not support the necessary options to "
      "enable garbage collection of unused sections.")
  endif()

  CMAKE_POP_CHECK_STATE()
endif()

# LINUX CONFIGURATION.
######################
string(COMPARE EQUAL ${CMAKE_SYSTEM_NAME} "Linux" LINUX)

if (LINUX)
  # We currenty only support using the bundled jemalloc on linux.
  # While building it and linking against is actually not a problem
  # on other platforms, to make it actually *useful* we need some
  # additional platform-specific code in the mesos binaries that re-routes
  # all existing malloc/free calls through jemalloc.
  # On linux, that is not necessary because the default malloc implementation
  # explicitly supports replacement via symbol interposition.
  option(
    ENABLE_JEMALLOC_ALLOCATOR
    "Use jemalloc as memory allocator for the master and agent binaries."
    FALSE)

  option(ENABLE_XFS_DISK_ISOLATOR
    "Whether to enable the XFS disk isolator."
    FALSE)

  if (ENABLE_XFS_DISK_ISOLATOR)
    # TODO(andschwa): Check for required headers and libraries.
    message(FATAL_ERROR
      "The XFS disk isolator is not yet supported, see MESOS-9117.")
  endif ()

  option(ENABLE_LAUNCHER_SEALING
    "Whether to enable containerizer launcher sealing via memfd."
    FALSE)

  option(ENABLE_PORT_MAPPING_ISOLATOR
    "Whether to enable the port mapping isolator."
    FALSE)

  if (ENABLE_PORT_MAPPING_ISOLATOR)
    # TODO(andschwa): Check for `libnl-3`.
    message(FATAL_ERROR
      "The port mapping isolator is not yet supported, see MESOS-8993.")
  endif ()

  option(ENABLE_NETWORK_PORTS_ISOLATOR
    "Whether to enable the network ports isolator."
    FALSE)

  if (ENABLE_NETWORK_PORTS_ISOLATOR)
    # TODO(andschwa): Check for `libnl-3`.
    message(FATAL_ERROR
      "The network ports isolator is not yet supported, see MESOS-8993.")
  endif ()

  # Enabled when either the port mapping isolator or network ports
  # isolator is enabled.
  if (ENABLE_PORT_MAPPING_ISOLATOR OR ENABLE_NETWORK_PORTS_ISOLATOR)
    set(ENABLE_LINUX_ROUTING TRUE)
  endif ()

  option(
    ENABLE_SECCOMP_ISOLATOR
    "Whether to enable `linux/seccomp` isolator."
    FALSE)
endif ()

# FREEBSD CONFIGURATION.
######################
string(COMPARE EQUAL ${CMAKE_SYSTEM_NAME} "FreeBSD" FREEBSD)

# There is a problem linking with BFD linkers when using Clang on
# FreeBSD (MESOS-8761). CMake uses the compiler to link, and the
# compiler uses `/usr/bin/ld` by default. On FreeBSD the default
# compiler is Clang but the default linker is GNU ld (BFD). Since LLD
# is available in the base system, and GOLD is available from
# `devel/binutils`, we look for a more modern linker and tell Clang to
# use that instead.
#
# TODO(dforsyth): Understand why this is failing and add a check to
# make sure we have a compatible linker (MESOS-8765), or wait until
# FreeBSD makes lld the default linker.
if (${CMAKE_SYSTEM_NAME} MATCHES FreeBSD
    AND CMAKE_CXX_COMPILER_ID MATCHES Clang)

  find_program(LD_PROGRAM
    NAMES ld.lld ld.gold)

  if (NOT LD_PROGRAM)
    message(FATAL_ERROR
      "Please set LD_PROGRAM to a working (non-BFD) linker (MESOS-8761) to \
      build on FreeBSD.")
  endif ()

  foreach (type EXE SHARED STATIC MODULE)
    string(APPEND CMAKE_${type}_LINKER_FLAGS " -fuse-ld=${LD_PROGRAM}")
  endforeach ()
endif ()

# WINDOWS CONFIGURATION.
########################
if (WIN32)
  # COFF/PE and friends are somewhat limited in the number of sections they
  # allow for an object file. We use this to avoid those problems.
  add_compile_options(/bigobj /vd2)

  # Fix Warning C4530: C++ exception handler used, but unwind semantics are not
  # enabled.
  add_compile_options(/EHsc)

  # Build against the multi-threaded version of the C runtime library (CRT).
  if (BUILD_SHARED_LIBS)
    message(WARNING "Building with shared libraries is a work-in-progress.")
    set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS TRUE)
  endif ()

  if (ENABLE_SSL)
    # NOTE: We don't care about using the debug version because OpenSSL includes
    # an adapter. However, we prefer OpenSSL to use the multi-threaded CRT.
    set(OPENSSL_MSVC_STATIC_RT TRUE)
  endif ()

  # Enable multi-threaded compilation for `cl.exe`.
  add_compile_options(/MP)

  # Force use of Unicode C and C++ Windows APIs.
  add_definitions(-DUNICODE -D_UNICODE)

  # Convenience flags to simplify Windows support in C++ source; used to
  # `#ifdef` out some platform-specific parts of Mesos.  We choose to define
  # a new flag rather than using an existing flag (`_WIN32`) because we want
  # to give the build system fine-grained control over what code is #ifdef'd
  # out in the future.  Using only flags defined by our build system to control
  # this logic is the clearest and most stable way of accomplishing this.
  add_definitions(-D__WINDOWS__)

  # Defines to disable warnings generated by Visual Studio when using
  # deprecated functions in CRT and the use of insecure functions in CRT.
  # TODO(dpravat): Once the entire codebase is changed to use secure CRT
  # functions, these defines should be removed.
  add_definitions(
    -D_SCL_SECURE_NO_WARNINGS
    -D_CRT_SECURE_NO_WARNINGS
    -D_CRT_NONSTDC_NO_WARNINGS)

  # Directory structure definitions.
  # TODO(hausdorff): (MESOS-5455) These are placeholder values.
  # Transition away from them.
  set(EXEC_INSTALL_PREFIX     "WARNINGDONOTUSEME")
  set(LIBEXEC_INSTALL_DIR     "WARNINGDONOTUSEME")
  set(PKG_LIBEXEC_INSTALL_DIR "WARNINGDONOTUSEME")
  set(LIB_INSTALL_DIR         "WARNINGDONOTUSEME")
  set(TEST_LIB_EXEC_DIR       "WARNINGDONOTUSEME")
  set(PKG_MODULE_DIR          "WARNINGDONOTUSEME")
  set(S_BIN_DIR               "WARNINGDONOTUSEME")
endif ()


# GLOBAL CONFIGURATION.
#######################
# Produce position independent libraries/executables so that we take
# better advantage of Address space layout randomization (ASLR).
# This helps guard against ROP and return-to-libc attacks,
# and other general exploits that rely on deterministic offsets.
set(CMAKE_POSITION_INDEPENDENT_CODE TRUE)

# TODO(andschwa): Make these non-global.
add_definitions(
  -DPKGLIBEXECDIR="${PKG_LIBEXEC_INSTALL_DIR}"
  -DLIBDIR="${LIB_INSTALL_DIR}"
  -DVERSION="${PACKAGE_VERSION}"
  -DPKGDATADIR="${DATA_INSTALL_PREFIX}")

if (ENABLE_SSL)
  # TODO(andschwa): Make this non-global.
  add_definitions(-DUSE_SSL_SOCKET=1)
endif ()

if (ENABLE_LIBEVENT)
  add_definitions(-DUSE_LIBEVENT=1)
endif ()

# Calculate some build information.
string(TIMESTAMP BUILD_DATE "%Y-%m-%d %H:%M:%S UTC" UTC)
string(TIMESTAMP BUILD_TIME "%s" UTC)
if (WIN32)
  set(BUILD_USER $ENV{USERNAME})
else ()
  set(BUILD_USER $ENV{USER})
endif ()

# NOTE: This is not quite the same as the Autotools build, as most definitions,
# include directories, etc. are embedded as target properties within the CMake
# graph. However, this is simply a "helper" variable anyway, so providing the
# "global" compile definitions (at least, those of this directory), is close
# enough to the intent.
#
# This code sets the variable `BUILD_FLAGS_RAW` to the content of the
# directory's `COMPILE_DEFINITIONS` property. The backslashes are then escaped
# and the final string is saved into the `BUILD_FLAGS` variable.
get_directory_property(BUILD_FLAGS_RAW COMPILE_DEFINITIONS)
string(REPLACE "\"" "\\\"" BUILD_FLAGS "${BUILD_FLAGS_RAW}")

set(BUILD_JAVA_JVM_LIBRARY ${JAVA_JVM_LIBRARY})

# Emit the BUILD_DATE, BUILD_TIME and BUILD_USER definitions into a file.
# This will be updated each time `cmake` is run.
configure_file(
  "${CMAKE_SOURCE_DIR}/src/common/build_config.hpp.in"
  "${CMAKE_BINARY_DIR}/src/common/build_config.hpp"
  @ONLY)

# Create 'src/common/git_version.hpp' only if we did not do so before.
# This protects the results from getting overwritten by additional cmake
# runs outside the reach of the git repository.
if(NOT EXISTS "${CMAKE_BINARY_DIR}/src/common/git_version.hpp")
  # When building from a git clone, the definitions BUILD_GIT_SHA,
  # BUILD_GIT_BRANCH and BUILD_GIT_TAG will be emitted.
  if (IS_DIRECTORY "${CMAKE_SOURCE_DIR}/.git")
    # Optionally set BUILD_GIT_SHA.
    set(DEFINE_BUILD_GIT_SHA "")

    execute_process(
      COMMAND git rev-parse HEAD
      OUTPUT_VARIABLE BUILD_GIT_SHA
      WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
      ERROR_QUIET
      OUTPUT_STRIP_TRAILING_WHITESPACE)

    if (NOT BUILD_GIT_SHA STREQUAL "")
      set(DEFINE_BUILD_GIT_SHA "#define BUILD_GIT_SHA \"${BUILD_GIT_SHA}\"")
    endif()

    # Optionally set BUILD_GIT_BRANCH.
    set(DEFINE_BUILD_GIT_BRANCH "")

    execute_process(
      COMMAND git symbolic-ref HEAD
      OUTPUT_VARIABLE BUILD_GIT_BRANCH
      WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
      ERROR_QUIET
      OUTPUT_STRIP_TRAILING_WHITESPACE)

    if (NOT BUILD_GIT_BRANCH STREQUAL "")
      set(DEFINE_BUILD_GIT_BRANCH "#define BUILD_GIT_BRANCH \"${BUILD_GIT_BRANCH}\"")
    endif()

    # Optionally set BUILD_GIT_TAG.
    set(DEFINE_BUILD_GIT_TAG "")

    execute_process(
      COMMAND git describe --exact --tags
      OUTPUT_VARIABLE BUILD_GIT_TAG
      WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
      ERROR_QUIET
      OUTPUT_STRIP_TRAILING_WHITESPACE)

    if (NOT BUILD_GIT_TAG STREQUAL "")
      set(DEFINE_BUILD_GIT_TAG "#define BUILD_GIT_TAG \"${BUILD_GIT_TAG}\"")
    endif()
  endif ()

  configure_file(
    "${CMAKE_SOURCE_DIR}/src/common/git_version.hpp.in"
    "${CMAKE_BINARY_DIR}/src/common/git_version.hpp"
    @ONLY)
endif()
