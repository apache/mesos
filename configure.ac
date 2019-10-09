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

# Generated with autoscan, then modified appropriately.
# Process this file with autoconf to produce a configure script.

AC_PREREQ([2.61])
AC_INIT([mesos], [1.10.0])

# Have autoconf setup some variables related to the system.
AC_CANONICAL_HOST
AC_CANONICAL_BUILD
AC_CANONICAL_TARGET
AC_DISABLE_STATIC

AC_LANG([C++])

# Default to gcc toolchain (we rely on some atomic builtins for now,
# that are also present with clang).
AC_PROG_CXX([g++])
AC_PROG_CC([gcc])

AC_PROG_LN_S

AX_COMPILER_VERSION
AX_COMPILER_VENDOR

AC_CONFIG_MACRO_DIR([m4])

# Initialize automake.
# -Wno-portability, since we require GNU Make for % patterns
# Removal of -Werror due warnings introduced by automake 1.14.
AM_INIT_AUTOMAKE([-Wall -Wno-portability foreign tar-pax])
AM_MAINTAINER_MODE([enable])

# This is required for linking non-POSIX libs.
m4_ifdef([AM_PROG_AR], [AM_PROG_AR])

# Initialize libtool (LT_OUTPUT builds ./libtool immediately, needed
# if we want to do tests with libtool during configuration).
LT_PREREQ([2.2])
LT_INIT
LT_LANG([C++])
LT_OUTPUT

# Save the configure arguments so we can pass them to any third-party
# libraries that we might run configure on (see 3rdparty/Makefile.am).
# One downside of our strategy for shipping and building third-party
# libraries is that we can't expose options from nested third-party
# configure scripts.
CONFIGURE_ARGS="$ac_configure_args"
AC_SUBST(CONFIGURE_ARGS)

AM_CONDITIONAL([STANDALONE_LIBPROCESS], [false])
AM_CONDITIONAL([STANDALONE_STOUT], [false])

AC_CONFIG_FILES([Makefile mesos.pc])
AC_CONFIG_FILES([src/Makefile])
AC_CONFIG_FILES([3rdparty/Makefile])
AC_CONFIG_FILES([3rdparty/libprocess/Makefile])
AC_CONFIG_FILES([3rdparty/libprocess/include/Makefile])
AC_CONFIG_FILES([3rdparty/stout/Makefile])
AC_CONFIG_FILES([3rdparty/stout/include/Makefile])
AC_CONFIG_FILES([3rdparty/gmock_sources.cc])

AC_CONFIG_FILES(
  [bin/mesos.sh],
  [chmod +x bin/mesos.sh])
AC_CONFIG_FILES(
  [bin/mesos-agent.sh],
  [chmod +x bin/mesos-agent.sh])
AC_CONFIG_FILES(
  [bin/mesos-local.sh],
  [chmod +x bin/mesos-local.sh])
AC_CONFIG_FILES(
  [bin/mesos-master.sh],
  [chmod +x bin/mesos-master.sh])
# TODO(tomxing): Remove this script once the
# slave->agent rename is complete(MESOS-3782).
AC_CONFIG_FILES(
  [bin/mesos-slave.sh:bin/mesos-agent.sh.in],
  [chmod +x bin/mesos-slave.sh])
AC_CONFIG_FILES(
  [bin/mesos-tests.sh],
  [chmod +x bin/mesos-tests.sh])
AC_CONFIG_FILES(
  [bin/mesos-agent-flags.sh],
  [chmod +x bin/mesos-agent-flags.sh])
AC_CONFIG_FILES(
  [bin/mesos-local-flags.sh],
  [chmod +x bin/mesos-local-flags.sh])
AC_CONFIG_FILES(
  [bin/mesos-master-flags.sh],
  [chmod +x bin/mesos-master-flags.sh])
# TODO(tomxing): Remove this script once the
# slave->agent rename is complete(MESOS-3782).
AC_CONFIG_FILES(
  [bin/mesos-slave-flags.sh:bin/mesos-agent-flags.sh.in],
  [chmod +x bin/mesos-slave-flags.sh])
AC_CONFIG_FILES(
  [bin/mesos-tests-flags.sh],
  [chmod +x bin/mesos-tests-flags.sh])
AC_CONFIG_FILES(
  [bin/gdb-mesos-agent.sh],
  [chmod +x bin/gdb-mesos-agent.sh])
AC_CONFIG_FILES(
  [bin/gdb-mesos-local.sh],
  [chmod +x bin/gdb-mesos-local.sh])
AC_CONFIG_FILES(
  [bin/gdb-mesos-master.sh],
  [chmod +x bin/gdb-mesos-master.sh])
# TODO(tomxing): Remove this script once the
# slave->agent rename is complete(MESOS-3782).
AC_CONFIG_FILES(
  [bin/gdb-mesos-slave.sh:bin/gdb-mesos-agent.sh.in],
  [chmod +x bin/gdb-mesos-slave.sh])
AC_CONFIG_FILES(
  [bin/gdb-mesos-tests.sh],
  [chmod +x bin/gdb-mesos-tests.sh])
AC_CONFIG_FILES(
  [bin/lldb-mesos-agent.sh],
  [chmod +x bin/lldb-mesos-agent.sh])
AC_CONFIG_FILES(
  [bin/lldb-mesos-local.sh],
  [chmod +x bin/lldb-mesos-local.sh])
AC_CONFIG_FILES(
  [bin/lldb-mesos-master.sh],
  [chmod +x bin/lldb-mesos-master.sh])
# TODO(tomxing): Remove this script once the
# slave->agent rename is complete(MESOS-3782).
AC_CONFIG_FILES(
  [bin/lldb-mesos-slave.sh:bin/lldb-mesos-agent.sh.in],
  [chmod +x bin/lldb-mesos-slave.sh])
AC_CONFIG_FILES(
  [bin/lldb-mesos-tests.sh],
  [chmod +x bin/lldb-mesos-tests.sh])
AC_CONFIG_FILES(
  [bin/valgrind-mesos-agent.sh],
  [chmod +x bin/valgrind-mesos-agent.sh])
AC_CONFIG_FILES(
  [bin/valgrind-mesos-local.sh],
  [chmod +x bin/valgrind-mesos-local.sh])
AC_CONFIG_FILES(
  [bin/valgrind-mesos-master.sh],
  [chmod +x bin/valgrind-mesos-master.sh])
# TODO(tomxing): Remove this script once the
# slave->agent rename is complete(MESOS-3782).
AC_CONFIG_FILES(
  [bin/valgrind-mesos-slave.sh:bin/valgrind-mesos-agent.sh.in],
  [chmod +x bin/valgrind-mesos-slave.sh])
AC_CONFIG_FILES(
  [bin/valgrind-mesos-tests.sh],
  [chmod +x bin/valgrind-mesos-tests.sh])
AC_CONFIG_FILES(
  [src/deploy/mesos-daemon.sh],
  [chmod +x src/deploy/mesos-daemon.sh])
AC_CONFIG_FILES(
  [src/deploy/mesos-start-agents.sh],
  [chmod +x src/deploy/mesos-start-agents.sh])
AC_CONFIG_FILES(
  [src/deploy/mesos-start-cluster.sh],
  [chmod +x src/deploy/mesos-start-cluster.sh])
AC_CONFIG_FILES(
  [src/deploy/mesos-start-masters.sh],
  [chmod +x src/deploy/mesos-start-masters.sh])
# TODO(tomxing): Remove this script once the
# slave->agent rename is complete(MESOS-3782).
AC_CONFIG_FILES(
  [src/deploy/mesos-start-slaves.sh:src/deploy/mesos-start-agents.sh.in],
  [chmod +x src/deploy/mesos-start-slaves.sh])
AC_CONFIG_FILES(
  [src/deploy/mesos-stop-agents.sh],
  [chmod +x src/deploy/mesos-stop-agents.sh])
AC_CONFIG_FILES(
  [src/deploy/mesos-stop-cluster.sh],
  [chmod +x src/deploy/mesos-stop-cluster.sh])
AC_CONFIG_FILES(
  [src/deploy/mesos-stop-masters.sh],
  [chmod +x src/deploy/mesos-stop-masters.sh])
# TODO(tomxing): Remove this script once the
# slave->agent rename is complete(MESOS-3782).
AC_CONFIG_FILES(
  [src/deploy/mesos-stop-slaves.sh:src/deploy/mesos-stop-agents.sh.in],
  [chmod +x src/deploy/mesos-stop-slaves.sh])

AC_CONFIG_FILES([include/mesos/version.hpp])

AC_CONFIG_FILES([src/java/generated/org/apache/mesos/MesosNativeLibrary.java])

AC_CONFIG_FILES([mpi/mpiexec-mesos], [chmod +x mpi/mpiexec-mesos])


###############################################################################
# Optional features.
###############################################################################


AC_ARG_ENABLE([bundled],
              AS_HELP_STRING([--disable-bundled],
                             [build against preinstalled dependencies instead
                              of bundled libraries]),
              [], [enable_bundled=yes])

AC_ARG_ENABLE([bundled-pip],
              AS_HELP_STRING([--disable-bundled-pip],
                             [excludes building and using the bundled pip
                              package in lieu of an installed version in
                              PYTHONPATH]),
              [], [enable_bundled_pip=yes])

AC_ARG_ENABLE([bundled-setuptools],
              AS_HELP_STRING([--disable-bundled-setuptools],
                             [excludes building and using the bundled setuptools
                              package in lieu of an installed version in
                              PYTHONPATH]),
              [], [enable_bundled_setuptools=yes])

AC_ARG_ENABLE([bundled-wheel],
              AS_HELP_STRING([--disable-bundled-wheel],
                             [excludes building and using the bundled wheel
                              package in lieu of an installed version in
                              PYTHONPATH]),
              [], [enable_bundled_wheel=yes])

AC_ARG_ENABLE([debug],
              AS_HELP_STRING([--enable-debug],
                             [enable debugging. If CFLAGS/CXXFLAGS are set, this
                              option will not change them]),
              [], [enable_debug=no])

AC_ARG_ENABLE([gc-unused],
              AS_HELP_STRING([--enable-gc-unused],
                             [enable garbage collection of unused program segments]),
                             [], [enable_gc_unused=no])

AC_ARG_ENABLE([hardening],
              AS_HELP_STRING([--disable-hardening],
                             [disables security measures such as stack
                             protection and position independent library code]),
                             [], [enable_hardening=yes])

AC_ARG_ENABLE([install-module-dependencies],
              AS_HELP_STRING([--enable-install-module-dependencies],
                             [Install third-party bundled dependencies required
                              for module development]),
              [], [enable_install_module_dependencies=no])

AC_ARG_ENABLE([network-ports-isolator],
              AS_HELP_STRING([--enable-network-ports-isolator],
                             [enable network ports isolator]),
              [], [enable_network_ports_isolator=no])

AC_ARG_ENABLE([java],
              AS_HELP_STRING([--disable-java],
                             [do not build Java bindings]),
              [], [enable_java=yes])

# TODO(bevers): Eventually make this enabled by default.
AC_ARG_ENABLE([jemalloc-allocator],
               AS_HELP_STRING([--enable-jemalloc-allocator],
                              [Use jemalloc as the default memory allocator for the
                               mesos-master and mesos-agent binaries.]),
               [], [enable_jemalloc_allocator=no])

# TODO(benh): Eventually make this enabled by default.
AC_ARG_ENABLE([last_in_first_out_fixed_size_semaphore],
              AS_HELP_STRING([--enable-last-in-first-out-fixed-size-semaphore],
                             [enables the optimized LIFO fixed-size semaphore in libprocess]),
                             [], [enable_last_in_first_out_fixed_size_semaphore=no])

AC_ARG_ENABLE([launcher_sealing],
              AS_HELP_STRING([--enable-launcher-sealing],
                             [Enables sealing of launcher binary during
                              initialization of Mesos containerizer.
                              A copy of launcher's binary is cloned into
                              memory and sealed to prevent overwriting of
                              launcher's executable by containers.
                              This is a Linux-specific feature and requires
                              the support of memfd by the kernel.]),
                             [], [enable_launcher_sealing=no])

AC_ARG_ENABLE([libevent],
              AS_HELP_STRING([--enable-libevent],
                             [use libevent instead of libev]),
              [], [enable_libevent=no])

# TODO(benh): Eventually make this enabled by default.
AC_ARG_ENABLE([lock_free_event_queue],
              AS_HELP_STRING([--enable-lock-free-event-queue],
                             [enables the lock-free event queue in libprocess]),
                             [], [enable_lock_free_event_queue=no])

# TODO(benh): Eventually make this enabled by default.
AC_ARG_ENABLE([lock_free_run_queue],
              AS_HELP_STRING([--enable-lock-free-run-queue],
                             [enables the lock-free run queue in libprocess]),
                             [], [enable_lock_free_run_queue=no])

AC_ARG_ENABLE([new_cli],
              AS_HELP_STRING([--enable-new-cli],
                             [enable building the new CLI instead of the old one]),
              [], [enable_new_cli=no])

AC_ARG_ENABLE([libtool-wrappers],
              AS_HELP_STRING([--disable-libtool-wrappers],
                             [Directly create binaries instead of libtool
                              wrapper scripts to prevent relinking when first
                              executing binaries from the build directory. This
                              flag leads to binaries with the build directory
                              encoded in the executables' RPATH which makes them
                              in general unfit for installation.
                              USE WITH CARE.]))

AC_ARG_ENABLE([optimize],
              AS_HELP_STRING([--enable-optimize],
                             [enable optimizations. If CFLAGS/CXXFLAGS are set,
                              this option will not change them]),
              [], [enable_optimize=no])

AC_ARG_ENABLE([parallel_test_execution],
              AS_HELP_STRING([--disable-parallel-test-execution],
                             [disable the parallel runner used during make check]),
              [], [enable_parallel_test_execution=yes])

AC_ARG_ENABLE([perftools],
              AS_HELP_STRING([--enable-perftools],
                             [enable Google perftools]),
              [], [enable_perftools=no])

AC_ARG_ENABLE([port-mapping-isolator],
              AS_HELP_STRING([--enable-port-mapping-isolator],
                             [enable port mapping network isolator]),
              [], [enable_port_mapping_isolator=no])

AC_ARG_ENABLE([python],
              AS_HELP_STRING([--disable-python],
                             [do not build Python bindings]),
              [], [enable_python=yes])

# This is used to make distcheck and python install work together. For more
# information, see the comment above AM_DISTCHECK_CONFIGURE_FLAGS in
# Makefile.am.
AC_ARG_ENABLE([python-dependency-install],
              AS_HELP_STRING([--disable-python-dependency-install],
                             [when the python packages are installed during
                              make install, no external dependencies are
                              downloaded or installed]),
              [], [enable_python_dependency_install=yes])

AC_ARG_ENABLE([seccomp-isolator],
              AS_HELP_STRING([--enable-seccomp-isolator],
                             [Builds the Seccomp isolator which depends on
                             libseccomp and requires Linux kernel 3.5+
                             This flag will be deprecated in the future when
                             we drop support for the outdated Linux kernels.]),
              [], [enable_seccomp_isolator=no])

AC_ARG_ENABLE([ssl],
              AS_HELP_STRING([--enable-ssl],
                             [use ssl for libprocess communication]),
              [], [enable_ssl=no])

AC_ARG_ENABLE([static-unimplemented],
              AS_HELP_STRING([--enable-static-unimplemented],
                             [generate static assertion errors for unimplemented
                             functions]),
              [], [enable_static_unimplemented=no])

AC_ARG_ENABLE([tests-install],
              AS_HELP_STRING([--enable-tests-install],
                             [build and install tests and their helper tools]),
              [], [enable_tests_install=no])

AC_ARG_ENABLE([werror],
              AS_HELP_STRING([--disable-werror],
                             [do not treat compiler warnings as fatal errors]))

AC_ARG_ENABLE([xfs-disk-isolator],
              AS_HELP_STRING([--enable-xfs-disk-isolator],
                             [builds the XFS disk isolator]),
              [], [enable_xfs_disk_isolator=no])

###############################################################################
# Optional packages.
###############################################################################

AC_ARG_WITH([apr],
            AS_HELP_STRING([--with-apr=@<:@=DIR@:>@],
                           [specify where to locate the apr-1 library]),
            [], [])

AC_ARG_WITH([boost],
            AS_HELP_STRING([--with-boost@<:@=DIR@:>@],
                           [excludes building and using the bundled Boost
                           package in lieu of an installed version at a
                           location prefixed by the given path]),
            [without_bundled_boost=yes], [])

AC_ARG_WITH([concurrentqueue],
            AS_HELP_STRING([--with-concurrentqueue@<:@=DIR@:>@],
                           [excludes building and using the bundled concurrentqueue
                           package in lieu of an installed version at a
                           location prefixed by the given path]),
            [without_bundled_concurrentqueue=yes], [])

AC_ARG_WITH([curl],
            AS_HELP_STRING([--with-curl=@<:@=DIR@:>@],
                           [specify where to locate the curl library]),
            [], [])

AC_ARG_WITH([elfio],
            AS_HELP_STRING([--with-elfio@<:@=DIR@:>@],
                           [excludes building and using the bundled ELFIO
                           package in lieu of an installed version at a
                           location prefixed by the given path]),
            [without_bundled_elfio=yes], [])

AC_ARG_WITH([glog],
            AS_HELP_STRING([--with-glog@<:@=DIR@:>@],
                           [excludes building and using the bundled glog
                            package in lieu of an installed version at a
                            location prefixed by the given path]),
            [without_bundled_glog=yes], [])

AC_ARG_WITH([gmock],
            AS_HELP_STRING([--with-gmock@<:@=DIR@:>@],
                           [excludes building and using the bundled gmock
                            package in lieu of an installed version at a
                            location prefixed by the given path]),
            [without_bundled_gmock=yes], [])

AC_ARG_WITH([grpc],
            AS_HELP_STRING([--with-grpc@<:@=DIR@:>@],
                           [excludes building and using the bundled grpc
                            package in lieu of an installed version at a
                            location prefixed by the given path]),
            [without_bundled_grpc=yes], [])

AC_ARG_WITH([http-parser],
            AS_HELP_STRING([--with-http-parser@<:@=DIR@:>@],
                           [excludes building and using the bundled http-parser
                           package in lieu of an installed version at a
                           location prefixed by the given path]),
            [without_bundled_http_parser=yes], [])

AC_ARG_WITH([jemalloc],
            AS_HELP_STRING([--with-jemalloc@<:@=DIR@:>@],
                          [excludes building and using the bundled jemalloc
                          package in lieu of an installed version at a
                          location prefixed by the given path.]),
            [without_bundled_jemalloc=yes], [])

AC_ARG_WITH([leveldb],
            AS_HELP_STRING([--with-leveldb@<:@=DIR@:>@],
                           [excludes building and using the bundled LevelDB
                            package in lieu of an installed version at a
                            location prefixed by the given path]),
            [without_bundled_leveldb=yes], [])

AC_ARG_WITH([libev],
            AS_HELP_STRING([--with-libev@<:@=DIR@:>@],
                           [excludes building and using the bundled libev
                           package in lieu of an installed version at a
                           location prefixed by the given path]),
            [without_bundled_libev=yes], [])

AC_ARG_WITH([libevent],
            AS_HELP_STRING([--with-libevent=@<:@=DIR@:>@],
                           [specify where to locate the libevent library]),
            [without_bundled_libevent=yes], [])

AC_ARG_WITH([libarchive],
            AS_HELP_STRING([--with-libarchive=@<:@=DIR@:>@],
                           [specify where to locate the libarchive library]),
            [without_bundled_libarchive=yes], [])

AC_ARG_WITH([libprocess],
            AS_HELP_STRING([--with-libprocess=@<:@=DIR@:>@],
                           [specify where to locate the libprocess library]),
            [], [])

AC_ARG_WITH([libseccomp],
            AS_HELP_STRING([--with-libseccomp=@<:@=DIR@:>@],
                           [specify where to locate the libseccomp library]),
            [without_bundled_libseccomp=yes], [])

AC_ARG_WITH([network-isolator],
            AS_HELP_STRING([--with-network-isolator],
                           [builds the network isolator]),
            [AC_MSG_WARN([--with-network-isolator is being deprecated, please use --enable-port-mapping-isolator instead.]);
             enable_port_mapping_isolator=yes],
            [])

AC_ARG_WITH([nvml],
            AS_HELP_STRING([--with-nvml@<:@=DIR@:>@],
                           [excludes building and using the bundled NVML headers
                            in lieu of an installed version at a location
                            prefixed by the given path]),
            [without_bundled_nvml=yes], [])

AC_ARG_WITH([picojson],
            AS_HELP_STRING([--with-picojson@<:@=DIR@:>@],
                           [excludes building and using the bundled picojson
                           package in lieu of an installed version at a
                           location prefixed by the given path]),
            [without_bundled_picojson=yes], [])

AC_ARG_WITH([rapidjson],
            AS_HELP_STRING([--with-rapidjson@<:@=DIR@:>@],
                           [excludes building and using the bundled rapidjson
                           package in lieu of an installed version at a
                           location prefixed by the given path]),
            [without_bundled_rapidjson=yes], [])

AC_ARG_WITH([protobuf],
            AS_HELP_STRING([--with-protobuf@<:@=DIR@:>@],
                           [excludes building and using the bundled protobuf
                            package in lieu of an installed version at a
                            location prefixed by the given path]),
            [without_bundled_protobuf=yes], [])

AC_ARG_WITH([sasl],
            AS_HELP_STRING([--with-sasl=@<:@=DIR@:>@],
                           [specify where to locate the sasl2 library]),
            [], [])

AC_ARG_WITH([ssl],
            AS_HELP_STRING([--with-ssl=@<:@=DIR@:>@],
                           [specify where to locate the ssl library]),
            [], [])

AC_ARG_WITH([stout],
            AS_HELP_STRING([--with-stout=@<:@=DIR@:>@],
                           [specify where to locate stout library]),
            [], [])

AC_ARG_WITH([svn],
            AS_HELP_STRING([--with-svn=@<:@=DIR@:>@],
                           [specify where to locate the svn-1 library]),
            [], [])

AC_ARG_WITH([zlib],
            AS_HELP_STRING([--with-zlib=@<:@=DIR@:>@],
                           [specify where to locate the zlib library]),
            [], [])

AC_ARG_WITH([zookeeper],
            AS_HELP_STRING([--with-zookeeper@<:@=DIR@:>@],
                           [excludes building and using the bundled ZooKeeper
                            package in lieu of an installed version at a
                            location prefixed by the given path]),
            [without_bundled_zookeeper=yes], [])


###############################################################################
# Debug/Optimization checks.
###############################################################################

# Check if we should enable debugging, optimization. Note we only
# update CFLAGS and CXXFLAGS if none are provided.
AM_CONDITIONAL([DEBUG], [test x"$enable_debug" = "xyes"])
AM_CONDITIONAL([OPTIMIZE], [test x"$enable_optimize" = "xyes"])

debug_flags="-g1"
if test "x$enable_debug" = "xyes"; then
  debug_flags="-g -fno-omit-frame-pointer"
elif test "x$enable_optimize" = "xyes"; then
  debug_flags=""
fi

if test "x$enable_optimize" = "xyes"; then
  optimize_flags="-O2"
else
  optimize_flags="-O0"
fi

AS_IF([test "x${ac_cv_env_CFLAGS_set}" = "x"],
      [CFLAGS="$debug_flags $optimize_flags"])
AS_IF([test "x${ac_cv_env_CXXFLAGS_set}" = "x"],
      [CXXFLAGS="$debug_flags $optimize_flags"])


###############################################################################
# Compiler checks.
###############################################################################

# Check if we should use the LIFO fixed-size semaphore.
AS_IF([test "x$enable_last_in_first_out_fixed_size_semaphore" = "xyes"],
      [AC_DEFINE([LAST_IN_FIRST_OUT_FIXED_SIZE_SEMAPHORE])])

# Check if we should use the lock-free event queue.
AS_IF([test "x$enable_lock_free_event_queue" = "xyes"],
      [AC_DEFINE([LOCK_FREE_EVENT_QUEUE])])

# Check if we should use the lock-free run queue.
AS_IF([test "x$enable_lock_free_run_queue" = "xyes"],
      [AC_DEFINE([LOCK_FREE_RUN_QUEUE])])

# Check if we should link the mesos binaries against jemalloc.
AM_CONDITIONAL([ENABLE_JEMALLOC_ALLOCATOR],
         [test x"$enable_jemalloc_allocator" = "xyes"])

# Check to see if we should harden or not.
AM_CONDITIONAL([ENABLE_HARDENING], [test x"$enable_hardening" = "xyes"])

AC_MSG_CHECKING([whether to enable GC of unused sections])
AC_MSG_RESULT([$enable_gc_unused])

AS_IF([test "x$enable_gc_unused" = "xyes"], [
  AC_LANG_PUSH([C++])
  saved_CXXFLAGS="$CXXFLAGS"
  saved_LDFLAGS="$LDFLAGS"

  CXXFLAGS="-ffunction-sections -fdata-sections"
  LDFLAGS="-Wl,--gc-sections"
  AC_LINK_IFELSE(
    [ AC_LANG_PROGRAM([], [return 0;]) ],
    [],
    [
      AC_MSG_ERROR([cannot enable GC of unused sections
-------------------------------------------------------------------
The current toolchain does not support the necessary options to
enable garbage collection of unused sections.
-------------------------------------------------------------------
      ])
    ]
  )

  CXXFLAGS="$saved_CXXFLAGS"
  LDFLAGS="$saved_LDFLAGS"
  AC_LANG_POP()
])

AS_IF([test "x${enable_gc_unused}" = "xyes"], [
  CCFLAGS="$CCFLAGS -ffunction-sections -fdata-sections"
  CXXFLAGS="$CXXFLAGS -ffunction-sections -fdata-sections"
  LDFLAGS="$LDFLAGS -Wl,--gc-sections"
])

AS_CASE($ax_cv_cxx_compiler_vendor,
  [clang], [
    # Upstream Clang 3.6 emits `-Wunused-local-typedef` warnings when compiling
    # Boost so we suppress them here. Apple LLVM based on Clang 3.6 doesn't have
    # the same behavior. See: https://reviews.apache.org/r/32749/
    # NOTE(chhsiao): Clang 3.6.0 from the link below seems to compile Boost
    # 1.53.0/1.65.0 just fine: http://releases.llvm.org/download.html#3.6.0
    #
    # Clang 3.6+ emits `-Winconsistent-missing-override` warnings for most uses
    # of the Google Mock `MOCK_METHOD` family of macros so we suppress them
    # here. See: https://github.com/google/googletest/issues/533
    AX_APPEND_COMPILE_FLAGS([-Wno-unused-local-typedef \
                             -Wno-inconsistent-missing-override],
                            [CXXFLAGS], [-Werror])
  ], [gnu], [
    # Check for GCC version >= 4.8.
    AX_COMPARE_VERSION([$ax_cv_cxx_compiler_version], [ge], [4.8],
                       [is_ge_gxx48=yes], [is_ge_gxx48=no])
    if test "x$is_ge_gxx48" != "xyes"; then
      # GCC < 4.8 is not supported.
      AC_MSG_ERROR([GCC 4.8 or higher required (found $ax_cv_cxx_compiler_version)])
    fi

    AC_MSG_NOTICE([Setting up CXXFLAGS for g++ version >= 4.8])
    # Boost fails to compile with GCC 4.8+ without `-Wno-unused-local-typedefs`,
    # and automake does not recognize the flag.
    # TODO(brenden): Remove this when Boost has a resolution.
    CFLAGS="${CFLAGS} -Wno-unused-local-typedefs"
    CXXFLAGS="${CXXFLAGS} -Wno-unused-local-typedefs"

    # Disable 'maybe-uninitialized' warning when "-O2" is enabled due
    # to MESOS-2690. This is short term fix until the GCC bug is fixed.
    case $CXXFLAGS in
      *-O2*) CXXFLAGS="${CXXFLAGS} -Wno-maybe-uninitialized" ;;
      *) ;;
    esac
  ], [
    AC_MSG_WARN([$ax_cv_cxx_compiler_vendor is an unsupported compiler])
  ]
)

# If our compiler supports strong stack protection, use it.
# If not, use a lesser form of stack protection.
AX_CHECK_COMPILE_FLAG([-fstack-protector-strong],
  [AC_SUBST(STACK_PROTECTOR, "-fstack-protector-strong")],
  [AC_SUBST(STACK_PROTECTOR, "-fstack-protector")])

# Ensure that CXX supports C++11 (with "strict" conformance), and set
# "--std=" flag and CXXFLAGS environment variable as appropriate.
AX_CXX_COMPILE_STDCXX([11], [noext], [mandatory])

# Blacklist known incompatible C++ standard libraries to reduce chance
# of users accidentally using them.
#  - libstdc++ (Pre 4.8.0, version datestamp 20130322)
#  - libc++ (Pre 3.5.0, version string 1101)
AC_LANG_PUSH([C++])
AC_COMPILE_IFELSE(
[AC_LANG_SOURCE([[
#include <vector>

#ifdef __GLIBCXX__
#if __GLIBCXX__ < 20130322
#error Force the compiler to fail and set COMPATIBLE_CXX_STDLIB=no.
#endif
#elif _LIBCPP_VERSION < 1101
#error Force the compiler to fail and set COMPATIBLE_CXX_STDLIB=no.
#endif
]])],
[COMPATIBLE_CXX_STDLIB=yes], [COMPATIBLE_CXX_STDLIB=no])
AC_LANG_POP([C++])

if test "x$COMPATIBLE_CXX_STDLIB" = "xno"; then
  AC_MSG_ERROR([known incompatible C++ Standard library in use
-------------------------------------------------------------------
libstdc++ from GCC 4.8.0+ or libc++ from LLVM/Clang 3.5+ is required.

If a new enough GCC installed (4.8.0+) it should automatically find
the right libstdc++. If clang is in use, it will automatically find
the newest libstdc++ on the host.

If you are trying to use a different compiler from (clang, gcc) or a
alternate C++ Standard Library implementation, likely the flags being
passed to the compiler (CXXFLAGS) are not currently correct / being
picked up.
-------------------------------------------------------------------
])
fi

case "$host_os" in
  darwin* )
   # If we're using clang, we need to pass -stdlib=libc++ too.
    if test "x$ax_cv_cxx_compiler_vendor" = "xclang"; then
      CXXFLAGS="$CXXFLAGS -stdlib=libc++"
    fi
    ;;
  freebsd* )
    # If we're using clang, we need to pass -stdlib=libc++ too.
    if test "x$ax_cv_cxx_compiler_vendor" = "xclang"; then
      CXXFLAGS="$CXXFLAGS -stdlib=libc++"
    fi

    # FreeBSD needs GTEST_HAS_PTHREAD for this version of gmock/gtest and
    # _WITH_GETLINE for getline().
    CXXFLAGS="$CXXFLAGS -DGTEST_HAS_PTHREAD -D_WITH_GETLINE"

    # Add no-sentinal so that stout builds with system clang on 11.0 and later
    # (3.8+).
    CXXFLAGS="$CXXFLAGS -Wno-sentinel"
    ;;
esac


###############################################################################
# Miscellaneous flags/library/tool checks.
###############################################################################

AC_ARG_VAR([JAVA_HOME], [location of Java Development Kit (JDK)])

AC_ARG_VAR([JAVA_CPPFLAGS], [preprocessor flags for JNI])

AC_ARG_VAR([JAVA_JVM_LIBRARY], [full path to libjvm.so])

AC_ARG_VAR([MAVEN_HOME], [looks for mvn at MAVEN_HOME/bin/mvn])

AC_ARG_VAR([PROTOBUF_JAR], [full path to protobuf jar on prefixed builds])

AC_ARG_VAR([PYTHON], [which Python 2 interpreter to use])

AC_ARG_VAR([PYTHON_3], [which Python 3 interpreter to use])

AC_ARG_VAR([TEST_DRIVER], [executable and arguments of a test driver])

# Check for pthreads (uses m4/ax_pthread.m4).
AX_PTHREAD([], [AC_MSG_ERROR([failed to find pthreads])])

if test "x$enable_static_unimplemented" = "xyes"; then
  AC_DEFINE([ENABLE_STATIC_UNIMPLEMENTED], [1])
fi

# Build libprocess as a static library with position independent code so
# that we can produce a final shared library which includes everything
# necessary (and only install that).
AM_CONDITIONAL([ENABLE_STATIC_LIBPROCESS], [true])

AC_MSG_NOTICE([Setting up build environment for ${target_cpu} ${target_os}])
# Determine the current OS (TODO(benh): Does autotools do this for us?).
case "${host_os}" in
  linux*)
    OS_NAME=linux
    LIBS="$LIBS -lrt"
    ;;
  darwin*)
    OS_NAME=darwin
    ;;
  freebsd*)
    OS_NAME=freebsd
    ;;
  *)
    AC_MSG_ERROR("Mesos is currently unsupported on your platform.")
esac

AM_CONDITIONAL([DISABLE_LIBTOOL_WRAPPERS],
               [test x"$enable_libtool_wrappers" = "xno"])

# Used for conditionally including source files.
AM_CONDITIONAL([OS_LINUX], [test "x$OS_NAME" = "xlinux"])
AM_CONDITIONAL([OS_FREEBSD], [test "x$OS_NAME" = "xfreebsd"])

AM_CONDITIONAL([HAS_GPERFTOOLS], [test "x$enable_perftools" = "xyes"])

AM_CONDITIONAL([INSTALL_MODULE_DEPENDENCIES],
               [test x"$enable_install_module_dependencies" = "xyes"])
AS_IF([test "x$enable_install_module_dependencies" = "xyes"],
      [AC_SUBST(MODULE_CFLAGS, "-I${libdir}/mesos/3rdparty/include")
       AC_SUBST(MODULE_LDFLAGS, "-L${libdir}/mesos/3rdparty/lib64 -L${libdir}/mesos/3rdparty/lib")])

AM_CONDITIONAL([INSTALL_TESTS], [test x"$enable_tests_install" = "xyes"])
AS_IF([test x"$enable_tests_install" = "xyes"],
      [AC_DEFINE([MESOS_INSTALL_TESTS], [1])])

# If we use our own test runner to parallelize gtests we can run
# everything but ROOT tests in parallel.
# We here set up `TEST_DRIVER` to contain an unexpanded automake
# variable name; this allows us to reuse the test runner to run bundled
# 3rdparty checks. The special quoting ensures that this containing a
# space is not expanded by autoconf.
# TODO(bbannier): Make this more readible by using autoconf/m4 magic.
AS_IF([test "x$enable_parallel_test_execution" = "xyes"],
      [AC_SUBST([TEST_DRIVER],
       '${abs_top_srcdir}/support/mesos-gtest-runner.py'[' --sequential='*ROOT_*'']]))

# Define `WERROR` if the use of `-Werror` was not disabled, leave
# unset otherwise. An unset variable evaluates to an empty string
# in autoconf's substitution.
AS_IF([test "x$enable_werror" != "xno"],
      [AC_SUBST([WERROR], [-Werror])])

# Check for libunwind, and link it in if present.
if test "x$OS_NAME" = "xfreebsd"; then
  AC_CHECK_LIB(execinfo, backtrace, LIBS="$LIBS -lexecinfo")
else
  AC_CHECK_LIB(unwind, backtrace, LIBS="$LIBS -lunwind")
fi


# Check if libssl prefix path was provided, and if so, add it to
# the CPPFLAGS and LDFLAGS with respective /include and /lib path
# suffixes.
if test -z "`echo $with_ssl`" &&
   test "$OS_NAME" = "darwin" &&
   test -n "`command -v brew`" &&
   test -n "`brew list --versions openssl`"; then
  with_ssl=`brew --prefix openssl`
fi

if test -n "`echo $with_ssl`"; then
  SSL_CPPFLAGS="-I${with_ssl}/include"
  SSL_LINKERFLAGS="-L${with_ssl}/lib"

  CPPFLAGS="$SSL_CPPFLAGS $CPPFLAGS"
  LDFLAGS="$SSL_LINKERFLAGS $LDFLAGS"
fi

AC_CHECK_LIB([crypto], [RAND_poll], [found_crypto=yes])

AC_CHECK_HEADERS([openssl/ssl.h],
                 [AC_CHECK_LIB([ssl],
                               [SSL_CTX_new],
                               [found_ssl=yes],
                               [],
                               [-lcrypto])])

if test "x$enable_ssl" = "xyes"; then
  if test "x$found_ssl" != "xyes"; then
    AC_MSG_ERROR([cannot find libssl
-------------------------------------------------------------------
libssl is required for an SSL-enabled build.
-------------------------------------------------------------------
    ])
  fi

  if test "x$found_crypto" != "xyes"; then
    AC_MSG_ERROR([cannot find libcrypto
-------------------------------------------------------------------
libcrypto is required for an SSL-enabled build.
-------------------------------------------------------------------
    ])
  fi

  LIBS="-lssl -lcrypto $LIBS"

  AC_DEFINE([USE_SSL_SOCKET], [1])
fi

AM_CONDITIONAL([ENABLE_SSL],
               [test "x$enable_ssl" = "xyes"])

AC_SUBST([SSL_CPPFLAGS])
AC_SUBST([SSL_LINKERFLAGS])


# Check if gflags are present. If so, make sure we link against them
# when building the Python module.
AC_CHECK_LIB([gflags],
             [main],
             [found_gflags=yes])
if test "x$found_gflags" = "xyes"; then
  GFLAGS_LIBS="-lgflags"
else
  GFLAGS_LIBS=""
fi

AC_CHECK_TOOL([PATCHCOMMAND_TEST], [patch], [], [$PATH])
if test -z "`echo $PATCHCOMMAND_TEST`"; then
    AC_MSG_ERROR([patch command not found in PATH])
fi

# Check if we need to do anything special to use the fts_*
# functions. On some systems (e.g., those that use musl instead of
# glibc), FTS support requires linking against an additional library.
AC_CHECK_HEADERS([fts.h], [], [AC_MSG_ERROR([cannot find fts.h
-------------------------------------------------------------------
Failed to find fts.h. If this header file is not provided by libc
on this system (e.g., musl), a separate FTS package must be installed.
-------------------------------------------------------------------
])])

AC_SEARCH_LIBS(fts_close, fts, [], [AC_MSG_ERROR([cannot find libfts
-------------------------------------------------------------------
Failed to find fts_close. If this function is not provided by libc
on this system (e.g., musl), a separate FTS package must be installed.
-------------------------------------------------------------------
])])

###############################################################################
# Miscellaneous checks related to (un)bundled software.
###############################################################################

# Attempt to use preinstalled dependencies instead of the bundled
# versions in cases where the user specified their location
# (--with-XXX=DIR) or asked us to attempt to detect them
# (--disable-bundled).


# On OS X, if `--with-apr` was not provided, we try to use an available
# brew installed version.
if test -z "`echo $with_apr`" &&
   test "$OS_NAME" = "darwin" &&
   test -n "`command -v brew`" &&
   test -n "`brew list --versions apr`"; then
  with_apr=`brew --prefix apr`/libexec
fi

# Check if libapr-1 prefix path was provided, and if so, add it to
# the CPPFLAGS and LDFLAGS with respective /include/apr-1 and /lib path
# suffixes. We include /include/apr-1 because we include <apr*>
# headers directly.
if test -n "`echo $with_apr`" ; then
    CPPFLAGS="-I${with_apr}/include/apr-1 \
              -I${with_apr}/include/apr-1.0 $CPPFLAGS"
    LDFLAGS="-L${with_apr}/lib $LDFLAGS"
else
    CPPFLAGS="-I/usr/include/apr-1 -I/usr/include/apr-1.0	\
      $CPPFLAGS"
fi

AC_CHECK_HEADERS([apr_pools.h],
                 [AC_CHECK_LIB([apr-1], [apr_initialize], [],
                               [AC_MSG_ERROR([cannot find libapr-1
-------------------------------------------------------------------
libapr-1 is required for mesos to build.
-------------------------------------------------------------------
                        ])])], [AC_MSG_ERROR([cannot find libapr-1 headers
-------------------------------------------------------------------
libapr-1 is required for mesos to build.
-------------------------------------------------------------------
])])

if test -n "`echo $with_boost`"; then
  CPPFLAGS="$CPPFLAGS -I${with_boost}/include"
fi

# Check if user has asked us to use a preinstalled Boost, or if they
# asked us to ignore all bundled libraries while compiling and
# linking.
if test "x$without_bundled_boost" = "xyes" || \
   test "x$enable_bundled" != "xyes"; then
  # Check if headers and library were located.
  AC_CHECK_HEADERS([boost/version.hpp], [found_boost=yes])

  if test "x$found_boost" = "xyes"; then
    with_bundled_boost=no
  else
    AC_MSG_ERROR([cannot find Boost
-------------------------------------------------------------------
You have requested the use of a non-bundled Boost but no suitable
Boost could be found.

You may want specify the location of Boost by providing a prefix
path via --with-boost=DIR, or check that the path you provided is
correct if you're already doing this.
-------------------------------------------------------------------
])
  fi
else
  with_bundled_boost=yes
fi

AM_CONDITIONAL([WITH_BUNDLED_BOOST], [test "x$with_bundled_boost" = "xyes"])

if test -n "`echo $with_concurrentqueue`"; then
  CPPFLAGS="-I${with_concurrentqueue} $CPPFLAGS"
fi


# Check if user has asked us to use a preinstalled concurrentqueue,
# or if they asked us to ignore all bundled libraries while compiling
# and linking.
if test "x$without_bundled_concurrentqueue" = "xyes" || \
   test "x$enable_bundled" != "xyes"; then
  # Check if headers and library were located.
  AC_CHECK_HEADERS([concurrentqueue.h], [found_concurrentqueue=yes])

  if test "x$found_concurrentqueue" = "xyes"; then
    with_bundled_concurrentqueue=no
  else
    AC_MSG_ERROR([cannot find concurrentqueue
-------------------------------------------------------------------
You have requested the use of a non-bundled concurrentqueue but no
suitable concurrentqueue could be found.

You may want specify the location of concurrentqueue by providing a
prefix path via --with-concurrentqueue=DIR, or check that the path
you provided is correct if you're already doing this.
-------------------------------------------------------------------
])
  fi
else
  with_bundled_concurrentqueue=yes
fi

AM_CONDITIONAL([WITH_BUNDLED_CONCURRENTQUEUE],
               [test "x$with_bundled_concurrentqueue" = "xyes"])


# Check if Libcurl prefix path was provided, and if so, add it to
# the CPPFLAGS and LDFLAGS with respective /include and /lib path
# suffixes.
if test -n "`echo $with_curl`" ; then
    CPPFLAGS="-I${with_curl}/include $CPPFLAGS"
    LDFLAGS="-L${with_curl}/lib $LDFLAGS"
fi

AC_CHECK_LIB([curl], [curl_global_init], [],
             [AC_MSG_ERROR([cannot find libcurl
-------------------------------------------------------------------
libcurl is required for mesos to build.
-------------------------------------------------------------------
])])


# If the user has asked not to include the bundled ELFIO headers,
# check to see if the path to the ELFIO headers has been specified. If
# it has, make sure that the path is absolute. If everything is in
# order, add this path to the CPPFLAGS.
if test "x$without_bundled_elfio" = "xyes" || \
   test "x$enable_bundled" != "xyes"; then
  if test -n "`echo $with_elfio`"; then
    if test "$with_elfio" = "${with_elfio#/}"; then
      AC_MSG_ERROR([The path passed to --with-elfio must be absolute.])
    fi
     CPPFLAGS="-I${with_elfio} $CPPFLAGS"
  fi

  AC_CHECK_HEADERS([elfio/elfio.hpp], [],
                   [AC_MSG_ERROR([Cannot find the ELFIO headers
-------------------------------------------------------------------
You have requested the use of a non-bundled ELFIO but no suitable
ELFIO headers could be found. Make sure these headers are either
installed on the system or the path passed via --with-elfio is correct.
-------------------------------------------------------------------
  ])])
else
  with_bundled_elfio=yes
fi

AM_CONDITIONAL([WITH_BUNDLED_ELFIO], [test "x$with_bundled_elfio" = "xyes"])


# Check if glog prefix path was supplied and if so, add it to CPPFLAGS
# while extending it by /include and to LDFLAGS while extending it by
# /lib.
if test -n "`echo $with_glog`"; then
  CPPFLAGS="$CPPFLAGS -I${with_glog}/include"
  LDFLAGS="$LDFLAGS -L${with_glog}/lib"
fi

# Check if user has asked us to use a preinstalled glog, or if they
# asked us to ignore all bundled libraries while compiling and
# linking.
if test "x$without_bundled_glog" = "xyes" || \
   test "x$enable_bundled" != "xyes"; then
  # Check if headers and library were located.
  AC_CHECK_HEADERS([glog/logging.h],
                   [AC_LINK_IFELSE(
                        [AC_LANG_PROGRAM([#include <glog/logging.h>], [])],
                        [TEST_LIBS="$TEST_LIBS -lglog"] [found_glog=yes],
                        [AC_MSG_WARN([glog is not installed.])])])

  if test "x$found_glog" = "xyes"; then
    with_bundled_glog=no
  else
    AC_MSG_ERROR([cannot find glog
-------------------------------------------------------------------
You have requested the use of a non-bundled glog but no suitable
glog could be found.

You may want specify the location of glog by providing a prefix
path via --with-glog=DIR, or check that the path you provided is
correct if you're already doing this.
-------------------------------------------------------------------
])
  fi
else
  with_bundled_glog=yes
fi

AM_CONDITIONAL([WITH_BUNDLED_GLOG],
               [test "x$with_bundled_glog" = "xyes"])


GMOCKSRC="gmock-all.cc"
GTESTSRC="gtest-all.cc"

if test -n "`echo $with_gmock`"; then
  CPPFLAGS="$CPPFLAGS -I${with_gmock} -I${with_gmock}/include \
    -I${with_gmock}/src -I${with_gmock}/gtest \
    -I${with_gmock}/gtest/include -I${with_gmock}/gtest/src"

elif test "x$enable_bundled" != "xyes"; then
  CPPFLAGS="$CPPFLAGS -I/usr/src/gmock"
  # On system installations the deps are handled separately.
  # So insert a placeholder for substitution.
  GTESTSRC="stdio.h"
  check_gtest=yes
fi

# Check if user has asked us to use a preinstalled gmock, or if they
# asked us to ignore all bundled libraries while compiling and
# linking.
if test "x$without_bundled_gmock" = "xyes" || \
   test "x$enable_bundled" != "xyes"; then
  # Check if headers and library were located.
  AC_CHECK_HEADER($GMOCKSRC,
                  [found_gmock=yes],
                  [])

  if test "x$found_gmock" = "xyes"; then
    if test "x$check_gtest" = "xyes"; then
      # TODO(jieyu): Symbol _init is not availble for libs on OS X.
      AC_CHECK_LIB([gtest],
                   [_init],
                   [],
                   [AC_MSG_ERROR([gtest missing])])
    fi
    with_bundled_gmock=no
  else
    AC_MSG_ERROR([cannot find gmock
-------------------------------------------------------------------
You have requested the use of a non-bundled gmock but no suitable
gmock could be found.

You may want specify the location of gmock by providing a prefix
path via --with-gmock=DIR, or check that the path you provided is
correct if you're already doing this.
-------------------------------------------------------------------
])
  fi
else
  with_bundled_gmock=yes
fi

AM_CONDITIONAL([WITH_BUNDLED_GMOCK], [test "x$with_bundled_gmock" = "xyes"])
AM_CONDITIONAL([LINKGTEST], [test "check_gtest" = "xyes"])
AC_SUBST([GMOCKSRC])
AC_SUBST([GTESTSRC])


if test -n "`echo $with_http_parser`"; then
  CPPFLAGS="$CPPFLAGS -I${with_http_parser}/include"
  LDFLAGS="$LDFLAGS -L${with_http_parser}/lib"
fi

# Check if user has asked us to use a preinstalled http-parser, or if
# they asked us to ignore all bundled libraries while compiling and
# linking.
if test "x$without_bundled_http_parser" = "xyes" || \
   test "x$enable_bundled" != "xyes"; then
  # Check if headers and library were located.
  AC_CHECK_HEADERS([http_parser.h],
                   [AC_CHECK_LIB([http_parser],
                                 [http_parser_init],
                                 [found_http_parser=yes])])

  if test "x$found_http_parser" = "xyes"; then
    with_bundled_http_parser=no
  else
    AC_MSG_ERROR([cannot find http-parser
-------------------------------------------------------------------
You have requested the use of a non-bundled http-parser but no suitable
http-parser could be found.

You may want specify the location of http-parser by providing a prefix
path via --with-http-parser=DIR, or check that the path you provided is
correct if you're already doing this.
-------------------------------------------------------------------
])
  fi
else
  with_bundled_http_parser=yes
fi

AM_CONDITIONAL([WITH_BUNDLED_HTTP_PARSER],
               [test "x$with_bundled_http_parser" = "xyes"])


# NB: Since malloc() and free() are declared in stdlib.h, and the
# jemalloc-specific api is detected at runtime, we dont need any
# jemalloc headers or modify CPPFLAGS here.
if test "x$enable_jemalloc_allocator" = "xyes"; then
  if test "x$OS_NAME" != "xlinux"; then
    # Building and linking against jemalloc is actually not a problem
    # on OSX even now, but to make it actually *useful* we would have
    # have to re-route all existing malloc/free calls through jemalloc.
    # The recommended way of doing that on Mac seems to be call
    # `malloc_default_zone()` and to overwrite the hooks in the
    # returned `malloc_zone_t`.
    AC_MSG_ERROR([
--------------------------------------------------------------------
You have requested the default memory allocator to be replaced by
jemalloc, but your operating system was detected to be $OS_NAME.

This feature is currently only supported on linux.
--------------------------------------------------------------------
    ])
  fi

  if test -n "`echo $with_jemalloc`"; then
    WITH_JEMALLOC="${with_jemalloc}"
  fi

  # Check if the user asked us to use a preinstalled jemalloc, or if
  # they asked us to ignore all bundled libraries while compiling and
  # linking.
  if test "x$without_bundled_jemalloc" = "xyes" || \
     test "x$enable_bundled" != "xyes"; then

    # Avoid AC_SEARCH_LIBS here because it would implicitly add jemalloc
    # to LIBS, but we dont want to link it against libmesos, only the
    # individual binaries
    AC_CHECK_LIB([jemalloc], [malloc_stats_print], [found_jemalloc=yes])
    if test "x$found_jemalloc" = "xyes"; then
      with_bundled_jemalloc=no
    else
      AC_MSG_ERROR([cannot find jemalloc
-------------------------------------------------------------------
You have requested the use of a non-bundled jemalloc but no suitable
library could be found.

You may want specify the location of jemalloc by providing a prefix
path via --with-jemalloc=DIR, or check that the path you provided is
correct if you're already doing this.
-------------------------------------------------------------------
  ])
    fi
  else
    with_bundled_jemalloc=yes
  fi
fi

AC_SUBST(WITH_JEMALLOC)
AM_CONDITIONAL([WITH_BUNDLED_JEMALLOC],
         [test "x$with_bundled_jemalloc" = "xyes"])

# TODO(benh): Consider using AS_IF instead of just shell 'if'
# statements for better autoconf style (the AS_IF macros also make
# sure variable dependencies are handled appropriately).

# A helper for checking whether we can compile and link using JNI with
# the current JAVA_CPPFLAGS (and JAVA_TEST_LDFLAGS which we setup
# ourselves below).
# TRY_LINK_JNI([ACTION-SUCCESS], [ACTION-FAILURE])
AC_DEFUN([TRY_LINK_JNI], [
  cat <<__EOF__ >conftest.cpp [
#include <jni.h>
int main(int argc, char** argv)
{
  JNIEnv* env;
  JavaVM* jvm;
  JavaVMInitArgs vmArgs;
  return JNI_CreateJavaVM(&jvm, (void**) &env, &vmArgs);
}]
__EOF__

  # Try to compile and link via libtool (the one we generate).
  ./libtool --tag=CXX --mode=link $CXX -Wall -Werror $JAVA_CPPFLAGS \
    -o conftest conftest.cpp $JAVA_TEST_LDFLAGS >&5

  if test $? != 0; then
    rm -f conftest # Cleanup after ourselves.
    $2 # Expand failure action.
  else
    rm -f conftest # Cleanup after ourselves.
    $1 # Expand success action.
  fi
])


# Perform necessary configuration for building with Java.
if test "x$enable_java" = "xyes"; then
  # First let's try and determine JAVA_HOME if it hasn't been set. We
  # do this by checking to see if the directory found at the
  # 'java.home' system property for the java found on the path
  # includes javac. If so, then we'll guess that this is a JDK
  # installation rather than a JRE installation.
  if test -z "$JAVA_HOME"; then
    AC_PATH_PROG([JAVAC], [javac], [$JAVAC])
    AC_PATH_PROG([JAVA], [java], [$JAVA])

    if test "x$JAVA" = "x" || test "x$JAVAC" = "x"; then
      AC_MSG_ERROR([cannot guess JAVA_HOME (no 'java' or 'javac' found)])
    fi

    AC_MSG_CHECKING([value of Java system property 'java.home'])
    cat <<__EOF__ >conftest.java [
public class conftest {
  public static void main(String[] args) {
    System.out.print(System.getProperty("java.home"));
  }
}]
__EOF__

    # Now build and run the code.
    $JAVAC conftest.java && JAVA_DOT_HOME="`$JAVA -cp . conftest`"

    if test $? = 0 && test ! -z "$JAVA_DOT_HOME"; then
      AC_MSG_RESULT($JAVA_DOT_HOME)
    else
      JAVA_DOT_HOME=""
      AC_MSG_RESULT([not found])
    fi

    # Clean up after ourselves.
    rm -f conftest.java conftest.class

    # Check if 'java.home' looks like a JDK installation, or if
    # 'java.home'/.. looks like a JDK installation (e.g., Linux).
    if test -f $JAVA_DOT_HOME/bin/javac; then
      JAVA_HOME=$JAVA_DOT_HOME
    elif test -f `dirname $JAVA_DOT_HOME`/bin/javac; then
      JAVA_HOME=`dirname $JAVA_DOT_HOME`
    fi

    if test -z "$JAVA_HOME"; then
      AC_MSG_ERROR([could not guess JAVA_HOME])
    else
      AC_MSG_NOTICE([using JAVA_HOME=$JAVA_HOME])
    fi
  fi

  # Determine linker flags for Java if not set.
  if test "$OS_NAME" = "darwin"; then
    dir="$JAVA_HOME/jre/lib/server"
    for dir in "$JAVA_HOME/lib/server" "$JAVA_HOME/jre/lib/server"; do
      if test -e "$dir"; then
	# Note that these are libtool specific flags.
        JAVA_TEST_LDFLAGS="-framework JavaVM"
        JAVA_JVM_LIBRARY=$dir/libjvm.dylib
	break;
      fi
    done
  elif test "$OS_NAME" = "linux"; then
    for dir in \
      "$JAVA_HOME/lib/server" \
      "$JAVA_HOME/jre/lib/"{amd64,i386,arm,aarch64,ppc64,ppc64le,s390,s390x}"/server"
    do
      if test -e "$dir"; then
	# Note that these are libtool specific flags.
	JAVA_TEST_LDFLAGS="-L$dir -R$dir -Wl,-ljvm"
	JAVA_JVM_LIBRARY=$dir/libjvm.so
	break;
      fi
    done
  elif test "$OS_NAME" = "freebsd"; then
    for arch in amd64 i386 arm; do
      dir="$JAVA_HOME/jre/lib/$arch/server"
      if test -e "$dir"; then
	# Note that these are libtool specific flags.
	JAVA_TEST_LDFLAGS="-L$dir -R$dir -pthread -Wl,-ljvm"
	JAVA_JVM_LIBRARY=$dir/libjvm.so
	break;
      fi
    done
  fi

  if test -z "$JAVA_TEST_LDFLAGS"; then
    AC_MSG_ERROR([failed to determine linker flags for using Java \
(bad JAVA_HOME or missing support for your architecture?)])
  fi

  # Now try and build with JNI, looping through possible compiler
  # flags as necessary (provided JAVA_CPPFLAGS was not set).
  AC_MSG_CHECKING([whether or not we can build with JNI])
  if test -z "$JAVA_CPPFLAGS"; then
    if test "$OS_NAME" = "darwin"; then
      while true; do # Loop until sucessful (via break) or exhausted options.
        m4_foreach([java_cppflags],
                   [["-I$JAVA_HOME/include -I$JAVA_HOME/include/$OS_NAME"],
                    ["-I/System/Library/Frameworks/JavaVM.framework/Headers"]],
                   [JAVA_CPPFLAGS=java_cppflags
                    TRY_LINK_JNI([break])])
        # Exhausted options.
        AC_MSG_ERROR([failed to build with JNI
  -------------------------------------------------------------------
  It appears we were unable to compile against the JNI. This is most
  likely due to one of the following issues:
    1. You do not have a JDK installed on your system.
    2. All JDKs installed on your system have deprecated JNI headers.
  It is advised to install OpenJDK on your system, as the JDK that
  ships with OS X has deprecated JNI headers.
  -------------------------------------------------------------------
        ])
      done
    else
      while true; do # Loop until sucessful (via break) or exhausted options.
        m4_foreach([java_cppflags],
                   [["-I$JAVA_HOME/include -I$JAVA_HOME/include/$OS_NAME"]],
                   [JAVA_CPPFLAGS=java_cppflags
                    TRY_LINK_JNI([break])])
        # Exhausted options.
        AC_MSG_ERROR([failed to build with JNI])
      done
    fi
  else
    TRY_LINK_JNI([], [AC_MSG_ERROR([failed to build with JNI])])
  fi
  AC_MSG_RESULT([yes])

  AC_CONFIG_FILES([src/examples/java/test-exception-framework],
                  [chmod +x src/examples/java/test-exception-framework])
  AC_CONFIG_FILES([src/examples/java/test-executor],
                  [chmod +x src/examples/java/test-executor])
  AC_CONFIG_FILES([src/examples/java/test-framework],
                  [chmod +x src/examples/java/test-framework])
  AC_CONFIG_FILES([src/examples/java/test-multiple-executors-framework],
                  [chmod +x src/examples/java/test-multiple-executors-framework])
  AC_CONFIG_FILES([src/examples/java/test-log],
                  [chmod +x src/examples/java/test-log])
  AC_CONFIG_FILES([src/examples/java/v1-test-framework],
                  [chmod +x src/examples/java/v1-test-framework])
  AC_CONFIG_FILES([src/java/mesos.pom])

  AC_DEFINE([MESOS_HAS_JAVA])

  has_java=yes

  # Test for Maven so we can build the jar.
  if test -z "$MAVEN_HOME"; then
    AC_PATH_PROG([MVN], [mvn], [$MVN])

    if test "x$MVN" = "x"; then
      AC_MSG_ERROR([cannot find mvn on your path
-------------------------------------------------------------------
You may want verify that Maven has been correctly installed and
added to your PATH or set the MAVEN_HOME environment variable
to point to your Maven installation.
-------------------------------------------------------------------
])
    fi
  else
    AC_PATH_PROGS([MVN], [mvn], [$MVN], [$MAVEN_HOME/bin])

    if test "x$MVN" = "x"; then
      AC_MSG_ERROR([cannot find mvn inside $MAVEN_HOME/bin
-------------------------------------------------------------------
You may want verify the MAVEN_HOME validity or check that Maven
has been correctly installed.
-------------------------------------------------------------------
])
    fi
  fi
fi

AM_CONDITIONAL([HAS_JAVA], [test "x$has_java" = "xyes"])
AC_PATH_PROG([JAVAH], [javah], [$JAVAH], [$JAVA_HOME/bin])
AM_CONDITIONAL([HAS_JAVAH], [test "x$JAVAH" != "x"])


# Check if LevelDB prefix path was supplied and if so, add it to
# CPPFLAGS while extending it by /include and to LDFLAGS while
# extending it by /lib.
if test -n "`echo $with_leveldb`"; then
  CPPFLAGS="$CPPFLAGS -I${with_leveldb}/include"
  LDFLAGS="$LDFLAGS -L${with_leveldb}/lib"
fi

# Check if user has asked us to use a preinstalled LevelDB, or if they
# asked us to ignore all bundled libraries while compiling and
# linking.
if test "x$without_bundled_leveldb" = "xyes" || \
   test "x$enable_bundled" != "xyes"; then
  # Check if headers and library were located.
  AC_CHECK_HEADERS([leveldb/db.h],
                   [AC_CHECK_LIB([leveldb],
                                 [leveldb_open],
                                 [found_leveldb=yes])])

  if test "x$found_leveldb" = "xyes"; then
    with_bundled_leveldb=no
  else
    AC_MSG_ERROR([cannot find LevelDB
-------------------------------------------------------------------
You have requested the use of a non-bundled LevelDB but no suitable
LevelDB could be found.

You may want specify the location of LevelDB by providing a prefix
path via --with-leveldb=DIR, or check that the path you provided is
correct if you're already doing this.
-------------------------------------------------------------------
])
  fi
else
  with_bundled_leveldb=yes
fi


AM_CONDITIONAL([WITH_BUNDLED_LEVELDB],
               [test "x$with_bundled_leveldb" = "xyes"])


# Check if we should use containerizer launcher sealing.
AC_MSG_CHECKING([whether to enable launcher sealing])
AC_MSG_RESULT([$enable_launcher_sealing])

AS_IF([test "x$enable_launcher_sealing" = "xyes"], [
  # We only support launcher sealing on Linux.
  AS_IF([test "$OS_NAME" = "linux"],
        [],
        [AC_MSG_ERROR([no launcher sealing support on $OS_NAME
-------------------------------------------------------------------
Launcher sealing is only supported on Linux.
-------------------------------------------------------------------
  ])])

  AC_DEFINE([ENABLE_LAUNCHER_SEALING])
])

AM_CONDITIONAL([ENABLE_LAUNCHER_SEALING], [test "x$enable_launcher_sealing" = "xyes"])


# Check if libevent prefix path was provided, and if so, add it to
# the CPPFLAGS and LDFLAGS with respective /include and /lib path
# suffixes.
if test -z "`echo $with_libevent`" &&
   test "$OS_NAME" = "darwin" &&
   test -n "`command -v brew`" &&
   test -n "`brew list --versions libevent`"; then
  with_libevent=`brew --prefix libevent`
fi

if test -n "`echo $with_libevent`"; then
  CPPFLAGS="-I${with_libevent}/include $CPPFLAGS"
  LDFLAGS="-L${with_libevent}/lib $LDFLAGS"
fi

if test "x$enable_libevent" = "xyes"; then
  # Check if user has asked us to use a preinstalled libevent, or if they
  # asked us to ignore all bundled libraries while compiling and
  # linking.
  if test "x$without_bundled_libevent" = "xyes" || \
     test "x$enable_bundled" != "xyes"; then
     MESOS_HAVE_LIBEVENT()
  else
    with_bundled_libevent=yes
  fi
fi

if test "x$enable_ssl" = "xyes"; then
  if test "x$enable_libevent" = "xyes"; then
    if test "x$with_bundled_libevent" != "xyes"; then
      MESOS_HAVE_LIBEVENT_SSL()
    fi
  fi
fi

AM_CONDITIONAL([ENABLE_LIBEVENT],
               [test "x$enable_libevent" = "xyes"])
AM_CONDITIONAL([WITH_BUNDLED_LIBEVENT],
               [test "x$with_bundled_libevent" = "xyes"])


# Check if user has asked us to use a preinstalled libarchive, or if
# they asked us to ignore all bundled libraries while compiling and
# linking.
if test -n "`echo $with_libarchive`"; then
  CPPFLAGS="$CPPFLAGS -I${with_libarchive}/include"
  LDFLAGS="$LDFLAGS -L${with_libarchive}/lib"
fi

if test "x$without_bundled_libarchive" = "xyes" || \
   test "x$enable_bundled" != "xyes"; then
  # Check if headers and library were located.
  AC_CHECK_HEADERS([archive.h], [found_libarchive=yes])

  if test "x$found_libarchive" = "xyes"; then
    with_bundled_libarchive=no
  else
    AC_MSG_ERROR([cannot find libarchive
-------------------------------------------------------------------
You have requested the use of a non-bundled libarchive but no suitable
libarchive could be found.

You may want specify the location of libarchive by providing a prefix
path via --with-libarchive=DIR, or check that the path you provided is
correct if you're already doing this.
-------------------------------------------------------------------
])
  fi

else
  with_bundled_libarchive=yes
fi

AM_CONDITIONAL([WITH_BUNDLED_LIBARCHIVE], [test "x$with_bundled_libarchive" = "xyes"])


# Check if user has asked us to use a preinstalled libprocess, or if
# they asked us to ignore all bundled libraries while compiling and
# linking.
if test "x$without_bundled_libprocess" = "xyes" || \
   test "x$enable_bundled" != "xyes"; then
  # Check if headers and library were located.
  AC_CHECK_HEADERS([process/process.hpp], [found_libprocess=yes])

  if test "x$found_libprocess" = "xyes"; then
    with_bundled_libprocess=no
  else
    AC_MSG_ERROR([cannot find libprocess
-------------------------------------------------------------------
You have requested the use of a non-bundled libprocess but no suitable
libprocess could be found.

You may want specify the location of libprocess by providing a prefix
path via --with-libprocess=DIR, or check that the path you provided is
correct if you're already doing this.
-------------------------------------------------------------------
])
  fi

  # TODO(kapil): Check for linkability of libprocess.so.
else
  with_bundled_libprocess=yes
fi

AM_CONDITIONAL([WITH_BUNDLED_LIBPROCESS], [test "x$with_bundled_libprocess" = "xyes"])


AC_MSG_CHECKING([whether to enable the Seccomp isolator])
AS_IF([test "x$enable_seccomp_isolator" = "xyes"],
      [AC_MSG_RESULT([yes])],
      [AC_MSG_RESULT([no])])

AS_IF([test "x$enable_seccomp_isolator" = "xyes"], [
  # We only support Seccomp on Linux.
  AS_IF([test "$OS_NAME" = "linux"],
        [],
        [AC_MSG_ERROR([no Seccomp support on $OS_NAME
-------------------------------------------------------------------
The Seccomp isolator is only supported on Linux.
-------------------------------------------------------------------
  ])])

  # Check if libseccomp prefix path was supplied and if so, add it to
  # CPPFLAGS while extending it by /include and to LDFLAGS while
  # extending it by /lib.
  if test -n "`echo $with_libseccomp`"; then
    CPPFLAGS="$CPPFLAGS -I${with_libseccomp}/include"
    LDFLAGS="$LDFLAGS -L${with_libseccomp}/lib"
  fi

  # Check if user has asked us to use a bundled libseccomp, or if
  # they asked us to ignore all bundled libraries while compiling and
  # linking.
  if test "x$without_bundled_libseccomp" = "xyes" || \
     test "x$enable_bundled" != "xyes"; then
    # Check if headers and library were located.
    AC_CHECK_HEADERS([seccomp.h], [found_libseccomp=yes])

    if test "x$found_libseccomp" = "xyes"; then
      with_bundled_libseccomp=no
    else
      AC_MSG_ERROR([cannot find libseccomp
-------------------------------------------------------------------
You have requested the use of a non-bundled libseccomp but no suitable
libseccomp could be found.

You may want specify the location of libseccomp by providing a prefix
path via --with-libseccomp=DIR, or check that the path you provided is
correct if you're already doing this.
-------------------------------------------------------------------
])
    fi
  else
    with_bundled_libseccomp=yes
  fi

  AC_DEFINE([ENABLE_SECCOMP_ISOLATOR])
])

AM_CONDITIONAL([ENABLE_SECCOMP_ISOLATOR], [test "x$enable_seccomp_isolator" = "xyes"])
AM_CONDITIONAL([WITH_BUNDLED_LIBSECCOMP], [test "x$with_bundled_libseccomp" = "xyes"])


# Perform necessary configuration for port mapping isolator.
if test "x$enable_port_mapping_isolator" = "xyes"; then
  # Check for OS support.
  AS_IF([test "$OS_NAME" = "linux"],
        [],
        [AC_MSG_ERROR([cannot build port mapping isolator
-------------------------------------------------------------------
The port mapping isolator is only supported on Linux
-------------------------------------------------------------------
  ])])

  MESOS_HAVE_LIBNL3([], [
    MESOS_MSG_LIBNL3_ERROR([libnl-3 is required for building the port mapping isolator])
  ])

  AC_DEFINE([ENABLE_PORT_MAPPING_ISOLATOR])
fi

AM_CONDITIONAL([ENABLE_PORT_MAPPING_ISOLATOR],
               [test "x$enable_port_mapping_isolator" = "xyes"])


if test "x$enable_network_ports_isolator" = "xyes"; then

  AS_IF([test "$OS_NAME" = "linux"],
        [],
        [AC_MSG_ERROR([cannot build network/ports isolator
-------------------------------------------------------------------
The network/ports isolator is only supported on Linux
-------------------------------------------------------------------
  ])])

  MESOS_HAVE_LIBNL3([], [
    MESOS_MSG_LIBNL3_ERROR([libnl-3 is required for building the network/ports isolator])
  ])

  AC_DEFINE([ENABLE_NETWORK_PORTS_ISOLATOR])
fi

AM_CONDITIONAL([ENABLE_NETWORK_PORTS_ISOLATOR],
               [test "x$enable_network_ports_isolator" = "xyes"])


AM_CONDITIONAL(
  [ENABLE_LINUX_ROUTING],
  [test "x$enable_network_ports_isolator" = "xyes" -o "x$enable_port_mapping_isolator" = "xyes"])


# If the user has asked not to include the bundled NVML headers for
# Nvidia GPU support, check to see if the path to the NVML headers has
# been specified.  If it has, make sure that the path is absolute. If
# everything is in order, add this path to the CPPFLAGS.
if test "x$without_bundled_nvml" = "xyes" || \
   test "x$enable_bundled" != "xyes"; then
  if test -n "`echo $with_nvml`"; then
    if test "$with_nvml" = "${with_nvml#/}"; then
      AC_MSG_ERROR([The path passed to --with-nvml must be absolute.])
    fi
     CPPFLAGS="-I${with_nvml} $CPPFLAGS"
  fi

  AC_CHECK_HEADERS([nvidia/gdk/nvml.h], [],
                   [AC_MSG_ERROR([Cannot find the Nvidia NVML headers
-------------------------------------------------------------------
You have requested the use of a non-bundled NVML but no suitable
NVML headers could be found. Make sure these headers are either
installed on the system or the path passed via --with-nvml is correct.
-------------------------------------------------------------------
  ])])
else
  with_bundled_nvml=yes
fi

AM_CONDITIONAL([WITH_BUNDLED_NVML],
  [test "x$with_bundled_nvml" = "xyes"])


# Check if protobuf prefix path was supplied and if so, add it to
# CPPFLAGS while extending it by /include and to LDFLAGS while
# extending it by /lib.
if test -n "`echo $with_protobuf`"; then
  PROTOBUF_CPPFLAGS="-I${with_protobuf}/include"
  PROTOBUF_LINKERFLAGS="-L${with_protobuf}/lib"

  CPPFLAGS="$PROTOBUF_CPPFLAGS $CPPFLAGS"
  LDFLAGS="$PROTOBUF_LINKERFLAGS $LDFLAGS"
fi

# Check if user has asked us to use a preinstalled protobuf, or if
# they asked us to ignore all bundled libraries while compiling and
# linking.
if test "x$without_bundled_protobuf" = "xyes" || \
   test "x$enable_bundled" != "xyes"; then
  # Use the system default prefix if not specified.
  if test -n "`echo $with_protobuf`"; then
    PROTOBUFPREFIX="${with_protobuf}"
  elif test "x$enable_bundled" != "xyes"; then
    PROTOBUFPREFIX="/usr"
  fi

  # Check for protobuf.

  AC_CHECK_HEADERS([google/protobuf/map.h], [found_protobuf=yes],
                   [found_protobuf=no])

  if test "x$found_protobuf" = "xyes"; then
    saved_LIBS="$LIBS"
    LIBS="-lprotobuf $LIBS"
    AC_LANG_PUSH([C++])
    AC_LINK_IFELSE([AC_LANG_PROGRAM([#include <google/protobuf/stubs/common.h>],
                    [ google::protobuf::ShutdownProtobufLibrary(); ])],
            [ found_protobuf=yes ],
            [ found_protobuf=no ])
    AC_LANG_POP()
    LIBS="$saved_LIBS"
  fi

  if test "x$found_protobuf" = "xyes"; then
    with_bundled_protobuf=no
    if test -z "`echo $PROTOBUF_JAR`"; then
      PROTOBUF_JAR="${PROTOBUFPREFIX}/share/java/protobuf.jar"
    fi

    if test "x$enable_java" = "xyes"; then
      AC_CHECK_FILE([$PROTOBUF_JAR],
                    [],
                    [AC_MSG_ERROR([cannot find PROTOBUF_JAR=$PROTOBUF_JAR])])
    fi

    AC_CHECK_FILE([$PROTOBUFPREFIX/include/google/protobuf/wrappers.proto],
                  [],
                  [AC_MSG_ERROR([cannot find protobuf proto files])])

    PROTOBUF_PROTOCFLAGS="-I${PROTOBUFPREFIX}/include"

    AC_CHECK_TOOL([PROTOCOMPILER_TEST], [protoc], [], [$PROTOBUFPREFIX/bin])

    if test -z "`echo $PROTOCOMPILER_TEST`"; then
      AC_MSG_ERROR([protoc not found in PATH])
    fi

    PROTOCOMPILER="$PROTOBUFPREFIX/bin/protoc"

    if test "x$enable_python" = "xyes"; then
      AC_PYTHON_MODULE([google.protobuf], [yes])
    fi
  else
    AC_MSG_ERROR([cannot find protobuf
-------------------------------------------------------------------
You have requested the use of a non-bundled protobuf but no suitable
protobuf (version 3+) could be found.

You may want specify the location of protobuf by providing a prefix
path via --with-protobuf=DIR, or check that the path you provided is
correct if youre already doing this.
-------------------------------------------------------------------
])
  fi
else
  with_bundled_protobuf=yes
  PROTOBUF_JAR="\${MESOS_BUILD_DIR}/src/java/target/protobuf-java-3.5.0.jar"
fi

AM_CONDITIONAL([WITH_BUNDLED_PROTOBUF],
               [test "x$with_bundled_protobuf" = "xyes"])

AC_SUBST([PROTOBUF_CPPFLAGS])
AC_SUBST([PROTOBUF_JAR])
AC_SUBST([PROTOBUF_LINKERFLAGS])
AC_SUBST([PROTOBUF_PROTOCFLAGS])
AC_SUBST([PROTOCOMPILER])


if test -n "`echo $with_libev`"; then
  CPPFLAGS="$CPPFLAGS -I${with_libev}/include"
  LDFLAGS="$LDFLAGS -L${with_libev}/lib"
fi

# Check if user has asked us to use a preinstalled libev, or if they
# asked us to ignore all bundled libraries while compiling and
# linking.
if test "x$without_bundled_libev" = "xyes" || \
   test "x$enable_bundled" != "xyes"; then
  # Check if headers and library were located.
  AC_CHECK_HEADERS([ev.h], [found_libev=yes])

  if test "x$found_libev" = "xyes"; then
    with_bundled_libev=no
    AC_MSG_WARN([libev found must have been built with -DEV_CHILD_ENABLE=0])
  else
    AC_MSG_ERROR([cannot find libev
-------------------------------------------------------------------
You have requested the use of a non-bundled libev but no suitable
libev could be found.

You may want specify the location of libev by providing a prefix
path via --with-libev=DIR, or check that the path you provided is
correct if you're already doing this.
-------------------------------------------------------------------
])
  fi
else
  with_bundled_libev=yes
fi

AM_CONDITIONAL([WITH_BUNDLED_LIBEV], [test "x$with_bundled_libev" = "xyes"])


if test -n "`echo $with_picojson`"; then
  CPPFLAGS="$CPPFLAGS -I${with_picojson}/include"
fi

# Check if user has asked us to use a preinstalled picojson, or if
# they asked us to ignore all bundled libraries while compiling and
# linking.
if test "x$without_bundled_picojson" = "xyes" || \
   test "x$enable_bundled" != "xyes"; then
  # Check if headers and library were located.
  AC_CHECK_HEADERS([picojson.h], [found_picojson=yes])

  if test "x$found_picojson" = "xyes"; then
    with_bundled_picojson=no
  else
    AC_MSG_ERROR([cannot find picojson
-------------------------------------------------------------------
You have requested the use of a non-bundled picojson but no suitable
picojson could be found.

You may want specify the location of picojson by providing a prefix
path via --with-picojson=DIR, or check that the path you provided is
correct if you're already doing this.
-------------------------------------------------------------------
])
  fi
else
  with_bundled_picojson=yes
fi

AM_CONDITIONAL([WITH_BUNDLED_PICOJSON], [test "x$with_bundled_picojson" = "xyes"])


if test -n "`echo $with_rapidjson`"; then
  CPPFLAGS="$CPPFLAGS -I${with_rapidjson}/include"
fi

# Check if user has asked us to use a preinstalled rapidjson, or if
# they asked us to ignore all bundled libraries while compiling and
# linking.
if test "x$without_bundled_rapidjson" = "xyes" || \
   test "x$enable_bundled" != "xyes"; then
  # Check if headers and library were located.
  AC_CHECK_HEADERS([rapidjson/rapidjson.h], [found_rapidjson=yes])

  if test "x$found_rapidjson" = "xyes"; then
    with_bundled_rapidjson=no
  else
    AC_MSG_ERROR([cannot find rapidjson
-------------------------------------------------------------------
You have requested the use of a non-bundled rapidjson but no suitable
rapidjson could be found.

You may want specify the location of rapidjson by providing a prefix
path via --with-rapidjson=DIR, or check that the path you provided is
correct if you're already doing this.
-------------------------------------------------------------------
])
  fi
else
  with_bundled_rapidjson=yes
fi

AM_CONDITIONAL([WITH_BUNDLED_RAPIDJSON], [test "x$with_bundled_rapidjson" = "xyes"])


# Check if Sasl2 prefix path was provided, and if so, add it to
# the CPPFLAGS and LDFLAGS with respective /include and /lib path
# suffixes.
if test -n "`echo $with_sasl`" ; then
  CPPFLAGS="-I${with_sasl}/include $CPPFLAGS"
  LDFLAGS="-L${with_sasl}/lib $LDFLAGS"
fi

AC_CHECK_LIB([sasl2], [sasl_done], [],
             [AC_MSG_ERROR([cannot find libsasl2
-------------------------------------------------------------------
We need libsasl2 for authentication!
-------------------------------------------------------------------
])])

AC_MSG_CHECKING([SASL CRAM-MD5 support])

# Render a test binary for checking if the SASL client supports CRAM-MD5.
cat <<__EOF__ >crammd5_installed.c [
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <sasl/sasl.h>
#include <sasl/saslplug.h>
#include <sasl/saslutil.h>

// We need to disable the deprecation warnings as Apple has decided
// to deprecate all of CyrusSASL's functions with OS 10.11
// (see MESOS-3030). We are using GCC pragmas also for covering clang.
#ifdef __APPLE__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif

static void search_crammd5(
  client_sasl_mechanism_t *m,
  sasl_info_callback_stage_t stage,
  void *rock)
{
  if (stage == SASL_INFO_LIST_START || stage == SASL_INFO_LIST_END) {
    return;
  }
  if (m->plug != NULL) {
    if (!strcmp("CRAM-MD5", m->plug->mech_name)) {
      printf("found");
    }
  }
}

int main(int argc, char** argv)
{
  int result = sasl_client_init(NULL);
  if (result != SASL_OK) {
    return 1;
  }
  sasl_client_plugin_info(NULL, &search_crammd5, NULL);
  return 0;
}]

#ifdef __APPLE__
#pragma GCC diagnostic pop
#endif
__EOF__

# Build the SASL client check test binary. Prevent the use of possibly
# activated debug symbols in executables (cc -g) as those cause a
# distcheck failure on OSX by leaking build artefacts (.dsym).
AS_IF([test "x${ac_cv_env_CFLAGS_set}" = "x"],
      [SASL_TEST_CFLAGS=""], [SASL_TEST_CFLAGS=$CFLAGS])
$CC crammd5_installed.c $CPPFLAGS $SASL_TEST_CFLAGS $LDFLAGS -lsasl2 \
  -o crammd5_installed 2>&1 >/dev/null

# Run the test binary and get its output.
crammd5test=`./crammd5_installed 2>&1`

# Clean up all leftovers from this test.
rm -f crammd5_installed.c crammd5_installed

AS_IF([test "x$crammd5test" = "xfound"],
      [AC_MSG_RESULT([yes])],
      [AC_MSG_ERROR([no
-------------------------------------------------------------------
We need CRAM-MD5 support for SASL authentication.
-------------------------------------------------------------------])])


# Check if user has asked us to use a preinstalled stout, or if
# they asked us to ignore all bundled libraries while compiling and
# linking.
if test "x$without_bundled_stout" = "xyes" || \
   test "x$enable_bundled" != "xyes"; then
  # Check if headers and library were located.
  AC_CHECK_HEADERS([stout/try.hpp], [found_stout=yes])

  if test "x$found_stout" = "xyes"; then
    with_bundled_stout=no
  else
    AC_MSG_ERROR([cannot find stout
-------------------------------------------------------------------
You have requested the use of a non-bundled stout but no suitable
stout could be found.

You may want specify the location of stout by providing a prefix
path via --with-stout=DIR, or check that the path you provided is
correct if you're already doing this.
-------------------------------------------------------------------
])
  fi
else
  with_bundled_stout=yes
fi

AM_CONDITIONAL([WITH_BUNDLED_STOUT], [test "x$with_bundled_stout" = "xyes"])


# Check if libsvn-1 prefix path was provided, and if so, add it to
# the CPPFLAGS and LDFLAGS with respective /include and /lib path
# suffixes. We include /include/subversion-1 because we include
# <svn_*> directly.
if test -z "`echo $with_svn`" &&
   test "$OS_NAME" = "darwin" &&
   test -n "`command -v brew`" &&
   test -n "`brew list --versions subversion`"; then
  with_svn=`brew --prefix subversion`
fi

if test -n "`echo $with_svn`"; then
  CPPFLAGS="-I${with_svn}/include/subversion-1 $CPPFLAGS"
  LDFLAGS="-L${with_svn}/lib $LDFLAGS"
else
  CPPFLAGS="-I/usr/include/subversion-1 $CPPFLAGS"
fi

AC_CHECK_HEADERS([svn_version.h],
                 [AC_CHECK_LIB([svn_subr-1], [svn_stringbuf_create_ensure], [],
                               [AC_MSG_ERROR([cannot find libsvn_subr-1
-------------------------------------------------------------------
libsubversion-1 is required for mesos to build.
-------------------------------------------------------------------
                        ])])], [AC_MSG_ERROR([cannot find libsvn_subr-1 headers
-------------------------------------------------------------------
libsubversion-1 is required for mesos to build.
-------------------------------------------------------------------
])])

AC_CHECK_HEADERS([svn_delta.h],
                 [AC_CHECK_LIB([svn_delta-1], [svn_txdelta], [],
                               [AC_MSG_ERROR([cannot find libsvn_delta-1
-------------------------------------------------------------------
libsubversion-1 is required for mesos to build.
-------------------------------------------------------------------
                        ])])], [AC_MSG_ERROR([cannot find libsvn_delta-1 headers
-------------------------------------------------------------------
libsubversion-1 is required for mesos to build.
-------------------------------------------------------------------
])])


AC_MSG_CHECKING([whether to enable the XFS disk isolator])
AS_IF([test "x$enable_xfs_disk_isolator" = "xyes"],
      [AC_MSG_RESULT([yes])],
      [AC_MSG_RESULT([no])])

AS_IF([test "x$enable_xfs_disk_isolator" = "xyes"], [
  # We only support XFS on Linux.
  AS_IF([test "$OS_NAME" = "linux"],
        [],
        [AC_MSG_ERROR([no XFS support on $OS_NAME
-------------------------------------------------------------------
The XFS disk isolator is only supported on Linux.
-------------------------------------------------------------------
  ])])

  # Check for build dependencies for the XFS disk isolator. We only
  # enable this if all the needed headers and libraries are present.
  AC_CHECK_HEADERS([xfs/xfs.h linux/dqblk_xfs.h linux/quota.h sys/quota.h],
                   [], [AC_MSG_ERROR([missing XFS quota headers
-------------------------------------------------------------------
Please install the Linux kernel headers and xfsprogs development
packages for XFS disk isolator support.
-------------------------------------------------------------------
  ])])

  # xfs_types.h is optional, depending on the build OS.
  AC_CHECK_HEADERS([xfs/xfs_types.h])

  AC_CHECK_HEADERS([blkid/blkid.h], [], [AC_MSG_ERROR([missing libblkid headers
-------------------------------------------------------------------
Please install the libblkid development package for XFS disk
isolator support.
-------------------------------------------------------------------
  ])])

  # Note that AC_SEARCH_LIBS causes libblkid to be added to each binary. In
  # this case, that is what we want, since the dependency will be in libmesos.
  AC_SEARCH_LIBS(blkid_devno_to_devname, blkid, [], [AC_MSG_ERROR([missing libblkid
-------------------------------------------------------------------
Please install the libblkid package for XFS disk isolator support.
-------------------------------------------------------------------
  ])])

  AC_DEFINE([ENABLE_XFS_DISK_ISOLATOR])
])

AM_CONDITIONAL([ENABLE_XFS_DISK_ISOLATOR], [test "x$enable_xfs_disk_isolator" = "xyes"])


# Check if zlib prefix path was provided, and if so, add it to
# the CPPFLAGS and LDFLAGS with respective /include and /lib path
# suffixes.
if test -n "`echo $with_zlib`" ; then
  ZLIB_CPPFLAGS="-I${with_zlib}/include"
  ZLIB_LINKERFLAGS="-L${with_zlib}/lib"

  CPPFLAGS="$ZLIB_CPPFLAGS $CPPFLAGS"
  LDFLAGS="$ZLIB_LINKERFLAGS $LDFLAGS"
fi

AC_CHECK_HEADERS([zlib.h],
                 [AC_CHECK_LIB([z], [deflate, gzread, gzwrite, inflate], [],
                               [AC_MSG_ERROR([cannot find libz
-------------------------------------------------------------------
libz is required for Mesos to build.
-------------------------------------------------------------------
                               ])])],
                 [AC_MSG_ERROR([cannot find libz headers
-------------------------------------------------------------------
libz headers are required for Mesos to build.
-------------------------------------------------------------------
                 ])])

AC_SUBST([ZLIB_CPPFLAGS])
AC_SUBST([ZLIB_LINKERFLAGS])


# Check if grpc prefix path was supplied and if so, add it to the
# CPPFLAGS and LDFLAGS with respective /include and /lib path suffixes.
if test -n "`echo $with_grpc`"; then
  if test "x$without_bundled_protobuf" != "xyes"; then
    AC_MSG_ERROR([protobuf not specified
-------------------------------------------------------------------
You have requested the use of a non-bundled grpc without specifying
the protobuf library it uses. This is prohibited to avoid using
conflicting versions of protobuf libraries. Please specify the
location of protobuf used by grpc by providing a prefix path via
--with-protobuf=DIR.
-------------------------------------------------------------------
    ])
  fi

  CPPFLAGS="$CPPFLAGS -I${with_grpc}/include"
  LDFLAGS="$LDFLAGS -L${with_grpc}/lib"
fi

# Check if user has asked us to use a preinstalled grpc, or if they
# asked us to ignore all bundled libraries while compiling and linking.
if test "x$without_bundled_grpc" = "xyes" || \
   test "x$enable_bundled" != "xyes"; then
  # Use the system default prefix if not specified.
  if test -n "`echo $with_grpc`"; then
    GRPCPREFIX="${with_grpc}"
  elif test "x$enable_bundled" != "xyes"; then
    GRPCPREFIX="/usr"
  fi

  if test "x$enable_ssl" = "xno"; then
    GRPC_VARIANT="_unsecure"
  fi

  # Check if headers and library were located.
  saved_LIBS="$LIBS"
  LIBS="-lgrpc++$GRPC_VARIANT -lgrpc$GRPC_VARIANT -lgpr $LIBS"
  AC_LANG_PUSH([C++])
  AC_CHECK_HEADERS([grpcpp/grpcpp.h],
                   [AC_LINK_IFELSE(
                       [AC_LANG_PROGRAM([#include <grpcpp/grpcpp.h>],
                                        [grpc::Version();])],
                       [found_grpc=yes],
                       [found_grpc=no])],
                   [found_grpc=no])
  AC_LANG_POP()
  LIBS="$saved_LIBS"

  if test "x$found_grpc" = "xyes"; then
    with_bundled_grpc=no

    AC_CHECK_TOOL([GRPC_CXX_PLUGIN_TEST],
                  [grpc_cpp_plugin],
                  [],
                  [$GRPCPREFIX/bin])

    if test -z "`echo $GRPC_CXX_PLUGIN_TEST`"; then
      AC_MSG_ERROR([grpc_cpp_plugin not found in PATH])
    fi

    GRPC_CXX_PLUGIN="$GRPCPREFIX/bin/grpc_cpp_plugin"

  else
    AC_MSG_ERROR([cannot find grpc
-------------------------------------------------------------------
You have requested the use of a non-bundled grpc but no suitable
grpc could be found.

You may want specify the location of grpc by providing a prefix
path via --with-grpc=DIR, or check that the path you provided is
correct if you are already doing this.
-------------------------------------------------------------------
    ])
  fi
else
  with_bundled_grpc=yes
fi

AM_CONDITIONAL([WITH_BUNDLED_GRPC],
               [test "x$with_bundled_grpc" = "xyes"])

AC_SUBST([GRPC_CXX_PLUGIN])


# Check if ZooKeeper prefix path was supplied and if so, add it to
# CPPFLAGS while extending it by /include/zookeeper and to LDFLAGS
# while extending it by /lib.
# NOTE: The reason we append /include/zookeeper is because in mesos,
# we include <zookeeper.h> rather than <zookeeper/zookeeper.h>.
if test -n "`echo $with_zookeeper`"; then
  CPPFLAGS="$CPPFLAGS -I${with_zookeeper}/include/zookeeper"
  LDFLAGS="$LDFLAGS -L${with_zookeeper}/lib"
elif test "x$enable_bundled" != "xyes"; then
  CPPFLAGS="$CPPFLAGS -I/usr/include/zookeeper"
fi


# Check if user has asked us to use a preinstalled ZooKeeper or if
# they asked us to ignore all bundled libraries while compiling and
# linking.
if test "x$without_bundled_zookeeper" = "xyes" || \
   test "x$enable_bundled" != "xyes"; then
  # Check if headers and library were located.
  AC_CHECK_HEADERS([zookeeper.h],
                   [AC_CHECK_LIB([zookeeper_mt],
                                 [zookeeper_init],
                                 [found_zookeeper=yes])])

  if test "x$found_zookeeper" = "xyes"; then
    with_bundled_zookeeper=no
  else
    AC_MSG_ERROR([cannot find ZooKeeper
-------------------------------------------------------------------
You have requested the use of a non-bundled ZooKeeper but no suitable
ZooKeeper could be found.

You may want specify the location of ZooKeeper by providing a prefix
path via --with-zookeeper=DIR, or check that the path you provided is
correct if you're already doing this
-------------------------------------------------------------------
])
  fi
else
  with_bundled_zookeeper=yes
fi


AM_CONDITIONAL([WITH_BUNDLED_ZOOKEEPER],
               [test "x$with_bundled_zookeeper" = "xyes"])


# Once all CXXFLAGS are completely assembled, make sure the we are not
# affected by libcxx undefined behavior,
# https://llvm.org/bugs/show_bug.cgi?id=28469.
AC_MSG_CHECKING([C++ standard library for undefined behaviour with selected optimization level])
# NOTE: We clear `LIBS` here to prevent linking in libraries unrelated
# to the test. These libraries might not be in the linker lookup paths.
saved_LIBS="$LIBS"
LIBS=""
AC_LANG_PUSH([C++])
AC_RUN_IFELSE([
  AC_LANG_SOURCE([[
    #include <functional>
    #include <map>

    struct F {
      std::map<int, std::function<void()>> m;
      F() { m[42] = []() {}; }
    };

    int main() {
      F f;
      f = F{};
    }
  ]])],
  [
    AC_MSG_RESULT([no])
  ], [
    AC_MSG_RESULT([yes])
    AC_MSG_ERROR([Mesos cannot be built with optimizations against this version of libcxx (MESOS-5745).
                  Consider building without optimizations, or changing the used C++ standard library.])
  ]
)
AC_LANG_POP([C++])
LIBS="$saved_LIBS"


# NOTE: Do not update any compiler or linker settings (e.g. CXXFLAGS,
# LDFLAGS, ...) beyond this line.

# Perform necessary configuration for building with Python.
if test "x$enable_python" = "xyes" || \
   test "x$enable_new_cli" = "xyes"; then
  # Ensure PYTHON and PYTHON_VERSION are both set appropriately. We need
  # to ensure this either when these variables are passed in the
  # environment or when set via AM_PATH_PYTHON. If we don't do this,
  # it's possible that a user will specify PYTHON_VERSION which will get
  # ignored (and likely replaced) via AM_PATH_PYTHON.
  # TODO(benh): Consolidate AM_PATH_PYTHON and AC_PYTHON_DEVEL into a
  # single macro that both iterates through potential 'python' options
  # looking for an acceptable version (i.e., AM_PATH_PYTHON) and
  # checks for Python.h (i.e., AC_PYTHON_DEVEL).
  if test -n "$PYTHON" && test -n "$PYTHON_VERSION"; then
    AC_MSG_ERROR([only specify one of PYTHON or PYTHON_VERSION])
  fi

  if test -n "$PYTHON"; then
    PYTHON_VERSION=`$PYTHON -c "import sys; sys.stdout.write(sys.version[[:3]])"`
  fi

  if test -n "$PYTHON_VERSION"; then
    AC_SUBST([PYTHON], [python$PYTHON_VERSION])
  fi

  if test -z "$PYTHON" && test -z "$PYTHON_VERSION"; then
    AM_PATH_PYTHON()
  fi
fi

if test "x$enable_python" = "xyes"; then
  # Check if PYTHON is at least 2.6 or try and find one that is if
  # PYTHON is not set (necessary to run our examples).
  AM_PYTHON_CHECK_VERSION([$PYTHON], [2.6], [],
                 [AC_MSG_ERROR([Mesos requires Python >= 2.6
-------------------------------------------------------------------
The detected Python version is $PYTHON_VERSION.

If you already have Python 2.6+ installed (and it's the default python
on the path), you might want to check if you have the PYTHON environment
variable set to an older version of Python.
-------------------------------------------------------------------
  ])])

  AM_PYTHON_CHECK_VERSION([$PYTHON], [3.0],
                 [AC_MSG_ERROR([Mesos requires Python < 3.0
-------------------------------------------------------------------
The detected Python version is $PYTHON_VERSION.

If you already have Python 2.6+ installed (and it's the default python
on the path), you might want to check if you have the PYTHON environment
variable set to a version of Python greater than 3.0.
-------------------------------------------------------------------
  ])], [])

  if test "x$enable_new_cli" = "xyes" && test -z "$PYTHON_3"; then
    AC_MSG_ERROR([Missing PYTHON_3 environment variable
-------------------------------------------------------------------
The detected Python version is $PYTHON_VERSION. Python 3 not detected.

When building both the Python bindings and the CLI, the detected Python
version is used to build the bindings and PYTHON_3 must be set
explicitely in order to build the CLI.
-------------------------------------------------------------------
    ])
  fi
fi

if test "x$enable_new_cli" = "xyes"; then
  if test -z "$PYTHON_3"; then
    AC_SUBST([PYTHON_3], [$PYTHON])
  fi

  PYTHON_3_VERSION=`$PYTHON_3 -c 'import sys; print(sys.version[[0:3]])'`

  AM_PYTHON_CHECK_VERSION([$PYTHON_3], [3.6], [],
                 [AC_MSG_ERROR([The Mesos CLI requires Python >= 3.6
-------------------------------------------------------------------
The detected Python 3 version is $PYTHON_3_VERSION.

The new CLI requires Python version 3.6 or newer in order to build.
You may wish to set the PYTHON_3 environment variable to make sure
that the right Python executable is found.
-------------------------------------------------------------------
  ])])

  # Find `tox` for testing `src/python/lib/`.
  AC_CHECK_PROGS([tox], [tox])
  if test "x$tox" = "x"; then
    AC_MSG_ERROR(['tox' is required in order to run Mesos Python library tests.])
  fi
fi

if test "x$enable_python" = "xyes"; then
  # Next we ensure we have the Python development libraries for Python 2.
  AX_PYTHON_DEVEL([>= '2.6'])

  # Ensure that we can build a native Python egg linking against a
  # static library that imports from the standard C++ library.
  # This was added due to MESOS-799.
  AC_MSG_CHECKING([whether we can build usable Python eggs])

  # Render code that imports from the standard C++ library.
  cat <<__EOF__ >testlib.cpp [
#include <string>
std::string test()
{
  return std::string("42");
}]
__EOF__

  # Build a static library from the above code.
  $CXX -x c++ -fPIC -c testlib.cpp $CPPFLAGS $CFLAGS $CXXFLAGS \
    $LDFLAGS -o testlib.o
  ar rcs libtest.a testlib.o

  # Render a native Python module that imports from that library.
  cat <<__EOF__ >testpyegg.cpp [
#include <Python.h>
#include <string>

std::string test();
PyMODINIT_FUNC initminimal();

static PyObject *minimal_test(PyObject *self, PyObject* args)
{
  return PyString_FromString(test().c_str());
}

static PyMethodDef minimal_methods[] = {
  { "test", minimal_test, METH_NOARGS, "Test." },
  { NULL, NULL }
};

PyMODINIT_FUNC initminimal()
{
  PyImport_AddModule("minimal");
  Py_InitModule("minimal", minimal_methods);
}]
__EOF__

  # Render a distutils setup for building the Python egg.
  cat <<__EOF__ >setup.py [
from distutils.core import setup, Extension;
setup(name = 'MinimalTest',
      version = '1.0',
      description = 'foo',
      ext_modules = [ Extension('minimal',
                                 extra_objects = ['libtest.a'],
                                 sources = ['testpyegg.cpp'] ) ] )
]
__EOF__

  # Build that Python egg that links against that library. The build
  # settings are propagated to distutils.
  # NOTE: We need to embed our CXXFLAGS in CFLAGS because distutils
  # doesn't pull out CXXFLAGS automagically.
  CXX="$CXX" CC="$CC" CFLAGS="$CFLAGS $CXXFLAGS" \
    LDFLAGS="$LDFLAGS" $PYTHON setup.py build_ext --inplace \
    --build-temp ./ 2>&1 >/dev/null

  # Run a test-script that makes use of the Python egg.
  pyeggtest=`$PYTHON -c "import minimal; print(minimal.test())" 2>&1`

  # Validate the test-script output.
  AS_IF([test "x$pyeggtest" = "x42"], [pyeggbuild=yes])

  # Clean up all leftovers from this test.
  rm -f testlib.cpp testlib.o libtest.a
  rm -f testpyegg.cpp testpyegg.o setup.py minimal.so

  AS_IF([test "x$pyeggbuild" = "xyes"],
        [AC_MSG_RESULT([yes])],
        [AS_IF([test "x$OS_NAME" = "xdarwin"],
               [AC_MSG_ERROR([no
-------------------------------------------------------------------
It appears we were unable to build a usable native Python egg. You
might be using the default Mac OS X Python distribution which is
incompatible for building native eggs using this compiler setup.
For more details, see:
    https://issues.apache.org/jira/browse/MESOS-799

There are two possible workarounds for this issue:
    1. Disable python bindings by configuring with --disable-python.
    2. Use an alternative Python installation that was built using
       the same build setup as Mesos.
-------------------------------------------------------------------])],
               [AC_MSG_ERROR([no
-------------------------------------------------------------------
It appears we were unable to build a usable native Python egg.

There are two possible workarounds for this issue:
    1. Disable python bindings by configuring with --disable-python.
    2. Use an alternative Python installation that was built using
       the same build setup as Mesos.
-------------------------------------------------------------------])])
        ])

  AC_MSG_CHECKING([for an old installation of the Mesos egg (before 0.20.0)])

  $PYTHON -c "import mesos; mesos._mesos" &> /dev/null

  if test $? = 0; then
    pymodulelocation=`$PYTHON -c \
      "import mesos; import os; print os.path.dirname(mesos.__path__[[0]])" \
      2> /dev/null`
  fi

  AS_IF([test -z "$pymodulelocation"],
        [AC_MSG_RESULT([no])],
        [AC_MSG_ERROR([yes
-------------------------------------------------------------------
It appears that you currently have a native Python egg installed
from a version before 0.20.0. This conflicts with the egg in this
version.

There are two possible workarounds for this issue:
    1. Disable Python bindings by configuring with --disable-python.
    2. Uninstall the legacy egg from your Python installation. This
       might require you doing:
         rm -rf $pymodulelocation
-------------------------------------------------------------------])])

  # Determine how the generated Python egg's will get named, used in
  # the Makefile to keep the targets from being rerun.
  PYTHON_EGG_POSTFIX=`$PYTHON -c \
    'import sys; \
     from distutils.util import get_platform; \
     print "-py" + sys.version[[0:3]] + "-" + get_platform()'`

  PYTHON_EGG_PUREPY_POSTFIX=`$PYTHON -c \
    'import sys; \
     from distutils.util import get_platform; \
     print "-py" + sys.version[[0:3]]'`

  PYTHON_WHL_POSTFIX=`$PYTHON -c \
    'import sys; \
     from distutils.util import get_platform; \
     print "-cp" + sys.version[[0:3]].replace(".", "") + "-none-" \
       + get_platform().replace(".", "_").replace("-", "_")'`

  PYTHON_WHL_PUREPY_POSTFIX=`$PYTHON -c \
    'import sys; \
     print "-py" + sys.version[[0]] + "-none-any"'`

  AC_CONFIG_FILES([src/examples/python/test-executor],
                  [chmod +x src/examples/python/test-executor])
  AC_CONFIG_FILES([src/examples/python/test-framework],
                  [chmod +x src/examples/python/test-framework])
  AC_CONFIG_FILES([src/python/setup.py])
  AC_CONFIG_FILES([src/python/cli/setup.py])
  AC_CONFIG_FILES([src/python/interface/setup.py])
  AC_CONFIG_FILES([src/python/native_common/ext_modules.py])
  AC_CONFIG_FILES([src/python/executor/setup.py])
  AC_CONFIG_FILES([src/python/native/setup.py])
  AC_CONFIG_FILES([src/python/scheduler/setup.py])

  AC_CONFIG_LINKS([src/python/executor/ext_modules.py:src/python/native_common/ext_modules.py])
  AC_CONFIG_LINKS([src/python/scheduler/ext_modules.py:src/python/native_common/ext_modules.py])

  # When clang is being used, make sure that the distutils python-
  # config cflags extraction does not cause build errors (MESOS-1079).
  # TODO(tillt): Remove this once Apple distributed an updated Python.
  PYTHON_CPPFLAGS="$CPPFLAGS"
  PYTHON_CFLAGS="$CFLAGS $CXXFLAGS" # distutils requires we embed CXXFLAGS.
  PYTHON_LDFLAGS="$LDFLAGS $GFLAGS_LIBS"

  AS_IF([test "x$ax_cv_cxx_compiler_vendor" = "xclang"],
	[PYTHON_CPPFLAGS="$PYTHON_CPPFLAGS -Qunused-arguments"
         PYTHON_CFLAGS="$PYTHON_CFLAGS -Qunused-arguments"])

  AC_SUBST([PYTHON_CPPFLAGS])
  AC_SUBST([PYTHON_CFLAGS])
  AC_SUBST([PYTHON_EGG_POSTFIX])
  AC_SUBST([PYTHON_EGG_PUREPY_POSTFIX])
  AC_SUBST([PYTHON_WHL_POSTFIX])
  AC_SUBST([PYTHON_WHL_PUREPY_POSTFIX])
  AC_SUBST([PYTHON]) # Used by the example shell scripts and src/Makefile.am.

  AC_DEFINE([MESOS_HAS_PYTHON])

  # Check if user has asked us to use a preinstalled setuptools, or if
  # they asked us to ignore all bundled libraries while compiling and linking.
  if test "x$enable_bundled_setuptools" != "xyes" || \
     test "x$enable_bundled" != "xyes"; then

    AC_PYTHON_MODULE([distutils], [no])
    if test "x$HAVE_PYMOD_distutils" = "xno"; then
      AC_PYTHON_MODULE([setuptools], [no])

      if test "x$HAVE_PYMOD_setuptools" = "xyes"; then
        found_setuptools=yes
      fi

    else
      found_setuptools=yes
    fi

    if test "x$found_setuptools" = "xyes"; then
      with_bundled_setuptools=no
    else
      AC_MSG_ERROR([cannot find setuptools
-------------------------------------------------------------------
You have requested the use of a non-bundled setuptools but no suitable
setuptools could be found.

You may want specify the location of setuptools by providing a prefix
path via --with-setuptools=DIR, or check that the path you provided is
correct if you're already doing this.
-------------------------------------------------------------------
])
    fi
  else
    with_bundled_setuptools=yes
  fi

  # Check if user has asked us to use a preinstalled pip, or if
  # they asked us to ignore all bundled libraries while compiling and linking.
  if test "x$enable_bundled_pip" != "xyes" || \
     test "x$enable_bundled" != "xyes"; then

    AC_PYTHON_MODULE([pip], [no])
    if test "x$HAVE_PYMOD_pip" = "xno"; then
      AC_MSG_ERROR([cannot find pip
-------------------------------------------------------------------
You have requested the use of a non-bundled pip but no suitable
pip could be found.

You may want specify the location of pip by providing a prefix
path via --with-pip=DIR, or check that the path you provided is
correct if you're already doing this.
-------------------------------------------------------------------
])
    else
      with_bundled_pip=no
    fi
  else
    with_bundled_pip=yes
  fi

  # Check if user has asked us to use a preinstalled wheel, or if
  # they asked us to ignore all bundled libraries while compiling and linking.
  if test "x$enable_bundled_wheel" != "xyes" || \
     test "x$enable_bundled" != "xyes"; then

    AC_PYTHON_MODULE([wheel], [no])
    if test "x$HAVE_PYMOD_wheel" = "xno"; then
      AC_MSG_ERROR([cannot find wheel
-------------------------------------------------------------------
You have requested the use of a non-bundled wheel but no suitable
wheel could be found.

You may want specify the location of wheel by providing a prefix
path via --with-wheel=DIR, or check that the path you provided is
correct if you're already doing this.
-------------------------------------------------------------------
])
    else
      with_bundled_wheel=no
    fi
  else
    with_bundled_wheel=yes
  fi

  has_python=yes
fi

AC_MSG_CHECKING([whether to enable new CLI])
AS_IF([test "x$enable_new_cli" = "xyes"],
      [AC_MSG_RESULT([yes])],
      [AC_MSG_RESULT([no])])

AM_CONDITIONAL([ENABLE_NEW_CLI], [test "x$enable_new_cli" = "xyes"])

AM_CONDITIONAL([HAS_PYTHON], [test "x$has_python" = "xyes"])

AM_CONDITIONAL([WITH_BUNDLED_SETUPTOOLS],
    [test "x$with_bundled_setuptools" = "xyes"])
AM_CONDITIONAL([WITH_BUNDLED_PIP],
    [test "x$with_bundled_pip" = "xyes"])
AM_CONDITIONAL([WITH_BUNDLED_WHEEL],
    [test "x$with_bundled_wheel" = "xyes"])

AM_CONDITIONAL([ENABLE_PYTHON_DEPENDENCY_INSTALL],
    [test "x$enable_python_dependency_install" = "xyes"])


###############################################################################
# Configure args to be passed to 3rdparty libraries.
###############################################################################

CONFIGURE_ARGS="$CONFIGURE_ARGS CXXFLAGS='$CXXFLAGS'"


###############################################################################
# Mesos version definitions.
###############################################################################

# Provide more granular version numbers based on the version string,
# using the format MAJOR.MINOR.PATCH[-SUFFIX], where SUFFIX can itself
# contain dashes.
#
# NOTE: This definition must not be named such that it conflicts with
# existing definitions, e.g., 'version', 'ver'.
m4_define([major_minor_patch],
          m4_split(m4_translit(AC_PACKAGE_VERSION, [-], [.]), [\.]))

AC_SUBST(MESOS_MAJOR_VERSION, m4_car(major_minor_patch))
AC_SUBST(MESOS_MINOR_VERSION, m4_car(m4_shift(major_minor_patch)))
AC_SUBST(MESOS_PATCH_VERSION, m4_car(m4_shift2(major_minor_patch)))


###############################################################################
# Configure libbuild definitions passed into 'src/common/git_version.hpp'.
###############################################################################

git_dir="${srcdir}/.git/"
git_config=src/common/git_version.hpp

# Create 'src/common/git_version.hpp' only if we did not do so before. This
# protects the results from getting overwritten by additional configure runs
# outside the reach of the git repository, as for example done by
# 'support/packaging/centos/build_rpm.sh'.

AC_MSG_CHECKING([${git_config} presence])

AS_IF([test -f ${srcdir}/${git_config} || test -f ${git_config}], [AC_MSG_RESULT([yes])], [
  AC_MSG_RESULT([no])

  AC_MSG_NOTICE([generating ${git_config}])

  # When building from a git clone, the definitions BUILD_GIT_SHA,
  # BUILD_GIT_BRANCH, and BUILD_GIT_TAG will be emitted.

  AS_IF([test -d ${git_dir}], [
    # Optionally set BUILD_GIT_SHA.
    DEFINE_BUILD_GIT_SHA=""

    git_sha=$(git --git-dir=${git_dir} rev-parse HEAD 2>/dev/null)

    AS_IF([test $? = 0], [
      DEFINE_BUILD_GIT_SHA="#define BUILD_GIT_SHA \"${git_sha}\""
    ])
    AC_SUBST([DEFINE_BUILD_GIT_SHA])

    # Optionally set BUILD_GIT_BRANCH.
    DEFINE_BUILD_GIT_BRANCH=""

    git_branch=$(git --git-dir=${git_dir} symbolic-ref HEAD 2>/dev/null)

    AS_IF([test $? = 0], [
      DEFINE_BUILD_GIT_BRANCH="#define BUILD_GIT_BRANCH \"${git_branch}\""
    ])
    AC_SUBST([DEFINE_BUILD_GIT_BRANCH])

    # Optionally set BUILD_GIT_TAG.
    DEFINE_BUILD_GIT_TAG=""

    git_tag=$(git --git-dir=${git_dir} describe --exact --tags 2>/dev/null)

    AS_IF([test $? = 0], [
      DEFINE_BUILD_GIT_TAG="#define BUILD_GIT_TAG \"${git_tag}\""
    ])
    AC_SUBST([DEFINE_BUILD_GIT_TAG])
  ])

  AC_CONFIG_FILES([${git_config}])
])


AC_OUTPUT

AC_MSG_NOTICE([Build option summary:
    CXX:        $CXX
    CXXFLAGS:   $CXXFLAGS
    CPPFLAGS:   $CPPFLAGS
    LDFLAGS:    $LDFLAGS
    LIBS:       $LIBS
    JAVA_TEST_LDFLAGS: $JAVA_TEST_LDFLAGS
    JAVA_JVM_LIBRARY:  $JAVA_JVM_LIBRARY
])
