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
# limitations under the License

# Add 'subdir-objects' to build objects in respective subdirectories
# which is necessary for dealing with naming conflicts, i.e,,
# master/http.cpp and slave/http.cpp.
AUTOMAKE_OPTIONS = subdir-objects
MESOS_BUILD_DIR=@abs_top_builddir@

include ../3rdparty/versions.am

# TODO(charles): Move these into an included automakefile and have
# them include $(top_builddir) as appropriate.
BOOST = 3rdparty/boost-$(BOOST_VERSION)
CONCURRENTQUEUE = 3rdparty/concurrentqueue-$(CONCURRENTQUEUE_VERSION)
CSI_V0 = 3rdparty/csi-$(CSI_V0_VERSION)
CSI_V1 = 3rdparty/csi-$(CSI_V1_VERSION)
ELFIO = 3rdparty/elfio-$(ELFIO_VERSION)
GLOG = 3rdparty/glog-$(GLOG_VERSION)
GMOCK = $(GOOGLETEST)/googlemock
GOOGLETEST = 3rdparty/googletest-release-$(GOOGLETEST_VERSION)
GRPC = 3rdparty/grpc-$(GRPC_VERSION)
GTEST = $(GOOGLETEST)/googletest
JEMALLOC = 3rdparty/jemalloc-$(JEMALLOC_VERSION)
LEVELDB = 3rdparty/leveldb-$(LEVELDB_VERSION)
LIBARCHIVE = 3rdparty/libarchive-$(LIBARCHIVE_VERSION)
LIBPROCESS = 3rdparty/libprocess
LIBSECCOMP = 3rdparty/libseccomp-$(LIBSECCOMP_VERSION)
NVML = 3rdparty/nvml-$(NVML_VERSION)
PICOJSON = 3rdparty/picojson-$(PICOJSON_VERSION)
PIP = 3rdparty/pip-$(PIP_VERSION)
PROTOBUF = 3rdparty/protobuf-$(PROTOBUF_VERSION)
RAPIDJSON = 3rdparty/rapidjson-$(RAPIDJSON_VERSION)
RE2 = 3rdparty/re2-$(RE2_VERSION)
SETUPTOOLS = 3rdparty/setuptools-$(SETUPTOOLS_VERSION)
STOUT = 3rdparty/stout
WHEEL = 3rdparty/wheel-$(WHEEL_VERSION)
ZOOKEEPER = 3rdparty/zookeeper-$(ZOOKEEPER_VERSION)/src/c
ZOOKEEPER_JAR = 3rdparty/zookeeper-$(ZOOKEEPER_VERSION)/zookeeper-$(ZOOKEEPER_VERSION).jar


# Unfortunatley, 'pkglibexecdir' and 'pkgsysconfdir' are not set
# before automake 1.11, so we need to set them manually (until we in
# the future assume an automake version).
pkglibexecdir = $(libexecdir)/$(PACKAGE)
pkgsysconfdir = $(sysconfdir)/$(PACKAGE)

# Directories to optionally install test binaries and libraries.
testlibexecdir = $(pkglibexecdir)/tests

# Directory where Mesos modules are installed.
pkgmoduledir = $(pkglibdir)/modules
relative_pkgmoduledir = $(PACKAGE)/modules

# Initialize variables here so we can use += operator everywhere else.
lib_LTLIBRARIES =
pkgmodule_LTLIBRARIES =
noinst_LTLIBRARIES =
sbin_PROGRAMS =
bin_PROGRAMS =
pkglibexec_PROGRAMS =
dist_bin_SCRIPTS =
dist_pkglibexec_SCRIPTS =
nobase_dist_pkgdata_DATA =
nodist_sbin_SCRIPTS =
pkgsysconf_DATA =
check_PROGRAMS =
dist_check_SCRIPTS =
check_SCRIPTS =
BUILT_SOURCES =
CLEANFILES =
EXTRA_DIST =
PHONY_TARGETS =
LDADD =

AM_LDFLAGS =

if DISABLE_LIBTOOL_WRAPPERS
# This is used to force libtool to generate executables instead of wrapper
# scripts. A wrapper script might trigger relinking, which takes quite a while
# on slow machines, thus causing failure of tests.
# NOTE: Disabling libtool wrapper should only be used when building a mesos to
# be tested within the build tree, see `info libtool` for details.
AM_LDFLAGS += -no-install
endif

# Use -Wl,--as-needed to AM_LDFLAGS since we link against all the
# libraries programs may depend upon, not the exact one. --as-needed
# causes the linker to only link to the ones a program actually uses,
# not the full list on the command line.
if OS_LINUX
AM_LDFLAGS += -Wl,--as-needed
endif

# Add compiler and linker flags for pthreads.
AM_CXXFLAGS = $(PTHREAD_CFLAGS)

# Enable common (and some language specific) warnings.
AM_CXXFLAGS += -Wall
# Warn when a comparison is made between signed and unsigned values.
AM_CXXFLAGS += -Wsign-compare
# Warn about use of format functions that can produce security issues.
AM_CXXFLAGS += -Wformat-security

# We will also have much more hardened/secured binaries and libraries.
if ENABLE_HARDENING
# Produce position independent executables so that we better take advantage of ASLR.
AM_LDFLAGS += -pie
# Protect many of the functions with stack guards
# (either -fstack-protector-strong or -fstack-protector depending on compiler support).
AM_CXXFLAGS += @STACK_PROTECTOR@
# Produce position independent code when appropriate.
AM_CXXFLAGS += -fPIC -fPIE
endif

AM_LIBS = $(PTHREAD_LIBS)

# Setup CPPFLAGS that are used for most source files.
MESOS_CPPFLAGS = $(AM_CPPFLAGS)
MESOS_CPPFLAGS += @WERROR@
MESOS_CPPFLAGS += -DLIBDIR=\"$(libdir)\"
MESOS_CPPFLAGS += -DPKGLIBEXECDIR=\"$(pkglibexecdir)\"
MESOS_CPPFLAGS += -DPKGDATADIR=\"$(pkgdatadir)\"
MESOS_CPPFLAGS += -DPKGMODULEDIR=\"$(pkgmoduledir)\"
MESOS_CPPFLAGS += -I$(top_srcdir)/include
MESOS_CPPFLAGS += -I../include

# Protobuf headers that depend on mesos.pb.h need this.
MESOS_CPPFLAGS += -I../include/mesos

# Set up include paths for the protocol buffer compiler.
PROTOCFLAGS = -I$(top_srcdir)/include -I$(srcdir) -I../$(CSI_V0) -I../$(CSI_V1)

# Header only dependencies will be ignored for --disable-bundled.
#
# For non-convenience libraries we need to link them in to make the shared
# library each time. (Currently, we don't support platforms where this is not
# possible.)

# NOTE: PicoJson requires __STDC_FORMAT_MACROS to be defined before importing
# 'inttypes.h'.  Since other libraries may also import this header, it must
# be globally defined so that PicoJson has access to the macros, regardless
# of the order of inclusion.
MESOS_CPPFLAGS += -D__STDC_FORMAT_MACROS

if WITH_BUNDLED_BOOST
MESOS_CPPFLAGS += -I../$(BOOST)
endif

if WITH_BUNDLED_CONCURRENTQUEUE
MESOS_CPPFLAGS += -I../$(CONCURRENTQUEUE)
endif

if WITH_BUNDLED_ELFIO
MESOS_CPPFLAGS += -I../$(ELFIO)
endif

if WITH_BUNDLED_GLOG
MESOS_CPPFLAGS += -I../$(GLOG)/src
LIB_GLOG = ../$(GLOG)/libglog.la
else
LIB_GLOG = -lglog
LDADD += -lglog
endif

if !ENABLE_SSL
GRPC_VARIANT = _unsecure
endif

if WITH_BUNDLED_GRPC
MESOS_CPPFLAGS += -I../$(GRPC)/include
LIB_GRPC = ../$(GRPC)/libs/opt/libgrpc++$(GRPC_VARIANT).a	\
           ../$(GRPC)/libs/opt/libgrpc$(GRPC_VARIANT).a		\
           ../$(GRPC)/libs/opt/libgpr.a
GRPC_CPP_PLUGIN = ../$(GRPC)/bins/opt/grpc_cpp_plugin
else
LIB_GRPC = -lgrpc++$(GRPC_VARIANT) -lgrpc$(GRPC_VARIANT) -lgpr
GRPC_CPP_PLUGIN = @GRPC_CXX_PLUGIN@
endif

if WITH_BUNDLED_JEMALLOC
# TODO(bennoe): We could call `jemalloc-conf --libs` to get the required
# libs without hardcoding `-ldl`, but then a version bump could introduce
# additional dependencies without us being aware of it.
LIB_JEMALLOC = ../$(JEMALLOC)/lib/libjemalloc_pic.a -ldl
else
LIB_JEMALLOC = $(if $(WITH_JEMALLOC),\
    -L$(WITH_JEMALLOC)/lib -ljemalloc -ldl,\
    -ljemalloc -ldl)
endif

if WITH_BUNDLED_LEVELDB
MESOS_CPPFLAGS += -I../$(LEVELDB)/include

# We need to directly include the leveldb library in order to avoid
# the installed libmesos.la file to include leveldb in
# 'dependency_libs' (via '-L../3rdparty/leveldb -lleveldb').

LIB_LEVELDB = ../$(LEVELDB)/out-static/libleveldb.a
else
LIB_LEVELDB = -lleveldb
LDADD += -lleveldb
endif

if WITH_BUNDLED_LIBARCHIVE
MESOS_CPPFLAGS += -I../$(LIBARCHIVE)/libarchive/
LIB_LIBARCHIVE = ../$(LIBARCHIVE)/.libs/libarchive.la
LDADD += $(LIB_LIBARCHIVE)
else
LIB_LIBARCHIVE = -larchive
LDADD += -larchive
endif

if WITH_BUNDLED_LIBPROCESS
MESOS_CPPFLAGS += -I$(top_srcdir)/$(LIBPROCESS)/include
LIB_PROCESS = ../$(LIBPROCESS)/libprocess.la
else
LIB_PROCESS = -lprocess
endif

if ENABLE_SECCOMP_ISOLATOR
if WITH_BUNDLED_LIBSECCOMP
MESOS_CPPFLAGS += -I../$(LIBSECCOMP)/include
LIB_LIBSECCOMP = ../$(LIBSECCOMP)/src/libseccomp.la
else
LIB_LIBSECCOMP = -lseccomp
endif
endif

if ENABLE_NVML
if WITH_BUNDLED_NVML
MESOS_CPPFLAGS += -I../$(NVML)
endif
endif

if WITH_BUNDLED_PICOJSON
MESOS_CPPFLAGS += -I../$(PICOJSON)
endif

if WITH_BUNDLED_PROTOBUF
MESOS_CPPFLAGS += -I../$(PROTOBUF)/src
LIB_PROTOBUF = ../$(PROTOBUF)/src/libprotobuf.la
PROTOC = ../$(PROTOBUF)/src/protoc
PROTOCFLAGS += -I../$(PROTOBUF)/src
else
LIB_PROTOBUF = -lprotobuf
LDADD += -lprotobuf
PROTOC = @PROTOCOMPILER@
PROTOCFLAGS += @PROTOBUF_PROTOCFLAGS@
endif

if WITH_BUNDLED_RAPIDJSON
MESOS_CPPFLAGS += -I../$(RAPIDJSON)/include
endif

if WITH_BUNDLED_RE2
MESOS_CPPFLAGS += -I../$(RE2)
LIB_RE2 = ../$(RE2)/obj/libre2.a
else
LIB_RE2 = -lre2
LDADD += -lre2
endif


if WITH_BUNDLED_STOUT
MESOS_CPPFLAGS += -I$(top_srcdir)/$(STOUT)/include
endif

if WITH_BUNDLED_ZOOKEEPER
MESOS_CPPFLAGS += -I../$(ZOOKEEPER)/include
MESOS_CPPFLAGS += -I../$(ZOOKEEPER)/generated
LIB_ZOOKEEPER = ../$(ZOOKEEPER)/libzookeeper_mt.la
else
LIB_ZOOKEEPER = -lzookeeper_mt
LDADD += -lzookeeper_mt
endif

# README: we build the Mesos library out of a collection of
# convenience libraries (that is, libraries that do not get installed
# but we can use as building blocks to vary compile flags as necessary
# and then aggregate into final archives): libmesos_no_3rdparty.la
# libbuild.la, liblog.la, libjava.la.

# First, let's define necessary protocol buffer files.

ACLS_PROTO = $(top_srcdir)/include/mesos/authorizer/acls.proto
AGENT_PROTO = $(top_srcdir)/include/mesos/agent/agent.proto
ALLOCATOR_PROTO = $(top_srcdir)/include/mesos/allocator/allocator.proto
APPC_SPEC_PROTO = $(top_srcdir)/include/mesos/appc/spec.proto
AUTHENTICATION_PROTO = $(top_srcdir)/include/mesos/authentication/authentication.proto
AUTHORIZATION_PROTO = $(top_srcdir)/include/mesos/authorizer/authorizer.proto
CONTAINERIZER_PROTO = $(top_srcdir)/include/mesos/slave/containerizer.proto
DOCKER_SPEC_PROTO = $(top_srcdir)/include/mesos/docker/spec.proto
DOCKER_V1_PROTO = $(top_srcdir)/include/mesos/docker/v1.proto
DOCKER_V2_PROTO = $(top_srcdir)/include/mesos/docker/v2.proto
DOCKER_V2_2_PROTO = $(top_srcdir)/include/mesos/docker/v2_2.proto
EXECUTOR_PROTO = $(top_srcdir)/include/mesos/executor/executor.proto
FETCHER_PROTO = $(top_srcdir)/include/mesos/fetcher/fetcher.proto
HOOK_PROTO = $(top_srcdir)/include/mesos/module/hook.proto
MAINTENANCE_PROTO = $(top_srcdir)/include/mesos/maintenance/maintenance.proto
MASTER_PROTO = $(top_srcdir)/include/mesos/master/master.proto
MESOS_PROTO = $(top_srcdir)/include/mesos/mesos.proto
MODULE_PROTO = $(top_srcdir)/include/mesos/module/module.proto
OCI_SPEC_PROTO = $(top_srcdir)/include/mesos/oci/spec.proto
QUOTA_PROTO = $(top_srcdir)/include/mesos/quota/quota.proto
RESOURCE_PROVIDER_PROTO = $(top_srcdir)/include/mesos/resource_provider/resource_provider.proto
SCHEDULER_PROTO = $(top_srcdir)/include/mesos/scheduler/scheduler.proto
SECCOMP_PROTO = $(top_srcdir)/include/mesos/seccomp/seccomp.proto
STATE_PROTO = $(top_srcdir)/include/mesos/state/state.proto
OVERSUBSCRIPTION_PROTO = $(top_srcdir)/include/mesos/slave/oversubscription.proto
URI_PROTO = $(top_srcdir)/include/mesos/uri/uri.proto
V1_AGENT_PROTO = $(top_srcdir)/include/mesos/v1/agent/agent.proto
V1_ALLOCATOR_PROTO = $(top_srcdir)/include/mesos/v1/allocator/allocator.proto
V1_EXECUTOR_PROTO = $(top_srcdir)/include/mesos/v1/executor/executor.proto
V1_MAINTENANCE_PROTO = $(top_srcdir)/include/mesos/v1/maintenance/maintenance.proto
V1_MASTER_PROTO = $(top_srcdir)/include/mesos/v1/master/master.proto
V1_MESOS_PROTO = $(top_srcdir)/include/mesos/v1/mesos.proto
V1_QUOTA_PROTO = $(top_srcdir)/include/mesos/v1/quota/quota.proto
V1_RESOURCE_PROVIDER_PROTO = $(top_srcdir)/include/mesos/v1/resource_provider/resource_provider.proto
V1_SCHEDULER_PROTO = $(top_srcdir)/include/mesos/v1/scheduler/scheduler.proto


CXX_PROTOS =								\
  ../include/mesos/mesos.pb.cc						\
  ../include/mesos/mesos.pb.h						\
  ../include/mesos/agent/agent.pb.cc					\
  ../include/mesos/agent/agent.pb.h					\
  ../include/mesos/allocator/allocator.pb.cc				\
  ../include/mesos/allocator/allocator.pb.h				\
  ../include/mesos/appc/spec.pb.cc					\
  ../include/mesos/appc/spec.pb.h					\
  ../include/mesos/authentication/authentication.pb.cc			\
  ../include/mesos/authentication/authentication.pb.h			\
  ../include/mesos/authorizer/acls.pb.cc				\
  ../include/mesos/authorizer/acls.pb.h					\
  ../include/mesos/authorizer/authorizer.pb.cc				\
  ../include/mesos/authorizer/authorizer.pb.h				\
  ../include/mesos/docker/spec.pb.cc					\
  ../include/mesos/docker/spec.pb.h					\
  ../include/mesos/docker/v1.pb.cc					\
  ../include/mesos/docker/v1.pb.h					\
  ../include/mesos/docker/v2.pb.cc					\
  ../include/mesos/docker/v2.pb.h					\
  ../include/mesos/docker/v2_2.pb.cc					\
  ../include/mesos/docker/v2_2.pb.h					\
  ../include/mesos/executor/executor.pb.cc				\
  ../include/mesos/executor/executor.pb.h				\
  ../include/mesos/fetcher/fetcher.pb.cc				\
  ../include/mesos/fetcher/fetcher.pb.h					\
  ../include/mesos/maintenance/maintenance.pb.cc			\
  ../include/mesos/maintenance/maintenance.pb.h				\
  ../include/mesos/master/master.pb.cc					\
  ../include/mesos/master/master.pb.h					\
  ../include/mesos/module/hook.pb.cc					\
  ../include/mesos/module/hook.pb.h					\
  ../include/mesos/module/module.pb.cc					\
  ../include/mesos/module/module.pb.h					\
  ../include/mesos/oci/spec.pb.cc					\
  ../include/mesos/oci/spec.pb.h					\
  ../include/mesos/quota/quota.pb.cc					\
  ../include/mesos/quota/quota.pb.h					\
  ../include/mesos/resource_provider/resource_provider.pb.cc		\
  ../include/mesos/resource_provider/resource_provider.pb.h		\
  ../include/mesos/scheduler/scheduler.pb.cc				\
  ../include/mesos/scheduler/scheduler.pb.h				\
  ../include/mesos/seccomp/seccomp.pb.cc				\
  ../include/mesos/seccomp/seccomp.pb.h					\
  ../include/mesos/slave/containerizer.pb.cc				\
  ../include/mesos/slave/containerizer.pb.h				\
  ../include/mesos/slave/oversubscription.pb.cc				\
  ../include/mesos/slave/oversubscription.pb.h				\
  ../include/mesos/state/state.pb.cc					\
  ../include/mesos/state/state.pb.h					\
  ../include/mesos/uri/uri.pb.cc					\
  ../include/mesos/uri/uri.pb.h						\
  ../include/mesos/v1/mesos.pb.cc					\
  ../include/mesos/v1/mesos.pb.h					\
  ../include/mesos/v1/agent/agent.pb.cc					\
  ../include/mesos/v1/agent/agent.pb.h					\
  ../include/mesos/v1/allocator/allocator.pb.cc				\
  ../include/mesos/v1/allocator/allocator.pb.h				\
  ../include/mesos/v1/executor/executor.pb.cc				\
  ../include/mesos/v1/executor/executor.pb.h				\
  ../include/mesos/v1/maintenance/maintenance.pb.cc			\
  ../include/mesos/v1/maintenance/maintenance.pb.h			\
  ../include/mesos/v1/master/master.pb.cc				\
  ../include/mesos/v1/master/master.pb.h				\
  ../include/mesos/v1/quota/quota.pb.cc					\
  ../include/mesos/v1/quota/quota.pb.h					\
  ../include/mesos/v1/resource_provider/resource_provider.pb.cc		\
  ../include/mesos/v1/resource_provider/resource_provider.pb.h		\
  ../include/mesos/v1/scheduler/scheduler.pb.cc				\
  ../include/mesos/v1/scheduler/scheduler.pb.h

CXX_PROTOS +=								\
  master/registry.pb.cc							\
  master/registry.pb.h							\
  messages/flags.pb.cc							\
  messages/flags.pb.h							\
  messages/messages.pb.cc						\
  messages/messages.pb.h						\
  resource_provider/registry.pb.cc					\
  resource_provider/registry.pb.h					\
  resource_provider/state.pb.cc						\
  resource_provider/state.pb.h						\
  slave/state.pb.cc							\
  slave/state.pb.h							\
  slave/containerizer/mesos/provisioner/docker/message.pb.cc		\
  slave/containerizer/mesos/provisioner/docker/message.pb.h		\
  slave/containerizer/mesos/isolators/docker/volume/state.pb.cc		\
  slave/containerizer/mesos/isolators/docker/volume/state.pb.h		\
  slave/containerizer/mesos/isolators/network/cni/spec.pb.cc		\
  slave/containerizer/mesos/isolators/network/cni/spec.pb.h		\
  slave/containerizer/mesos/isolators/volume/csi/state.pb.cc		\
  slave/containerizer/mesos/isolators/volume/csi/state.pb.h		\
  slave/volume_gid_manager/state.pb.cc					\
  slave/volume_gid_manager/state.pb.h

CXX_PROTOS +=								\
  resource_provider/storage/disk_profile.pb.cc				\
  resource_provider/storage/disk_profile.pb.h

CXX_CSI_PROTOS =							\
  ../include/csi/v0/csi.grpc.pb.cc					\
  ../include/csi/v0/csi.grpc.pb.h					\
  ../include/csi/v0/csi.pb.cc						\
  ../include/csi/v0/csi.pb.h						\
  ../include/csi/v1/csi.grpc.pb.cc					\
  ../include/csi/v1/csi.grpc.pb.h					\
  ../include/csi/v1/csi.pb.cc						\
  ../include/csi/v1/csi.pb.h						\
  csi/state.pb.cc							\
  csi/state.pb.h

CXX_LOG_PROTOS =							\
  messages/log.pb.cc							\
  messages/log.pb.h

CXX_STATE_PROTOS =							\
  messages/state.pb.cc							\
  messages/state.pb.h

JAVA_PROTOS =								\
  java/generated/org/apache/mesos/Protos.java				\
  java/generated/org/apache/mesos/executor/Protos.java			\
  java/generated/org/apache/mesos/scheduler/Protos.java

PYTHON_PROTOS =								\
  python/interface/src/mesos/interface/mesos_pb2.py

V1_JAVA_PROTOS =							\
  java/generated/org/apache/mesos/v1/Protos.java			\
  java/generated/org/apache/mesos/v1/agent/Protos.java			\
  java/generated/org/apache/mesos/v1/allocator/Protos.java		\
  java/generated/org/apache/mesos/v1/executor/Protos.java		\
  java/generated/org/apache/mesos/v1/maintenance/Protos.java		\
  java/generated/org/apache/mesos/v1/master/Protos.java			\
  java/generated/org/apache/mesos/v1/quota/Protos.java			\
  java/generated/org/apache/mesos/v1/resource_provider/Protos.java	\
  java/generated/org/apache/mesos/v1/scheduler/Protos.java

V1_PYTHON_PROTOS =                                                      \
  python/interface/src/mesos/v1/interface/agent_pb2.py                  \
  python/interface/src/mesos/v1/interface/allocator_pb2.py              \
  python/interface/src/mesos/v1/interface/executor_pb2.py               \
  python/interface/src/mesos/v1/interface/maintenance_pb2.py            \
  python/interface/src/mesos/v1/interface/master_pb2.py                 \
  python/interface/src/mesos/v1/interface/mesos_pb2.py                  \
  python/interface/src/mesos/v1/interface/quota_pb2.py                  \
  python/interface/src/mesos/v1/interface/resource_provider_pb2.py      \
  python/interface/src/mesos/v1/interface/scheduler_pb2.py

BUILT_SOURCES +=							\
  $(CXX_CSI_PROTOS)							\
  $(CXX_LOG_PROTOS)							\
  $(CXX_PROTOS)								\
  $(CXX_STATE_PROTOS)							\
  $(JAVA_PROTOS)							\
  $(PYTHON_PROTOS)							\
  $(V1_JAVA_PROTOS)							\
  $(V1_PYTHON_PROTOS)

CLEANFILES +=								\
  $(CXX_CSI_PROTOS)							\
  $(CXX_LOG_PROTOS)							\
  $(CXX_PROTOS)								\
  $(CXX_STATE_PROTOS)							\
  $(JAVA_PROTOS)							\
  $(PYTHON_PROTOS)							\
  $(V1_JAVA_PROTOS)							\
  $(V1_PYTHON_PROTOS)


# Targets for generating C++ protocol buffer and gRPC code.
../include/mesos/%.pb.cc ../include/mesos/%.pb.h: $(top_srcdir)/include/mesos/%.proto
	$(PROTOC) $(PROTOCFLAGS) --cpp_out=../include $^

%.pb.cc %.pb.h: %.proto
	$(PROTOC) $(PROTOCFLAGS) --cpp_out=. $^

../include/%.grpc.pb.cc ../include/%.grpc.pb.h ../include/%.pb.cc ../include/%.pb.h: ../$(CSI_V0)/%.proto
	$(PROTOC) $(PROTOCFLAGS) --cpp_out=../include --grpc_out=../include	\
		--plugin=protoc-gen-grpc=$(GRPC_CPP_PLUGIN) $^

../include/%.grpc.pb.cc ../include/%.grpc.pb.h ../include/%.pb.cc ../include/%.pb.h: ../$(CSI_V1)/%.proto
	$(PROTOC) $(PROTOCFLAGS) --cpp_out=../include --grpc_out=../include	\
		--plugin=protoc-gen-grpc=$(GRPC_CPP_PLUGIN) $^

# Targets for generating Java protocol buffer code.
java/generated/org/apache/mesos/executor/Protos.java: $(EXECUTOR_PROTO)
	$(MKDIR_P) $(@D)
	$(PROTOC) $(PROTOCFLAGS) --java_out=java/generated $^

java/generated/org/apache/mesos/fetcher/Protos.java: $(FETCHER_PROTO)
	$(MKDIR_P) $(@D)
	$(PROTOC) $(PROTOCFLAGS) --java_out=java/generated $^

java/generated/org/apache/mesos/Protos.java: $(MESOS_PROTO)
	$(MKDIR_P) $(@D)
	$(PROTOC) $(PROTOCFLAGS) --java_out=java/generated $^

java/generated/org/apache/mesos/scheduler/Protos.java: $(SCHEDULER_PROTO)
	$(MKDIR_P) $(@D)
	$(PROTOC) $(PROTOCFLAGS) --java_out=java/generated $^

java/generated/org/apache/mesos/v1/agent/Protos.java: $(V1_AGENT_PROTO)
	$(MKDIR_P) $(@D)
	$(PROTOC) $(PROTOCFLAGS) --java_out=java/generated $^

java/generated/org/apache/mesos/v1/allocator/Protos.java: $(V1_ALLOCATOR_PROTO)
	$(MKDIR_P) $(@D)
	$(PROTOC) $(PROTOCFLAGS) --java_out=java/generated $^

java/generated/org/apache/mesos/v1/executor/Protos.java: $(V1_EXECUTOR_PROTO)
	$(MKDIR_P) $(@D)
	$(PROTOC) $(PROTOCFLAGS) --java_out=java/generated $^

java/generated/org/apache/mesos/v1/maintenance/Protos.java: $(V1_MAINTENANCE_PROTO)
	$(MKDIR_P) $(@D)
	$(PROTOC) $(PROTOCFLAGS) --java_out=java/generated $^

java/generated/org/apache/mesos/v1/master/Protos.java: $(V1_MASTER_PROTO)
	$(MKDIR_P) $(@D)
	$(PROTOC) $(PROTOCFLAGS) --java_out=java/generated $^

java/generated/org/apache/mesos/v1/Protos.java: $(V1_MESOS_PROTO)
	$(MKDIR_P) $(@D)
	$(PROTOC) $(PROTOCFLAGS) --java_out=java/generated $^

java/generated/org/apache/mesos/v1/quota/Protos.java: $(V1_QUOTA_PROTO)
	$(MKDIR_P) $(@D)
	$(PROTOC) $(PROTOCFLAGS) --java_out=java/generated $^

java/generated/org/apache/mesos/v1/resource_provider/Protos.java: $(V1_RESOURCE_PROVIDER_PROTO)
	$(MKDIR_P) $(@D)
	$(PROTOC) $(PROTOCFLAGS) --java_out=java/generated $^

java/generated/org/apache/mesos/v1/scheduler/Protos.java: $(V1_SCHEDULER_PROTO)
	$(MKDIR_P) $(@D)
	$(PROTOC) $(PROTOCFLAGS) --java_out=java/generated $^


# Targets for generating Python protocol buffer code.
# NOTE: All python protocol buffer code that imports from mesos.proto
# has to get patched using sed, removing the leading 'mesos.'
# namespace qualifier due to the flat hierachy in the resulting
# mesos.interface egg. See MESOS-1750 for more.
python/interface/src/mesos/interface/executor_pb2.py: $(EXECUTOR_PROTO)
	$(MKDIR_P) $(@D)
	$(PROTOC) -I$(top_srcdir)/include/mesos/executor $(PROTOCFLAGS)		\
		--python_out=python/interface/src/mesos/interface $^
	$(SED) -i.bak 's/mesos\.mesos_pb2/mesos_pb2/' $@ && rm $@.bak

python/interface/src/mesos/interface/mesos_pb2.py: $(MESOS_PROTO)
	$(MKDIR_P) $(@D)
	$(PROTOC) -I$(top_srcdir)/include/mesos $(PROTOCFLAGS)			\
		--python_out=python/interface/src/mesos/interface $^

python/interface/src/mesos/interface/scheduler_pb2.py: $(SCHEDULER_PROTO)
	$(MKDIR_P) $(@D)
	$(PROTOC) -I$(top_srcdir)/include/mesos/scheduler $(PROTOCFLAGS)	\
		--python_out=python/interface/src/mesos/interface $^
	$(SED) -i.bak 's/mesos\.mesos_pb2/mesos_pb2/' $@ && rm $@.bak

python/interface/src/mesos/v1/interface/agent_pb2.py: $(V1_AGENT_PROTO)
	$(MKDIR_P) $(@D)
	$(PROTOC) -I$(top_srcdir)/include/mesos/v1/agent $(PROTOCFLAGS)		\
		--python_out=python/interface/src/mesos/v1/interface $^
	$(SED) -i.bak 's/mesos\.mesos_pb2/mesos_pb2/' $@ && rm $@.bak

python/interface/src/mesos/v1/interface/allocator_pb2.py: $(V1_ALLOCATOR_PROTO)
	$(MKDIR_P) $(@D)
	$(PROTOC) -I$(top_srcdir)/include/mesos/v1/allocator $(PROTOCFLAGS)	\
		--python_out=python/interface/src/mesos/v1/interface $^
	$(SED) -i.bak 's/mesos\.mesos_pb2/mesos_pb2/' $@ && rm $@.bak

python/interface/src/mesos/v1/interface/executor_pb2.py: $(V1_EXECUTOR_PROTO)
	$(MKDIR_P) $(@D)
	$(PROTOC) -I$(top_srcdir)/include/mesos/v1/executor $(PROTOCFLAGS)	\
		--python_out=python/interface/src/mesos/v1/interface $^
	$(SED) -i.bak 's/mesos\.mesos_pb2/mesos_pb2/' $@ && rm $@.bak

python/interface/src/mesos/v1/interface/maintenance_pb2.py: $(V1_MAINTENANCE_PROTO)
	$(MKDIR_P) $(@D)
	$(PROTOC) -I$(top_srcdir)/include/mesos/v1/maintenance $(PROTOCFLAGS)	\
		--python_out=python/interface/src/mesos/v1/interface $^
	$(SED) -i.bak 's/mesos\.mesos_pb2/mesos_pb2/' $@ && rm $@.bak

python/interface/src/mesos/v1/interface/master_pb2.py: $(V1_MASTER_PROTO)
	$(MKDIR_P) $(@D)
	$(PROTOC) -I$(top_srcdir)/include/mesos/v1/master $(PROTOCFLAGS)	\
		--python_out=python/interface/src/mesos/v1/interface $^
	$(SED) -i.bak 's/mesos\.mesos_pb2/mesos_pb2/' $@ && rm $@.bak

python/interface/src/mesos/v1/interface/mesos_pb2.py: $(V1_MESOS_PROTO)
	$(MKDIR_P) $(@D)
	$(PROTOC) -I$(top_srcdir)/include/mesos/v1 $(PROTOCFLAGS)		\
		--python_out=python/interface/src/mesos/v1/interface $^

python/interface/src/mesos/v1/interface/quota_pb2.py: $(V1_QUOTA_PROTO)
	$(MKDIR_P) $(@D)
	$(PROTOC) -I$(top_srcdir)/include/mesos/v1/quota $(PROTOCFLAGS)		\
		--python_out=python/interface/src/mesos/v1/interface $^
	$(SED) -i.bak 's/mesos\.mesos_pb2/mesos_pb2/' $@ && rm $@.bak

python/interface/src/mesos/v1/interface/resource_provider_pb2.py: $(V1_RESOURCE_PROVIDER_PROTO)
	$(MKDIR_P) $(@D)
	$(PROTOC) -I$(top_srcdir)/include/mesos/v1/resource_provider $(PROTOCFLAGS)	\
		--python_out=python/interface/src/mesos/v1/interface $^
	$(SED) -i.bak 's/mesos\.mesos_pb2/mesos_pb2/' $@ && rm $@.bak

python/interface/src/mesos/v1/interface/scheduler_pb2.py: $(V1_SCHEDULER_PROTO)
	$(MKDIR_P) $(@D)
	$(PROTOC) -I$(top_srcdir)/include/mesos/v1/scheduler $(PROTOCFLAGS)	\
		--python_out=python/interface/src/mesos/v1/interface $^
	$(SED) -i.bak 's/mesos\.mesos_pb2/mesos_pb2/' $@ && rm $@.bak


pkginclude_HEADERS =							\
  $(top_srcdir)/include/mesos/attributes.hpp				\
  $(top_srcdir)/include/mesos/executor.hpp				\
  $(top_srcdir)/include/mesos/hook.hpp					\
  $(top_srcdir)/include/mesos/http.hpp					\
  $(top_srcdir)/include/mesos/mesos.hpp					\
  $(top_srcdir)/include/mesos/mesos.proto				\
  $(top_srcdir)/include/mesos/module.hpp				\
  $(top_srcdir)/include/mesos/resources.hpp				\
  $(top_srcdir)/include/mesos/resource_quantities.hpp				\
  $(top_srcdir)/include/mesos/roles.hpp					\
  $(top_srcdir)/include/mesos/scheduler.hpp				\
  $(top_srcdir)/include/mesos/type_utils.hpp				\
  $(top_srcdir)/include/mesos/values.hpp

nodist_pkginclude_HEADERS =						\
  ../include/mesos/version.hpp						\
  ../include/mesos/mesos.pb.h

allocatordir = $(pkgincludedir)/allocator

allocator_HEADERS =								\
  $(top_srcdir)/include/mesos/allocator/allocator.hpp				\
  $(top_srcdir)/include/mesos/allocator/allocator.proto

nodist_allocator_HEADERS =							\
  ../include/mesos/allocator/allocator.pb.h

appcdir = $(pkgincludedir)/appc

appc_HEADERS =								\
  $(top_srcdir)/include/mesos/appc/spec.hpp				\
  $(top_srcdir)/include/mesos/appc/spec.proto

nodist_appc_HEADERS =							\
  ../include/mesos/appc/spec.pb.h

authenticationdir = $(pkgincludedir)/authentication

authentication_HEADERS =						\
  $(top_srcdir)/include/mesos/authentication/authenticatee.hpp		\
  $(top_srcdir)/include/mesos/authentication/authentication.hpp		\
  $(top_srcdir)/include/mesos/authentication/authentication.proto	\
  $(top_srcdir)/include/mesos/authentication/authenticator.hpp		\
  $(top_srcdir)/include/mesos/authentication/secret_generator.hpp

nodist_authentication_HEADERS =						\
  ../include/mesos/authentication/authentication.pb.h

httpauthenticationdir = $(pkgincludedir)/authentication/http

httpauthentication_HEADERS =						\
  $(top_srcdir)/include/mesos/authentication/http/authenticatee.hpp	\
  $(top_srcdir)/include/mesos/authentication/http/basic_authenticator_factory.hpp	\
  $(top_srcdir)/include/mesos/authentication/http/combined_authenticator.hpp

authorizerdir = $(pkgincludedir)/authorizer

authorizer_HEADERS =							\
  $(top_srcdir)/include/mesos/authorizer/acls.hpp			\
  $(top_srcdir)/include/mesos/authorizer/acls.proto			\
  $(top_srcdir)/include/mesos/authorizer/authorizer.hpp			\
  $(top_srcdir)/include/mesos/authorizer/authorizer.proto

nodist_authorizer_HEADERS =						\
  ../include/mesos/authorizer/acls.pb.h					\
  ../include/mesos/authorizer/authorizer.pb.h

csidir = $(pkgincludedir)/csi

csi_HEADERS =								\
  $(top_srcdir)/include/mesos/csi/v0.hpp				\
  $(top_srcdir)/include/mesos/csi/v1.hpp

# Install the generated CSI headers into $PREFIX/include.
csi_v0dir = $(includedir)/csi/v0

nodist_csi_v0_HEADERS =							\
  ../include/csi/v0/csi.grpc.pb.h					\
  ../include/csi/v0/csi.pb.h

csi_v1dir = $(includedir)/csi/v1

nodist_csi_v1_HEADERS =							\
  ../include/csi/v1/csi.grpc.pb.h					\
  ../include/csi/v1/csi.pb.h

dockerdir = $(pkgincludedir)/docker

docker_HEADERS =							\
  $(top_srcdir)/include/mesos/docker/spec.hpp				\
  $(top_srcdir)/include/mesos/docker/spec.proto				\
  $(top_srcdir)/include/mesos/docker/v1.hpp				\
  $(top_srcdir)/include/mesos/docker/v1.proto				\
  $(top_srcdir)/include/mesos/docker/v2.hpp				\
  $(top_srcdir)/include/mesos/docker/v2.proto       \
  $(top_srcdir)/include/mesos/docker/v2_2.hpp				\
  $(top_srcdir)/include/mesos/docker/v2_2.proto

nodist_docker_HEADERS =							\
  ../include/mesos/docker/spec.pb.h					\
  ../include/mesos/docker/v1.pb.h					\
  ../include/mesos/docker/v2.pb.h         \
  ../include/mesos/docker/v2_2.pb.h

executordir = $(pkgincludedir)/executor

executor_HEADERS =							\
  $(top_srcdir)/include/mesos/executor/executor.hpp			\
  $(top_srcdir)/include/mesos/executor/executor.proto

nodist_executor_HEADERS =						\
  ../include/mesos/executor/executor.pb.h

fetcherdir = $(pkgincludedir)/fetcher

fetcher_HEADERS =							\
  $(top_srcdir)/include/mesos/fetcher/fetcher.hpp			\
  $(top_srcdir)/include/mesos/fetcher/fetcher.proto

nodist_fetcher_HEADERS =						\
  ../include/mesos/fetcher/fetcher.pb.h

logdir = $(pkgincludedir)/log

log_HEADERS =								\
  $(top_srcdir)/include/mesos/log/log.hpp

maintenancedir = $(pkgincludedir)/maintenance

maintenance_HEADERS =							\
  $(top_srcdir)/include/mesos/maintenance/maintenance.hpp		\
  $(top_srcdir)/include/mesos/maintenance/maintenance.proto

nodist_maintenance_HEADERS =						\
  ../include/mesos/maintenance/maintenance.pb.h

masterdir = $(pkgincludedir)/master

master_HEADERS =							\
  $(top_srcdir)/include/mesos/master/contender.hpp			\
  $(top_srcdir)/include/mesos/master/detector.hpp			\
  $(top_srcdir)/include/mesos/master/master.hpp				\
  $(top_srcdir)/include/mesos/master/master.proto

nodist_master_HEADERS =							\
  ../include/mesos/master/master.pb.h

moduledir = $(pkgincludedir)/module

module_HEADERS =							\
  $(top_srcdir)/include/mesos/module/allocator.hpp			\
  $(top_srcdir)/include/mesos/module/anonymous.hpp			\
  $(top_srcdir)/include/mesos/module/authenticatee.hpp			\
  $(top_srcdir)/include/mesos/module/authenticator.hpp			\
  $(top_srcdir)/include/mesos/module/authorizer.hpp			\
  $(top_srcdir)/include/mesos/module/container_logger.hpp		\
  $(top_srcdir)/include/mesos/module/contender.hpp			\
  $(top_srcdir)/include/mesos/module/detector.hpp			\
  $(top_srcdir)/include/mesos/module/disk_profile_adaptor.hpp		\
  $(top_srcdir)/include/mesos/module/hook.hpp				\
  $(top_srcdir)/include/mesos/module/hook.proto				\
  $(top_srcdir)/include/mesos/module/http_authenticatee.hpp		\
  $(top_srcdir)/include/mesos/module/http_authenticator.hpp		\
  $(top_srcdir)/include/mesos/module/isolator.hpp			\
  $(top_srcdir)/include/mesos/module/module.hpp				\
  $(top_srcdir)/include/mesos/module/module.proto			\
  $(top_srcdir)/include/mesos/module/qos_controller.hpp			\
  $(top_srcdir)/include/mesos/module/resource_estimator.hpp		\
  $(top_srcdir)/include/mesos/module/secret_generator.hpp		\
  $(top_srcdir)/include/mesos/module/secret_resolver.hpp

nodist_module_HEADERS =							\
  ../include/mesos/module/hook.pb.h					\
  ../include/mesos/module/module.pb.h

ocidir = $(pkgincludedir)/oci

oci_HEADERS =								\
  $(top_srcdir)/include/mesos/oci/spec.hpp				\
  $(top_srcdir)/include/mesos/oci/spec.proto

nodist_oci_HEADERS =							\
  ../include/mesos/oci/spec.pb.h

quotadir = $(pkgincludedir)/quota

quota_HEADERS =								\
  $(top_srcdir)/include/mesos/quota/quota.hpp				\
  $(top_srcdir)/include/mesos/quota/quota.proto

nodist_quota_HEADERS =							\
  ../include/mesos/quota/quota.pb.h

resourceproviderdir = $(pkgincludedir)/resource_provider

resourceprovider_HEADERS =							\
  $(top_srcdir)/include/mesos/resource_provider/resource_provider.hpp		\
  $(top_srcdir)/include/mesos/resource_provider/resource_provider.proto

nodist_resourceprovider_HEADERS =					\
  ../include/mesos/resource_provider/resource_provider.pb.h

resourceprovider_storagedir = $(pkgincludedir)/resource_provider/storage

resourceprovider_storage_HEADERS =						\
  $(top_srcdir)/include/mesos/resource_provider/storage/disk_profile_adaptor.hpp

schedulerdir = $(pkgincludedir)/scheduler

scheduler_HEADERS =							\
  $(top_srcdir)/include/mesos/scheduler/scheduler.hpp			\
  $(top_srcdir)/include/mesos/scheduler/scheduler.proto

nodist_scheduler_HEADERS =						\
  ../include/mesos/scheduler/scheduler.pb.h

seccompdir = $(pkgincludedir)/seccomp

seccomp_HEADERS =							\
  $(top_srcdir)/include/mesos/seccomp/seccomp.hpp			\
  $(top_srcdir)/include/mesos/seccomp/seccomp.proto

nodist_seccomp_HEADERS =						\
  ../include/mesos/seccomp/seccomp.pb.h

agentdir = $(pkgincludedir)/agent

agent_HEADERS =								\
  $(top_srcdir)/include/mesos/agent/agent.hpp				\
  $(top_srcdir)/include/mesos/agent/agent.proto				\
  $(top_srcdir)/include/mesos/slave/container_logger.hpp		\
  $(top_srcdir)/include/mesos/slave/containerizer.hpp			\
  $(top_srcdir)/include/mesos/slave/isolator.hpp			\
  $(top_srcdir)/include/mesos/slave/oversubscription.hpp		\
  $(top_srcdir)/include/mesos/slave/oversubscription.proto		\
  $(top_srcdir)/include/mesos/slave/qos_controller.hpp			\
  $(top_srcdir)/include/mesos/slave/resource_estimator.hpp

nodist_agent_HEADERS =							\
  ../include/mesos/agent/agent.pb.h					\
  ../include/mesos/slave/containerizer.pb.h				\
  ../include/mesos/slave/oversubscription.pb.h

secretdir = $(pkgincludedir)/secret

secret_HEADERS =							\
  $(top_srcdir)/include/mesos/secret/resolver.hpp

statedir = $(pkgincludedir)/state

state_HEADERS =								\
  $(top_srcdir)/include/mesos/state/in_memory.hpp			\
  $(top_srcdir)/include/mesos/state/leveldb.hpp				\
  $(top_srcdir)/include/mesos/state/log.hpp				\
  $(top_srcdir)/include/mesos/state/protobuf.hpp			\
  $(top_srcdir)/include/mesos/state/state.hpp				\
  $(top_srcdir)/include/mesos/state/state.proto				\
  $(top_srcdir)/include/mesos/state/storage.hpp				\
  $(top_srcdir)/include/mesos/state/zookeeper.hpp

nodist_state_HEADERS =							\
  ../include/mesos/state/state.pb.h

uridir = $(pkgincludedir)/uri

uri_HEADERS =								\
  $(top_srcdir)/include/mesos/uri/fetcher.hpp				\
  $(top_srcdir)/include/mesos/uri/uri.hpp				\
  $(top_srcdir)/include/mesos/uri/uri.proto

nodist_uri_HEADERS =							\
  ../include/mesos/uri/uri.pb.h

v1dir = $(pkgincludedir)/v1

v1_HEADERS =								\
  $(top_srcdir)/include/mesos/v1/attributes.hpp				\
  $(top_srcdir)/include/mesos/v1/executor.hpp				\
  $(top_srcdir)/include/mesos/v1/mesos.hpp				\
  $(top_srcdir)/include/mesos/v1/mesos.proto				\
  $(top_srcdir)/include/mesos/v1/resources.hpp				\
  $(top_srcdir)/include/mesos/v1/resource_provider.hpp			\
  $(top_srcdir)/include/mesos/v1/scheduler.hpp				\
  $(top_srcdir)/include/mesos/v1/values.hpp

nodist_v1_HEADERS =							\
  ../include/mesos/v1/mesos.pb.h

v1agentdir = $(pkgincludedir)/v1/agent

v1agent_HEADERS =							\
  $(top_srcdir)/include/mesos/v1/agent/agent.hpp			\
  $(top_srcdir)/include/mesos/v1/agent/agent.proto

nodist_v1agent_HEADERS =						\
  ../include/mesos/v1/agent/agent.pb.h

v1allocatordir = $(pkgincludedir)/v1/allocator

v1allocator_HEADERS =							\
  $(top_srcdir)/include/mesos/v1/allocator/allocator.proto

nodist_v1allocator_HEADERS =						\
  ../include/mesos/v1/allocator/allocator.pb.h

v1executordir = $(pkgincludedir)/v1/executor

v1executor_HEADERS =							\
  $(top_srcdir)/include/mesos/v1/executor/executor.hpp			\
  $(top_srcdir)/include/mesos/v1/executor/executor.proto

nodist_v1executor_HEADERS =						\
  ../include/mesos/v1/executor/executor.pb.h

v1maintenancedir = $(pkgincludedir)/v1/maintenance

v1maintenance_HEADERS =							\
  $(top_srcdir)/include/mesos/v1/maintenance/maintenance.hpp		\
  $(top_srcdir)/include/mesos/v1/maintenance/maintenance.proto

nodist_v1maintenance_HEADERS =						\
  ../include/mesos/v1/maintenance/maintenance.pb.h

v1masterdir = $(pkgincludedir)/v1/master

v1master_HEADERS =							\
  $(top_srcdir)/include/mesos/v1/master/master.hpp			\
  $(top_srcdir)/include/mesos/v1/master/master.proto

nodist_v1master_HEADERS =						\
  ../include/mesos/v1/master/master.pb.h

v1quotadir = $(pkgincludedir)/v1/quota

v1quota_HEADERS =							\
  $(top_srcdir)/include/mesos/v1/quota/quota.proto

nodist_v1quota_HEADERS =						\
  ../include/mesos/v1/quota/quota.pb.h

v1resourceproviderdir = $(pkgincludedir)/v1/resource_provider

v1resourceprovider_HEADERS =							\
  $(top_srcdir)/include/mesos/v1/resource_provider/resource_provider.hpp	\
  $(top_srcdir)/include/mesos/v1/resource_provider/resource_provider.proto

nodist_v1resourceprovider_HEADERS =						\
  ../include/mesos/v1/resource_provider/resource_provider.pb.h

v1schedulerdir = $(pkgincludedir)/v1/scheduler

v1scheduler_HEADERS =							\
  $(top_srcdir)/include/mesos/v1/scheduler/scheduler.hpp		\
  $(top_srcdir)/include/mesos/v1/scheduler/scheduler.proto

nodist_v1scheduler_HEADERS =						\
  ../include/mesos/v1/scheduler/scheduler.pb.h

zookeeperdir = $(pkgincludedir)/zookeeper

zookeeper_HEADERS =							\
  $(top_srcdir)/include/mesos/zookeeper/authentication.hpp		\
  $(top_srcdir)/include/mesos/zookeeper/contender.hpp			\
  $(top_srcdir)/include/mesos/zookeeper/detector.hpp			\
  $(top_srcdir)/include/mesos/zookeeper/group.hpp			\
  $(top_srcdir)/include/mesos/zookeeper/url.hpp				\
  $(top_srcdir)/include/mesos/zookeeper/watcher.hpp			\
  $(top_srcdir)/include/mesos/zookeeper/zookeeper.hpp


# We even use a convenience library for most of Mesos so that we can
# exclude third party libraries so setuptools can build a
# self-contained Python library and statically link in the third party
# libraries themselves.
noinst_LTLIBRARIES += libmesos_no_3rdparty.la

nodist_libmesos_no_3rdparty_la_SOURCES = $(CXX_PROTOS)


libmesos_no_3rdparty_la_SOURCES =					\
  master/registry.proto							\
  messages/flags.proto							\
  messages/messages.proto						\
  resource_provider/registry.proto					\
  resource_provider/state.proto						\
  slave/state.proto							\
  slave/containerizer/mesos/provisioner/docker/message.proto		\
  slave/containerizer/mesos/isolators/docker/volume/state.proto		\
  slave/containerizer/mesos/isolators/network/cni/spec.proto		\
  slave/containerizer/mesos/isolators/volume/csi/state.proto		\
  slave/volume_gid_manager/state.proto

# TODO(tillt): Remove authentication/cram_md5/* which will enable us to
# lose the immediate cyrus-sasl2 dependency.
libmesos_no_3rdparty_la_SOURCES +=					\
  appc/spec.cpp								\
  authentication/cram_md5/authenticatee.cpp				\
  authentication/cram_md5/authenticatee.hpp				\
  authentication/cram_md5/authenticator.cpp				\
  authentication/cram_md5/authenticator.hpp				\
  authentication/cram_md5/auxprop.cpp					\
  authentication/cram_md5/auxprop.hpp					\
  authentication/http/basic_authenticatee.cpp				\
  authentication/http/basic_authenticatee.hpp				\
  authentication/http/basic_authenticator_factory.cpp			\
  authentication/http/combined_authenticator.cpp			\
  authorizer/acls.cpp							\
  authorizer/authorizer.cpp						\
  authorizer/local/authorizer.cpp					\
  authorizer/local/authorizer.hpp					\
  checks/checker.cpp							\
  checks/checker.hpp							\
  checks/checker_process.cpp						\
  checks/checker_process.hpp						\
  checks/checks_runtime.hpp						\
  checks/checks_types.hpp						\
  checks/health_checker.cpp						\
  checks/health_checker.hpp						\
  common/attributes.cpp							\
  common/authorization.cpp						\
  common/authorization.hpp						\
  common/build.hpp							\
  common/command_utils.cpp						\
  common/command_utils.hpp						\
  common/domain_sockets.hpp						\
  common/future_tracker.hpp						\
  common/heartbeater.hpp						\
  common/http.cpp							\
  common/http.hpp							\
  common/kernel_version.cpp						\
  common/kernel_version.hpp						\
  common/parse.hpp							\
  common/protobuf_utils.cpp						\
  common/protobuf_utils.hpp						\
  common/recordio.hpp							\
  common/resources.cpp							\
  common/resource_quantities.cpp					\
  common/resources_utils.cpp						\
  common/resources_utils.hpp						\
  common/roles.cpp							\
  common/status_utils.hpp						\
  common/type_utils_differencers.hpp					\
  common/type_utils.cpp							\
  common/validation.cpp							\
  common/validation.hpp							\
  common/values.cpp							\
  common/values.hpp							\
  credentials/credentials.hpp						\
  docker/docker.cpp							\
  docker/docker.hpp							\
  docker/executor.hpp							\
  docker/executor.cpp							\
  docker/spec.cpp							\
  examples/flags.hpp							\
  examples/test_anonymous_module.hpp					\
  examples/test_module.hpp						\
  examples/utils.hpp							\
  exec/exec.cpp								\
  executor/executor.cpp							\
  executor/v0_v1executor.cpp						\
  executor/v0_v1executor.hpp						\
  files/files.cpp							\
  files/files.hpp							\
  hdfs/hdfs.cpp								\
  hdfs/hdfs.hpp								\
  hook/manager.cpp							\
  hook/manager.hpp							\
  internal/devolve.cpp							\
  internal/devolve.hpp							\
  internal/evolve.cpp							\
  internal/evolve.hpp							\
  local/flags.hpp							\
  local/local.cpp							\
  local/local.hpp							\
  logging/flags.cpp							\
  logging/flags.hpp							\
  logging/logging.cpp							\
  logging/logging.hpp							\
  master/allocator/allocator.cpp					\
  master/allocator/mesos/allocator.hpp					\
  master/allocator/mesos/hierarchical.cpp				\
  master/allocator/mesos/hierarchical.hpp				\
  master/allocator/mesos/metrics.cpp					\
  master/allocator/mesos/metrics.hpp					\
  master/allocator/mesos/offer_constraints_filter.cpp			\
  master/allocator/mesos/sorter/drf/metrics.cpp				\
  master/allocator/mesos/sorter/drf/metrics.hpp				\
  master/allocator/mesos/sorter/drf/sorter.cpp				\
  master/allocator/mesos/sorter/drf/sorter.hpp				\
  master/allocator/mesos/sorter/random/sorter.cpp				\
  master/allocator/mesos/sorter/random/sorter.hpp				\
  master/allocator/mesos/sorter/random/utils.hpp				\
  master/allocator/mesos/sorter/sorter.hpp					\
  master/authorization.cpp						\
  master/authorization.hpp						\
  master/constants.cpp							\
  master/constants.hpp							\
  master/contender/contender.cpp					\
  master/contender/standalone.cpp					\
  master/contender/standalone.hpp					\
  master/contender/zookeeper.cpp					\
  master/contender/zookeeper.hpp					\
  master/detector/detector.cpp						\
  master/detector/standalone.cpp					\
  master/detector/standalone.hpp					\
  master/detector/zookeeper.cpp						\
  master/detector/zookeeper.hpp						\
  master/flags.cpp							\
  master/flags.hpp							\
  master/framework.cpp							\
  master/http.cpp							\
  master/machine.hpp							\
  master/maintenance.cpp						\
  master/maintenance.hpp						\
  master/master.cpp							\
  master/master.hpp							\
  master/metrics.cpp							\
  master/metrics.hpp							\
  master/quota.cpp							\
  master/quota.hpp							\
  master/quota_handler.cpp						\
  master/readonly_handler.cpp						\
  master/registrar.cpp							\
  master/registrar.hpp							\
  master/registry.hpp							\
  master/registry_operations.cpp					\
  master/registry_operations.hpp					\
  master/validation.cpp							\
  master/validation.hpp							\
  master/weights.cpp							\
  master/weights.hpp							\
  master/weights_handler.cpp						\
  messages/flags.hpp							\
  messages/messages.cpp							\
  messages/messages.hpp							\
  module/manager.cpp							\
  module/manager.hpp							\
  oci/spec.cpp								\
  posix/rlimits.cpp							\
  posix/rlimits.hpp							\
  resource_provider/constants.hpp					\
  resource_provider/daemon.cpp						\
  resource_provider/daemon.hpp						\
  resource_provider/detector.cpp					\
  resource_provider/detector.hpp					\
  resource_provider/driver.cpp						\
  resource_provider/http_connection.hpp					\
  resource_provider/local.cpp						\
  resource_provider/local.hpp						\
  resource_provider/manager.cpp						\
  resource_provider/manager.hpp						\
  resource_provider/message.hpp						\
  resource_provider/registrar.cpp					\
  resource_provider/registrar.hpp					\
  resource_provider/registry.hpp					\
  resource_provider/state.hpp						\
  resource_provider/validation.cpp					\
  resource_provider/validation.hpp					\
  sched/constants.hpp							\
  sched/flags.hpp							\
  sched/sched.cpp							\
  scheduler/constants.hpp						\
  scheduler/flags.hpp							\
  scheduler/scheduler.cpp						\
  secret/resolver.cpp							\
  slave/compatibility.cpp						\
  slave/compatibility.hpp						\
  slave/constants.cpp							\
  slave/constants.hpp							\
  slave/container_daemon.cpp						\
  slave/container_daemon.hpp						\
  slave/container_daemon_process.hpp					\
  slave/container_logger.cpp						\
  slave/csi_server.cpp							\
  slave/csi_server.hpp							\
  slave/container_loggers/sandbox.cpp					\
  slave/container_loggers/sandbox.hpp					\
  slave/containerizer/composing.cpp					\
  slave/containerizer/composing.hpp					\
  slave/containerizer/containerizer.cpp					\
  slave/containerizer/containerizer.hpp					\
  slave/containerizer/docker.cpp					\
  slave/containerizer/docker.hpp					\
  slave/containerizer/fetcher.cpp					\
  slave/containerizer/fetcher.hpp					\
  slave/containerizer/fetcher_process.hpp				\
  slave/containerizer/mesos/constants.hpp				\
  slave/containerizer/mesos/containerizer.cpp				\
  slave/containerizer/mesos/containerizer.hpp				\
  slave/containerizer/mesos/io/switchboard.cpp				\
  slave/containerizer/mesos/io/switchboard.hpp				\
  slave/containerizer/mesos/isolator.cpp				\
  slave/containerizer/mesos/isolator.hpp				\
  slave/containerizer/mesos/isolator_tracker.cpp			\
  slave/containerizer/mesos/isolator_tracker.hpp			\
  slave/containerizer/mesos/isolators/docker/volume/driver.cpp		\
  slave/containerizer/mesos/isolators/docker/volume/driver.hpp		\
  slave/containerizer/mesos/isolators/docker/volume/paths.cpp		\
  slave/containerizer/mesos/isolators/docker/volume/paths.hpp		\
  slave/containerizer/mesos/isolators/docker/volume/state.hpp		\
  slave/containerizer/mesos/isolators/environment_secret.cpp		\
  slave/containerizer/mesos/isolators/environment_secret.hpp		\
  slave/containerizer/mesos/isolators/filesystem/posix.cpp		\
  slave/containerizer/mesos/isolators/filesystem/posix.hpp		\
  slave/containerizer/mesos/isolators/filesystem/windows.hpp		\
  slave/containerizer/mesos/isolators/network/cni/paths.cpp		\
  slave/containerizer/mesos/isolators/network/cni/paths.hpp		\
  slave/containerizer/mesos/isolators/network/cni/spec.cpp		\
  slave/containerizer/mesos/isolators/network/cni/spec.hpp		\
  slave/containerizer/mesos/isolators/posix.hpp				\
  slave/containerizer/mesos/isolators/posix/disk.cpp			\
  slave/containerizer/mesos/isolators/posix/disk.hpp			\
  slave/containerizer/mesos/isolators/posix/rlimits.cpp			\
  slave/containerizer/mesos/isolators/posix/rlimits.hpp			\
  slave/containerizer/mesos/isolators/volume/sandbox_path.cpp		\
  slave/containerizer/mesos/isolators/volume/sandbox_path.hpp		\
  slave/containerizer/mesos/isolators/volume/csi/paths.cpp		\
  slave/containerizer/mesos/isolators/volume/csi/paths.hpp		\
  slave/containerizer/mesos/isolators/volume/csi/state.hpp		\
  slave/containerizer/mesos/isolators/windows/cpu.hpp			\
  slave/containerizer/mesos/isolators/windows/mem.hpp			\
  slave/containerizer/mesos/launch.cpp					\
  slave/containerizer/mesos/launch.hpp					\
  slave/containerizer/mesos/launcher.cpp				\
  slave/containerizer/mesos/launcher.hpp				\
  slave/containerizer/mesos/launcher_tracker.cpp			\
  slave/containerizer/mesos/launcher_tracker.hpp			\
  slave/containerizer/mesos/mount.cpp					\
  slave/containerizer/mesos/mount.hpp					\
  slave/containerizer/mesos/paths.cpp					\
  slave/containerizer/mesos/paths.hpp					\
  slave/containerizer/mesos/provisioner/appc/cache.cpp			\
  slave/containerizer/mesos/provisioner/appc/cache.hpp			\
  slave/containerizer/mesos/provisioner/appc/fetcher.cpp		\
  slave/containerizer/mesos/provisioner/appc/fetcher.hpp		\
  slave/containerizer/mesos/provisioner/appc/paths.cpp			\
  slave/containerizer/mesos/provisioner/appc/paths.hpp			\
  slave/containerizer/mesos/provisioner/appc/store.cpp			\
  slave/containerizer/mesos/provisioner/appc/store.hpp			\
  slave/containerizer/mesos/provisioner/backend.cpp			\
  slave/containerizer/mesos/provisioner/backend.hpp			\
  slave/containerizer/mesos/provisioner/backends/copy.cpp		\
  slave/containerizer/mesos/provisioner/backends/copy.hpp		\
  slave/containerizer/mesos/provisioner/constants.hpp			\
  slave/containerizer/mesos/provisioner/docker/image_tar_puller.cpp	\
  slave/containerizer/mesos/provisioner/docker/image_tar_puller.hpp	\
  slave/containerizer/mesos/provisioner/docker/message.hpp		\
  slave/containerizer/mesos/provisioner/docker/metadata_manager.cpp	\
  slave/containerizer/mesos/provisioner/docker/metadata_manager.hpp	\
  slave/containerizer/mesos/provisioner/docker/paths.cpp		\
  slave/containerizer/mesos/provisioner/docker/paths.hpp		\
  slave/containerizer/mesos/provisioner/docker/puller.cpp		\
  slave/containerizer/mesos/provisioner/docker/puller.hpp		\
  slave/containerizer/mesos/provisioner/docker/registry_puller.cpp	\
  slave/containerizer/mesos/provisioner/docker/registry_puller.hpp	\
  slave/containerizer/mesos/provisioner/docker/store.cpp		\
  slave/containerizer/mesos/provisioner/docker/store.hpp		\
  slave/containerizer/mesos/provisioner/paths.cpp			\
  slave/containerizer/mesos/provisioner/paths.hpp			\
  slave/containerizer/mesos/provisioner/provisioner.cpp			\
  slave/containerizer/mesos/provisioner/provisioner.hpp			\
  slave/containerizer/mesos/provisioner/store.cpp			\
  slave/containerizer/mesos/provisioner/store.hpp			\
  slave/containerizer/mesos/provisioner/utils.cpp			\
  slave/containerizer/mesos/provisioner/utils.hpp			\
  slave/containerizer/mesos/utils.cpp					\
  slave/containerizer/mesos/utils.hpp					\
  slave/flags.cpp							\
  slave/flags.hpp							\
  slave/gc.cpp								\
  slave/gc.hpp								\
  slave/gc_process.hpp							\
  slave/http.cpp							\
  slave/http.hpp							\
  slave/metrics.cpp							\
  slave/metrics.hpp							\
  slave/paths.cpp							\
  slave/paths.hpp							\
  slave/posix_signalhandler.hpp						\
  slave/qos_controller.cpp						\
  slave/qos_controllers/noop.cpp					\
  slave/qos_controllers/noop.hpp					\
  slave/resource_estimator.cpp						\
  slave/resource_estimators/noop.cpp					\
  slave/resource_estimators/noop.hpp					\
  slave/slave.cpp							\
  slave/slave.hpp							\
  slave/state.cpp							\
  slave/state.hpp							\
  slave/task_status_update_manager.cpp					\
  slave/task_status_update_manager.hpp					\
  slave/validation.cpp							\
  slave/validation.hpp							\
  slave/volume_gid_manager/state.hpp					\
  slave/volume_gid_manager/volume_gid_manager.cpp			\
  slave/volume_gid_manager/volume_gid_manager.hpp			\
  slave/windows_ctrlhandler.hpp						\
  status_update_manager/operation.cpp					\
  status_update_manager/operation.hpp					\
  status_update_manager/status_update_manager_process.hpp		\
  uri/fetcher.cpp							\
  uri/fetcher.hpp							\
  uri/fetchers/copy.cpp							\
  uri/fetchers/copy.hpp							\
  uri/fetchers/curl.cpp							\
  uri/fetchers/curl.hpp							\
  uri/fetchers/docker.cpp						\
  uri/fetchers/docker.hpp						\
  uri/fetchers/hadoop.cpp						\
  uri/fetchers/hadoop.hpp						\
  uri/schemes/docker.hpp						\
  uri/schemes/file.hpp							\
  uri/schemes/hdfs.hpp							\
  uri/schemes/http.hpp							\
  uri/utils.cpp								\
  uri/utils.hpp								\
  usage/usage.cpp							\
  usage/usage.hpp							\
  v1/attributes.cpp							\
  v1/mesos.cpp								\
  v1/parse.hpp								\
  v1/resources.cpp							\
  v1/values.cpp								\
  version/version.cpp							\
  version/version.hpp							\
  watcher/whitelist_watcher.cpp						\
  watcher/whitelist_watcher.hpp						\
  zookeeper/authentication.cpp						\
  zookeeper/contender.cpp						\
  zookeeper/detector.cpp						\
  zookeeper/group.cpp							\
  zookeeper/zookeeper.cpp

MESOS_LINUX_FILES =									\
  linux/capabilities.cpp								\
  linux/capabilities.hpp								\
  linux/cgroups.cpp									\
  linux/cgroups.hpp									\
  linux/fs.cpp										\
  linux/fs.hpp										\
  linux/ldcache.cpp									\
  linux/ldcache.hpp									\
  linux/ldd.cpp										\
  linux/ldd.hpp										\
  linux/ns.cpp										\
  linux/ns.hpp										\
  linux/perf.cpp									\
  linux/perf.hpp									\
  linux/sched.hpp									\
  linux/systemd.cpp									\
  linux/systemd.hpp									\
  slave/containerizer/mesos/linux_launcher.cpp						\
  slave/containerizer/mesos/linux_launcher.hpp						\
  slave/containerizer/mesos/isolators/appc/runtime.cpp					\
  slave/containerizer/mesos/isolators/appc/runtime.hpp					\
  slave/containerizer/mesos/isolators/cgroups/cgroups.cpp				\
  slave/containerizer/mesos/isolators/cgroups/cgroups.hpp				\
  slave/containerizer/mesos/isolators/cgroups/constants.hpp				\
  slave/containerizer/mesos/isolators/cgroups/subsystem.cpp				\
  slave/containerizer/mesos/isolators/cgroups/subsystem.hpp				\
  slave/containerizer/mesos/isolators/cgroups/subsystems/blkio.cpp			\
  slave/containerizer/mesos/isolators/cgroups/subsystems/blkio.hpp			\
  slave/containerizer/mesos/isolators/cgroups/subsystems/cpu.cpp			\
  slave/containerizer/mesos/isolators/cgroups/subsystems/cpu.hpp			\
  slave/containerizer/mesos/isolators/cgroups/subsystems/cpuacct.cpp			\
  slave/containerizer/mesos/isolators/cgroups/subsystems/cpuacct.hpp			\
  slave/containerizer/mesos/isolators/cgroups/subsystems/cpuset.cpp			\
  slave/containerizer/mesos/isolators/cgroups/subsystems/cpuset.hpp			\
  slave/containerizer/mesos/isolators/cgroups/subsystems/devices.cpp			\
  slave/containerizer/mesos/isolators/cgroups/subsystems/devices.hpp			\
  slave/containerizer/mesos/isolators/cgroups/subsystems/hugetlb.cpp			\
  slave/containerizer/mesos/isolators/cgroups/subsystems/hugetlb.hpp			\
  slave/containerizer/mesos/isolators/cgroups/subsystems/memory.cpp			\
  slave/containerizer/mesos/isolators/cgroups/subsystems/memory.hpp			\
  slave/containerizer/mesos/isolators/cgroups/subsystems/net_cls.cpp			\
  slave/containerizer/mesos/isolators/cgroups/subsystems/net_cls.hpp			\
  slave/containerizer/mesos/isolators/cgroups/subsystems/net_prio.cpp			\
  slave/containerizer/mesos/isolators/cgroups/subsystems/net_prio.hpp			\
  slave/containerizer/mesos/isolators/cgroups/subsystems/perf_event.cpp			\
  slave/containerizer/mesos/isolators/cgroups/subsystems/perf_event.hpp			\
  slave/containerizer/mesos/isolators/cgroups/subsystems/pids.cpp			\
  slave/containerizer/mesos/isolators/cgroups/subsystems/pids.hpp			\
  slave/containerizer/mesos/isolators/docker/runtime.cpp				\
  slave/containerizer/mesos/isolators/docker/runtime.hpp				\
  slave/containerizer/mesos/isolators/docker/volume/isolator.cpp			\
  slave/containerizer/mesos/isolators/docker/volume/isolator.hpp			\
  slave/containerizer/mesos/isolators/filesystem/linux.cpp				\
  slave/containerizer/mesos/isolators/filesystem/linux.hpp				\
  slave/containerizer/mesos/isolators/filesystem/shared.cpp				\
  slave/containerizer/mesos/isolators/filesystem/shared.hpp				\
  slave/containerizer/mesos/isolators/gpu/allocator.cpp					\
  slave/containerizer/mesos/isolators/gpu/allocator.hpp					\
  slave/containerizer/mesos/isolators/gpu/components.hpp				\
  slave/containerizer/mesos/isolators/gpu/isolator.cpp					\
  slave/containerizer/mesos/isolators/gpu/isolator.hpp					\
  slave/containerizer/mesos/isolators/gpu/nvidia.hpp					\
  slave/containerizer/mesos/isolators/gpu/nvml.cpp					\
  slave/containerizer/mesos/isolators/gpu/nvml.hpp					\
  slave/containerizer/mesos/isolators/gpu/volume.cpp					\
  slave/containerizer/mesos/isolators/gpu/volume.hpp					\
  slave/containerizer/mesos/isolators/linux/capabilities.cpp				\
  slave/containerizer/mesos/isolators/linux/capabilities.hpp				\
  slave/containerizer/mesos/isolators/linux/devices.cpp					\
  slave/containerizer/mesos/isolators/linux/devices.hpp					\
  slave/containerizer/mesos/isolators/linux/nnp.hpp					\
  slave/containerizer/mesos/isolators/linux/nnp.cpp					\
  slave/containerizer/mesos/isolators/namespaces/ipc.cpp				\
  slave/containerizer/mesos/isolators/namespaces/ipc.hpp				\
  slave/containerizer/mesos/isolators/namespaces/pid.cpp				\
  slave/containerizer/mesos/isolators/namespaces/pid.hpp				\
  slave/containerizer/mesos/isolators/network/cni/cni.cpp				\
  slave/containerizer/mesos/isolators/network/cni/cni.hpp				\
  slave/containerizer/mesos/isolators/network/cni/plugins/port_mapper/port_mapper.cpp	\
  slave/containerizer/mesos/isolators/network/cni/plugins/port_mapper/port_mapper.hpp	\
  slave/containerizer/mesos/isolators/volume/host_path.cpp				\
  slave/containerizer/mesos/isolators/volume/host_path.hpp				\
  slave/containerizer/mesos/isolators/volume/image.cpp					\
  slave/containerizer/mesos/isolators/volume/image.hpp					\
  slave/containerizer/mesos/isolators/volume/secret.cpp					\
  slave/containerizer/mesos/isolators/volume/secret.hpp					\
  slave/containerizer/mesos/isolators/volume/utils.cpp					\
  slave/containerizer/mesos/isolators/volume/utils.hpp					\
  slave/containerizer/mesos/isolators/volume/csi/isolator.cpp				\
  slave/containerizer/mesos/isolators/volume/csi/isolator.hpp				\
  slave/containerizer/mesos/provisioner/backends/aufs.cpp				\
  slave/containerizer/mesos/provisioner/backends/aufs.hpp				\
  slave/containerizer/mesos/provisioner/backends/bind.cpp				\
  slave/containerizer/mesos/provisioner/backends/bind.hpp				\
  slave/containerizer/mesos/provisioner/backends/overlay.cpp				\
  slave/containerizer/mesos/provisioner/backends/overlay.hpp

if ENABLE_XFS_DISK_ISOLATOR
MESOS_LINUX_FILES +=							\
  slave/containerizer/mesos/isolators/xfs/utils.cpp			\
  slave/containerizer/mesos/isolators/xfs/utils.hpp			\
  slave/containerizer/mesos/isolators/xfs/disk.cpp			\
  slave/containerizer/mesos/isolators/xfs/disk.hpp
endif

if ENABLE_SECCOMP_ISOLATOR
MESOS_LINUX_FILES +=							\
  linux/seccomp/seccomp.cpp						\
  linux/seccomp/seccomp.hpp						\
  linux/seccomp/seccomp_parser.cpp					\
  linux/seccomp/seccomp_parser.hpp					\
  slave/containerizer/mesos/isolators/linux/seccomp.cpp			\
  slave/containerizer/mesos/isolators/linux/seccomp.hpp
endif

if ENABLE_LAUNCHER_SEALING
MESOS_LINUX_FILES +=							\
  linux/memfd.cpp							\
  linux/memfd.hpp
endif

if ENABLE_LINUX_ROUTING
MESOS_LINUX_FILES +=							\
  linux/routing/handle.cpp						\
  linux/routing/handle.hpp						\
  linux/routing/internal.hpp						\
  linux/routing/route.cpp						\
  linux/routing/route.hpp						\
  linux/routing/utils.cpp						\
  linux/routing/utils.hpp						\
  linux/routing/diagnosis/diagnosis.cpp					\
  linux/routing/diagnosis/diagnosis.hpp					\
  linux/routing/filter/action.hpp					\
  linux/routing/filter/basic.cpp					\
  linux/routing/filter/basic.hpp					\
  linux/routing/filter/filter.hpp					\
  linux/routing/filter/handle.hpp					\
  linux/routing/filter/icmp.cpp						\
  linux/routing/filter/icmp.hpp						\
  linux/routing/filter/internal.hpp					\
  linux/routing/filter/ip.cpp						\
  linux/routing/filter/ip.hpp						\
  linux/routing/filter/priority.hpp					\
  linux/routing/link/internal.hpp					\
  linux/routing/link/link.cpp						\
  linux/routing/link/link.hpp						\
  linux/routing/link/veth.cpp						\
  linux/routing/link/veth.hpp						\
  linux/routing/queueing/discipline.hpp					\
  linux/routing/queueing/fq_codel.cpp					\
  linux/routing/queueing/fq_codel.hpp					\
  linux/routing/queueing/htb.cpp					\
  linux/routing/queueing/htb.hpp					\
  linux/routing/queueing/ingress.cpp					\
  linux/routing/queueing/ingress.hpp					\
  linux/routing/queueing/internal.hpp					\
  linux/routing/queueing/statistics.hpp
endif

if ENABLE_NETWORK_PORTS_ISOLATOR
MESOS_LINUX_FILES +=							\
  slave/containerizer/mesos/isolators/network/ports.cpp			\
  slave/containerizer/mesos/isolators/network/ports.hpp
endif

if ENABLE_PORT_MAPPING_ISOLATOR
MESOS_LINUX_FILES +=							\
  slave/containerizer/mesos/isolators/network/port_mapping.hpp		\
  slave/containerizer/mesos/isolators/network/port_mapping.cpp
endif

if OS_LINUX
libmesos_no_3rdparty_la_SOURCES += $(MESOS_LINUX_FILES)
else
EXTRA_DIST += $(MESOS_LINUX_FILES)
endif

if ENABLE_SSL
libmesos_no_3rdparty_la_SOURCES +=					\
  authentication/executor/jwt_secret_generator.cpp			\
  authentication/executor/jwt_secret_generator.hpp
endif

libmesos_no_3rdparty_la_SOURCES +=					\
  resource_provider/storage/disk_profile_adaptor.cpp			\
  resource_provider/storage/disk_profile.proto				\
  resource_provider/storage/disk_profile_utils.cpp			\
  resource_provider/storage/disk_profile_utils.hpp			\
  resource_provider/storage/provider.cpp				\
  resource_provider/storage/provider.hpp

libmesos_no_3rdparty_la_CPPFLAGS = $(MESOS_CPPFLAGS)

libmesos_no_3rdparty_la_LIBADD = # Initialized to enable using +=.

# Convenience library that *always* gets rebuilt to ensure accurate info.
noinst_LTLIBRARIES += libbuild.la
libbuild_la_SOURCES =							\
  common/build.cpp							\
  common/git_version.hpp
libbuild_la_CPPFLAGS = $(AM_CPPFLAGS)
libbuild_la_CPPFLAGS += $(MESOS_CPPFLAGS)
libbuild_la_CPPFLAGS += -DBUILD_DATE="\"$$(date '+%Y-%m-%d %H:%M:%S')\""
libbuild_la_CPPFLAGS += -DBUILD_TIME="\"$$(date '+%s')\""
libbuild_la_CPPFLAGS += -DBUILD_USER="\"$$USER\""
libbuild_la_CPPFLAGS += -DBUILD_JAVA_JVM_LIBRARY=\"$(JAVA_JVM_LIBRARY)\"

# We need to escape the build flags properly.
BUILD_FLAGS = $(echo $(MESOS_CPPFLAGS) $(CPPFLAGS) | $(SED) 's/\"/\\\"/g')	\
              $(echo $(AM_CFLAGS) $(CFLAGS) | $(SED) 's/\"/\\\"/g')		\
              $(echo $(AM_CXXFLAGS) $(CXXFLAGS) | $(SED) 's/\"/\\\"/g')

EXTRA_DIST += common/git_version.hpp.in

# TODO(benh): Provide other build flags.

libbuild_la_CPPFLAGS += -DBUILD_FLAGS="\"$$BUILD_FLAGS\""

libmesos_no_3rdparty_la_LIBADD += libbuild.la


# Convenience library for build the CSI client.
noinst_LTLIBRARIES += libcsi.la
libcsi_la_SOURCES =							\
  csi/constants.hpp							\
  csi/metrics.cpp							\
  csi/metrics.hpp							\
  csi/paths.cpp								\
  csi/paths.hpp								\
  csi/service_manager.cpp						\
  csi/service_manager.hpp						\
  csi/state.hpp								\
  csi/state.proto							\
  csi/v0.cpp								\
  csi/v0_client.cpp							\
  csi/v0_client.hpp							\
  csi/v0_utils.cpp							\
  csi/v0_utils.hpp							\
  csi/v0_volume_manager.cpp						\
  csi/v0_volume_manager.hpp						\
  csi/v0_volume_manager_process.hpp					\
  csi/v1.cpp								\
  csi/v1_client.cpp							\
  csi/v1_client.hpp							\
  csi/v1_utils.cpp							\
  csi/v1_utils.hpp							\
  csi/v1_volume_manager.cpp						\
  csi/v1_volume_manager.hpp						\
  csi/v1_volume_manager_process.hpp					\
  csi/volume_manager.cpp						\
  csi/volume_manager.hpp

nodist_libcsi_la_SOURCES = $(CXX_CSI_PROTOS)
libcsi_la_CPPFLAGS = $(MESOS_CPPFLAGS)

libmesos_no_3rdparty_la_LIBADD += libcsi.la


# Convenience library for building the replicated log in order to
# include the leveldb headers.
noinst_LTLIBRARIES += liblog.la
liblog_la_SOURCES =							\
  log/catchup.cpp							\
  log/catchup.hpp							\
  log/consensus.cpp							\
  log/consensus.hpp							\
  log/coordinator.cpp							\
  log/coordinator.hpp							\
  log/leveldb.cpp							\
  log/leveldb.hpp							\
  log/log.cpp								\
  log/log.hpp								\
  log/metrics.cpp							\
  log/metrics.hpp							\
  log/network.hpp							\
  log/recover.cpp							\
  log/recover.hpp							\
  log/replica.cpp							\
  log/replica.hpp							\
  log/storage.hpp							\
  log/tool/benchmark.cpp						\
  log/tool/benchmark.hpp						\
  log/tool.hpp								\
  log/tool/initialize.cpp						\
  log/tool/initialize.hpp						\
  log/tool/read.cpp							\
  log/tool/read.hpp							\
  log/tool/replica.cpp							\
  log/tool/replica.hpp							\
  messages/log.hpp							\
  messages/log.proto

nodist_liblog_la_SOURCES = $(CXX_LOG_PROTOS)
liblog_la_CPPFLAGS = $(MESOS_CPPFLAGS)

libmesos_no_3rdparty_la_LIBADD += liblog.la


# Convenience library for building "state" abstraction in order to
# include the leveldb headers.
noinst_LTLIBRARIES += libstate.la
libstate_la_SOURCES =							\
  messages/state.hpp							\
  messages/state.proto							\
  state/in_memory.cpp							\
  state/leveldb.cpp							\
  state/log.cpp								\
  state/zookeeper.cpp

nodist_libstate_la_SOURCES = $(CXX_STATE_PROTOS)
libstate_la_CPPFLAGS = $(MESOS_CPPFLAGS)

libmesos_no_3rdparty_la_LIBADD += libstate.la


# The final result!
lib_LTLIBRARIES += libmesos.la

# Include as part of the distribution.
libmesos_la_SOURCES =							\
  $(ACLS_PROTO)								\
  $(AGENT_PROTO)							\
  $(ALLOCATOR_PROTO)							\
  $(APPC_SPEC_PROTO)							\
  $(AUTHENTICATION_PROTO)						\
  $(AUTHORIZATION_PROTO)						\
  $(CONTAINERIZER_PROTO)						\
  $(EXECUTOR_PROTO)							\
  $(DOCKER_SPEC_PROTO)							\
  $(DOCKER_V1_PROTO)							\
  $(DOCKER_V2_PROTO)							\
  $(DOCKER_V2_2_PROTO)							\
  $(FETCHER_PROTO)							\
  $(HOOK_PROTO)								\
  $(MAINTENANCE_PROTO)							\
  $(MASTER_PROTO)							\
  $(MESOS_PROTO)							\
  $(MODULE_PROTO)							\
  $(OCI_SPEC_PROTO)							\
  $(OVERSUBSCRIPTION_PROTO)						\
  $(QUOTA_PROTO)							\
  $(RESOURCE_PROVIDER_PROTO)						\
  $(SCHEDULER_PROTO)							\
  $(SECCOMP_PROTO)							\
  $(STATE_PROTO)							\
  $(URI_PROTO)								\
  $(V1_AGENT_PROTO)							\
  $(V1_ALLOCATOR_PROTO)							\
  $(V1_EXECUTOR_PROTO)							\
  $(V1_MAINTENANCE_PROTO)						\
  $(V1_MASTER_PROTO)							\
  $(V1_MESOS_PROTO)							\
  $(V1_QUOTA_PROTO)							\
  $(V1_RESOURCE_PROVIDER_PROTO)						\
  $(V1_SCHEDULER_PROTO)

libmesos_la_LDFLAGS = -release $(PACKAGE_VERSION)

# Since we just include the convenience library (and no sources), we
# need to tell libtool to build this as a C++ library.
libmesos_la_LIBTOOLFLAGS = --tag=CXX

# Add the convenience library.
libmesos_la_LIBADD =							\
  libmesos_no_3rdparty.la						\
  $(LIB_GLOG)								\
  $(LIB_GRPC)								\
  $(LIB_LEVELDB)							\
  $(LIB_LIBARCHIVE)							\
  $(LIB_PROCESS)							\
  $(LIB_PROTOBUF)							\
  $(LIB_LIBSECCOMP)							\
  $(LIB_RE2)								\
  $(LIB_ZOOKEEPER)							\
  -lsvn_subr-1								\
  -lsvn_delta-1								\
  -lapr-1


# Binaries.
sbin_PROGRAMS += mesos-agent
mesos_agent_SOURCES = slave/main.cpp
mesos_agent_CPPFLAGS = $(MESOS_CPPFLAGS)
mesos_agent_LDADD = libmesos.la $(LDADD)

sbin_PROGRAMS += mesos-master
mesos_master_SOURCES = master/main.cpp
mesos_master_CPPFLAGS = $(MESOS_CPPFLAGS)
mesos_master_LDADD = libmesos.la $(LDADD)

bin_PROGRAMS += mesos-local
mesos_local_SOURCES = local/main.cpp
mesos_local_CPPFLAGS = $(MESOS_CPPFLAGS)
mesos_local_LDADD = libmesos.la $(LDADD)

if ENABLE_JEMALLOC_ALLOCATOR
mesos_agent_LDADD += $(LIB_JEMALLOC)
mesos_master_LDADD  += $(LIB_JEMALLOC)
endif

pkglibexec_PROGRAMS += mesos-fetcher
mesos_fetcher_SOURCES = launcher/fetcher.cpp
mesos_fetcher_CPPFLAGS = $(MESOS_CPPFLAGS)
mesos_fetcher_LDADD = libmesos.la $(LDADD)

pkglibexec_PROGRAMS += mesos-executor
mesos_executor_SOURCES = launcher/executor.cpp
mesos_executor_CPPFLAGS = $(MESOS_CPPFLAGS)
mesos_executor_LDADD = libmesos.la $(LDADD)

pkglibexec_PROGRAMS += mesos-default-executor
mesos_default_executor_SOURCES =					\
  launcher/default_executor.cpp
mesos_default_executor_CPPFLAGS = $(MESOS_CPPFLAGS)
mesos_default_executor_LDADD = libmesos.la $(LDADD)

pkglibexec_PROGRAMS += mesos-containerizer
mesos_containerizer_SOURCES = slave/containerizer/mesos/main.cpp
mesos_containerizer_CPPFLAGS = $(MESOS_CPPFLAGS)
mesos_containerizer_LDADD = libmesos.la $(LDADD)

if OS_LINUX
pkglibexec_PROGRAMS += mesos-cni-port-mapper
mesos_cni_port_mapper_SOURCES = \
  slave/containerizer/mesos/isolators/network/cni/plugins/port_mapper/main.cpp
mesos_cni_port_mapper_CPPFLAGS = $(MESOS_CPPFLAGS)
mesos_cni_port_mapper_LDADD = libmesos.la $(LDADD)
endif

pkglibexec_PROGRAMS += mesos-logrotate-logger
mesos_logrotate_logger_SOURCES =		\
  slave/container_loggers/logrotate.hpp		\
  slave/container_loggers/logrotate.cpp
mesos_logrotate_logger_CPPFLAGS = $(MESOS_CPPFLAGS)
mesos_logrotate_logger_LDADD = libmesos.la $(LDADD)

pkglibexec_PROGRAMS += mesos-io-switchboard
mesos_io_switchboard_SOURCES =	\
  slave/containerizer/mesos/io/switchboard_main.cpp
mesos_io_switchboard_CPPFLAGS = $(MESOS_CPPFLAGS)
mesos_io_switchboard_LDADD = libmesos.la $(LDADD)

if ENABLE_PORT_MAPPING_ISOLATOR
pkglibexec_PROGRAMS += mesos-network-helper
mesos_network_helper_SOURCES = slave/containerizer/mesos/isolators/network/helper.cpp
mesos_network_helper_CPPFLAGS = $(MESOS_CPPFLAGS)
mesos_network_helper_LDADD = libmesos.la $(LDADD)
endif

pkglibexec_PROGRAMS += mesos-usage
mesos_usage_SOURCES = usage/main.cpp
mesos_usage_CPPFLAGS = $(MESOS_CPPFLAGS)
mesos_usage_LDADD = libmesos.la $(LDADD)

pkglibexec_PROGRAMS += mesos-docker-executor
mesos_docker_executor_SOURCES = launcher/docker_executor.cpp
mesos_docker_executor_CPPFLAGS = $(MESOS_CPPFLAGS)
mesos_docker_executor_LDADD = libmesos.la $(LDADD)

pkglibexec_PROGRAMS += mesos-tcp-connect
mesos_tcp_connect_SOURCES = checks/tcp_connect.cpp
mesos_tcp_connect_CPPFLAGS = $(MESOS_CPPFLAGS)
mesos_tcp_connect_LDADD = libmesos.la $(LDADD)

bin_PROGRAMS += mesos-log
mesos_log_SOURCES = log/main.cpp
mesos_log_CPPFLAGS = $(MESOS_CPPFLAGS)
mesos_log_LDADD = libmesos.la $(LDADD)

bin_PROGRAMS += mesos-execute
mesos_execute_SOURCES = cli/execute.cpp
mesos_execute_CPPFLAGS = $(MESOS_CPPFLAGS)
mesos_execute_LDADD = libmesos.la $(LDADD)

bin_PROGRAMS += mesos-resolve
mesos_resolve_SOURCES = cli/resolve.cpp
mesos_resolve_CPPFLAGS = $(MESOS_CPPFLAGS)
mesos_resolve_LDADD = libmesos.la $(LDADD)

bin_PROGRAMS += mesos

NEW_CLI_PLUGINS = \
	python/cli_new/lib/cli/plugins/agent/__init__.py		\
	python/cli_new/lib/cli/plugins/agent/main.py			\
	python/cli_new/lib/cli/plugins/config/__init__.py		\
	python/cli_new/lib/cli/plugins/config/main.py			\
	python/cli_new/lib/cli/plugins/task/__init__.py			\
	python/cli_new/lib/cli/plugins/task/main.py			\
	python/cli_new/lib/cli/plugins/__init__.py			\
	python/cli_new/lib/cli/plugins/base.py

NEW_CLI = \
	python/cli_new/bin/main.py					\
	python/cli_new/bin/mesos					\
	python/cli_new/bin/mesos-cli-tests				\
	python/cli_new/bin/settings.py					\
	python/cli_new/activate						\
	python/cli_new/deactivate					\
	python/cli_new/lib/cli/__init__.py				\
	python/cli_new/lib/cli/constants.py				\
	python/cli_new/lib/cli/tests/tests.py				\
	python/cli_new/lib/cli/tests/__init__.py			\
	python/cli_new/lib/cli/tests/agent.py				\
	python/cli_new/lib/cli/tests/base.py				\
	python/cli_new/lib/cli/tests/constants.py			\
	python/cli_new/lib/cli/tests/data/lorem-ipsum.txt		\
	python/cli_new/lib/cli/tests/task.py				\
	python/cli_new/lib/cli/config.py				\
	python/cli_new/lib/cli/docopt.py				\
	python/cli_new/lib/cli/exceptions.py				\
	python/cli_new/lib/cli/http.py					\
	python/cli_new/lib/cli/mesos.py					\
	python/cli_new/lib/cli/util.py					\
	python/cli_new/mesos.bash_completion				\
	python/cli_new/tests/default_config.toml			\
	python/cli_new/tests/CMakeLists.txt				\
	python/cli_new/tests/__init__.py				\
	python/cli_new/tests/main.py					\
	python/cli_new/CMakeLists.txt					\
	python/cli_new/README.md					\
	python/cli_new/bootstrap					\
	python/cli_new/pip-requirements.txt

if ENABLE_NEW_CLI
MESOS_CLI_SRCDIR = $(srcdir)/python/cli_new
MESOS_CLI_BUILDDIR = $(builddir)/cli

mesos: $(shell git ls-files $(MESOS_CLI_SRCDIR))
	@echo "Building the CLI ..."
	VIRTUALENV_DIRECTORY=$(builddir)/.virtualenv	\
	PYTHON=$(PYTHON_3)				\
	$(MESOS_CLI_SRCDIR)/bootstrap &&		\
	echo "VERSION = \"$(PACKAGE_VERSION)\""		\
		> $(MESOS_CLI_BUILDDIR)/version.py &&	\
	source $(builddir)/.virtualenv/bin/activate &&	\
	pyinstaller -p $(MESOS_CLI_SRCDIR)/lib		\
		$(NEW_CLI_PLUGINS)			\
		--specpath $(MESOS_CLI_BUILDDIR)	\
		--workpath $(MESOS_CLI_BUILDDIR)/work	\
		--distpath $(builddir)			\
		--name $(@)				\
		--noconfirm				\
		--onefile				\
		$(MESOS_CLI_SRCDIR)/bin/main.py
check_PROGRAMS += mesos

EXTRA_DIST += cli/mesos.cpp

else
mesos_SOURCES = cli/mesos.cpp
mesos_CPPFLAGS = $(MESOS_CPPFLAGS)
mesos_LDADD = libmesos.la $(LDADD)

# Need to distribute and install mesos-*.
dist_bin_SCRIPTS +=							\
  cli/mesos-cat								\
  cli/mesos-ps								\
  cli/mesos-scp								\
  cli/mesos-tail
endif

EXTRA_DIST += \
	$(NEW_CLI)							\
	$(NEW_CLI_PLUGINS)						\
	python/lib/mesos/__init__.py					\
	python/lib/mesos/exceptions.py					\
	python/lib/mesos/http.py					\
	python/lib/mesos/recordio.py					\
	python/lib/tests/__init__.py					\
	python/lib/tests/conftest.py					\
	python/lib/tests/test_exceptions.py				\
	python/lib/tests/test_http.py					\
	python/lib/tests/test_mesos.py					\
	python/lib/tests/test_recordio.py				\
	python/lib/requirements.in					\
	python/lib/setup.py

# Need to distribute/install webui javascript. We use 'pkgdatadir'
# instead of 'datadir' as the install directory so we get the
# package name (i.e., 'mesos') as part of the path (i.e.,
# /path/to/mesos/webui versus something like /path/to/webui). Note
# that 'datadir' (e.g., /usr/local/share) is for read-only "data" and
# 'sysconfdir' (e.g., /usr/local/var) is for modifiable "data".
nobase_dist_pkgdata_DATA +=						\
  webui/app/app.js							\
  webui/app/controllers.js						\
  webui/app/services.js

# Need to distribute/install webui CSS.
nobase_dist_pkgdata_DATA +=						\
  webui/assets/css/bootstrap-3.3.6.min.css                              \
  webui/assets/css/bootstrap-table-1.11.1.min.css\
  webui/assets/css/mesos.css

# Need to distribute/install webui HTML.
nobase_dist_pkgdata_DATA +=						\
  webui/index.html							\
  webui/app/home.html							\
  webui/app/agents/agent.html						\
  webui/app/agents/agent-browse.html					\
  webui/app/agents/agent-executor.html					\
  webui/app/agents/agent-framework.html					\
  webui/app/agents/agents.html						\
  webui/app/frameworks/framework.html					\
  webui/app/frameworks/frameworks.html					\
  webui/app/frameworks/roles.html					\
  webui/app/frameworks/roles-tree.html					\
  webui/app/frameworks/roles-tree-root.html				\
  webui/app/maintenance/maintenance.html				\
  webui/app/offers/offers.html						\
  webui/app/roles/roles.html						\
  webui/app/shared/pagination.html					\
  webui/app/shared/pailer.html						\
  webui/app/shared/table-header.html					\
  webui/app/shared/timestamp.html

# Need to distribute/install webui images.
nobase_dist_pkgdata_DATA +=						\
  webui/assets/ico/favicon.ico						\
  webui/assets/img/loading.gif						\
  webui/assets/img/mesos-logo.png

# Need to distribute/install webui fonts.
nobase_dist_pkgdata_DATA +=						\
  webui/assets/fonts/glyphicons-halflings-regular.eot			\
  webui/assets/fonts/glyphicons-halflings-regular.svg			\
  webui/assets/fonts/glyphicons-halflings-regular.ttf			\
  webui/assets/fonts/glyphicons-halflings-regular.woff			\
  webui/assets/fonts/glyphicons-halflings-regular.woff2

# Need to distribute/install third-party javascript.
nobase_dist_pkgdata_DATA +=						\
  webui/assets/libs/angular-1.2.32.min.js				\
  webui/assets/libs/angular-route-1.2.32.min.js				\
  webui/assets/libs/bootstrap-table-1.11.1.min.js			\
  webui/assets/libs/clipboard-1.5.16.min.js				\
  webui/assets/libs/jquery-3.6.0.min.js					\
  webui/assets/libs/jquery.pailer.js					\
  webui/assets/libs/relative-date.js					\
  webui/assets/libs/ui-bootstrap-tpls-0.9.0.min.js			\
  webui/assets/libs/underscore-1.4.3.min.js

# And the deploy related stuff.
nodist_sbin_SCRIPTS +=							\
  deploy/mesos-daemon.sh						\
  deploy/mesos-start-agents.sh						\
  deploy/mesos-start-cluster.sh						\
  deploy/mesos-start-masters.sh						\
  deploy/mesos-start-slaves.sh						\
  deploy/mesos-stop-agents.sh						\
  deploy/mesos-stop-cluster.sh						\
  deploy/mesos-stop-masters.sh						\
  deploy/mesos-stop-slaves.sh

pkgsysconf_DATA +=							\
  deploy/mesos-agent-env.sh.template					\
  deploy/mesos-deploy-env.sh.template					\
  deploy/mesos-master-env.sh.template

# Need to explicitly add this because by default DATA files are not
# included in distributions.
EXTRA_DIST +=								\
  deploy/mesos-agent-env.sh.template					\
  deploy/mesos-deploy-env.sh.template					\
  deploy/mesos-master-env.sh.template

# Java related files are listed outside if HAS_JAVA so we can add them
# to EXTRA_DIST unconditionally.

# Mesos JAR.
MESOS_JAR = java/target/mesos-$(PACKAGE_VERSION).jar
MESOS_JAR_SOURCE =							\
  $(srcdir)/java/src/org/apache/mesos/ExecutorDriver.java		\
  $(srcdir)/java/src/org/apache/mesos/Executor.java			\
  $(srcdir)/java/src/org/apache/mesos/Log.java				\
  $(srcdir)/java/src/org/apache/mesos/MesosExecutorDriver.java		\
  $(srcdir)/java/src/org/apache/mesos/MesosSchedulerDriver.java		\
  $(srcdir)/java/src/org/apache/mesos/SchedulerDriver.java		\
  $(srcdir)/java/src/org/apache/mesos/Scheduler.java			\
  $(srcdir)/java/src/org/apache/mesos/state/AbstractState.java		\
  $(srcdir)/java/src/org/apache/mesos/state/InMemoryState.java		\
  $(srcdir)/java/src/org/apache/mesos/state/LevelDBState.java		\
  $(srcdir)/java/src/org/apache/mesos/state/LogState.java		\
  $(srcdir)/java/src/org/apache/mesos/state/State.java			\
  $(srcdir)/java/src/org/apache/mesos/state/Variable.java		\
  $(srcdir)/java/src/org/apache/mesos/state/ZooKeeperState.java		\
  $(srcdir)/java/src/org/apache/mesos/v1/scheduler/V1Mesos.java		\
  $(srcdir)/java/src/org/apache/mesos/v1/scheduler/Mesos.java		\
  $(srcdir)/java/src/org/apache/mesos/v1/scheduler/Scheduler.java	\
  $(srcdir)/java/src/org/apache/mesos/v1/scheduler/V0Mesos.java
MESOS_JAR_GENERATED = $(JAVA_PROTOS) $(V1_JAVA_PROTOS)			\
  java/generated/org/apache/mesos/MesosNativeLibrary.java
EXTRA_DIST += $(MESOS_JAR_SOURCE)					\
  $(srcdir)/java/generated/org/apache/mesos/MesosNativeLibrary.java.in

# We build all the Java examples into their own JAR.
EXAMPLES_JAR = examples.jar
EXAMPLES_SOURCE =							\
  $(srcdir)/examples/java/TestExceptionFramework.java			\
  $(srcdir)/examples/java/TestExecutor.java				\
  $(srcdir)/examples/java/TestFramework.java				\
  $(srcdir)/examples/java/TestLog.java					\
  $(srcdir)/examples/java/TestMultipleExecutorsFramework.java		\
  $(srcdir)/examples/java/V1TestFramework.java
EXTRA_DIST += $(EXAMPLES_SOURCE)


if HAS_JAVA

$(MESOS_JAR): $(MESOS_JAR_SOURCE) $(MESOS_JAR_GENERATED) java/mesos.pom
	@echo "Building mesos-$(PACKAGE_VERSION).jar ..."
	@cd $(abs_top_builddir)/src/java &&  \
	  env JAVA_HOME=$(JAVA_HOME) $(MVN) -B -q -f mesos.pom clean package

# Convenience library for JNI bindings.
# TODO(Charles Reiss): We really should be building the Java library
# with -module, which means it should be a different library than the
# main libmesos.so.
noinst_LTLIBRARIES += libjava.la

libjava_la_SOURCES =							\
  java/jni/convert.cpp							\
  java/jni/convert.hpp							\
  java/jni/construct.cpp						\
  java/jni/construct.hpp						\
  java/jni/org_apache_mesos_Log.cpp					\
  java/jni/org_apache_mesos_MesosExecutorDriver.cpp			\
  java/jni/org_apache_mesos_MesosNativeLibrary.cpp			\
  java/jni/org_apache_mesos_MesosSchedulerDriver.cpp			\
  java/jni/org_apache_mesos_state_AbstractState.cpp			\
  java/jni/org_apache_mesos_state_LevelDBState.cpp			\
  java/jni/org_apache_mesos_state_LogState.cpp				\
  java/jni/org_apache_mesos_state_Variable.cpp				\
  java/jni/org_apache_mesos_state_ZooKeeperState.cpp			\
  java/jni/org_apache_mesos_v1_scheduler_V1Mesos.cpp			\
  java/jni/org_apache_mesos_v1_scheduler_V0Mesos.cpp			\
  jvm/jvm.cpp								\
  jvm/jvm.hpp								\
  jvm/java/io.hpp							\
  jvm/java/lang.hpp							\
  jvm/java/net.hpp							\
  jvm/org/apache/log4j.cpp						\
  jvm/org/apache/log4j.hpp						\
  jvm/org/apache/zookeeper.cpp						\
  jvm/org/apache/zookeeper.hpp

libjava_la_CPPFLAGS = $(MESOS_CPPFLAGS)
libjava_la_CPPFLAGS += $(JAVA_CPPFLAGS)
libjava_la_CPPFLAGS += -I$(srcdir)/java/jni -Ijava/jni

libjava_la_DEPENDENCIES =						\
  $(MESOS_PROTO)							\
  $(V1_AGENT_PROTO)							\
  $(V1_ALLOCATOR_PROTO)							\
  $(V1_EXECUTOR_PROTO)							\
  $(V1_MAINTENANCE_PROTO)						\
  $(V1_MASTER_PROTO)							\
  $(V1_MESOS_PROTO)							\
  $(V1_QUOTA_PROTO)							\
  $(V1_RESOURCE_PROVIDER_PROTO)						\
  $(V1_SCHEDULER_PROTO)

# We don't add libjava.la to libmesos_no_3rdparty.la so we don't
# include the JNI bindings in the Python egg (but we might want to
# reconsider this in the future).
libmesos_la_LIBADD += libjava.la

# We need to generate the JNI header files before we build the sources
# and the easiest way to do that with autotools is with the
# BUILT_SOURCES mechanism. The downside of this is that a user cannot
# run 'make libjava.la' (or any target that uses BUILT_SOURCES) but
# that should not matter as libjava.la is just a convenience library
# and should not get exposed.

nodist_libjava_la_SOURCES =						\
  java/jni/org_apache_mesos_Log.h					\
  java/jni/org_apache_mesos_MesosExecutorDriver.h			\
  java/jni/org_apache_mesos_MesosSchedulerDriver.h			\
  java/jni/org_apache_mesos_state_AbstractState.h			\
  java/jni/org_apache_mesos_state_LevelDBState.h			\
  java/jni/org_apache_mesos_state_LogState.h				\
  java/jni/org_apache_mesos_state_Variable.h				\
  java/jni/org_apache_mesos_state_ZooKeeperState.h			\
  java/jni/org_apache_mesos_v1_scheduler_V1Mesos.h			\
  java/jni/org_apache_mesos_v1_scheduler_V0Mesos.h

BUILT_SOURCES += $(nodist_libjava_la_SOURCES)

# The automatic variable '$(*F)' captures the matching stem (whatever matches
# the '%') of the target (e.g., 'org_apache_mesos_Log').
# * For 'javah' recipe, we then substitute '_' with '.' to get the class name
#   (e.g., org.apache.mesos.Log).
# * For 'javac -h' recipe, we substitute '_' with '/' to compute the source path
#   of the java file. Unlike 'javah', 'javac -h' requires java sources to
#   generate headers.
#
# NOTE: javah has a race between concurrent runs trying to create
# the same output directory:
# http://hg.openjdk.java.net/jdk8/jdk8/langtools/file/1ff9d5118aae/src/share/classes/com/sun/tools/javah/JavahTask.java#l475
# Thus, we have to create the output directory here.
#
# TODO(asekretenko): Replace `javah` (deprecated as of JDK 9+)
# with `javac` when switching to a newer JDK.
java/jni/%.h: $(MESOS_JAR)
	mkdir -p java/jni
if HAS_JAVAH
	$(JAVA_HOME)/bin/javah -d java/jni                              \
	  -classpath $(MESOS_JAR):@PROTOBUF_JAR@                        \
	   $(subst _,.,$(*F))
else
	$(JAVA_HOME)/bin/javac -h java/jni				\
	  -classpath $(MESOS_JAR):@PROTOBUF_JAR@                        \
	   $(srcdir)/java/src/$(subst _,/,$(*F)).java
endif

$(EXAMPLES_JAR): $(EXAMPLES_SOURCE)
	@echo "Building examples.jar ..."
	$(MKDIR_P) examples/java
	$(JAVA_HOME)/bin/javac -source 1.6 -target 1.6			\
          -cp ../$(ZOOKEEPER_JAR):@PROTOBUF_JAR@:$(MESOS_JAR):$(srcdir)/examples/java	\
          -sourcepath $(srcdir)/examples/java -d examples/java	        \
          $(srcdir)/examples/java/*.java
	$(JAVA_HOME)/bin/jar cf $@ -C examples/java .

CLEANFILES += $(EXAMPLES_JAR)

maven-install: $(MESOS_JAR) java/mesos.pom
	env JAVA_HOME=$(JAVA_HOME) $(MVN) -B -q -f java/mesos.pom install

PHONY_TARGETS += maven-install
endif # HAS_JAVA

clean-java:
	-rm -rf java/target
	-rm -f examples/java/*.class
	-rm -f java/jni/org_apache_mesos*.h

PHONY_TARGETS += clean-java


# Python files listed outside HAS_PYTHON so they are included with the
# distribution unconditionally.
PYTHON_SOURCE =								\
  python/cli/src/mesos/__init__.py					\
  python/cli/src/mesos/cli.py						\
  python/cli/src/mesos/futures.py					\
  python/cli/src/mesos/http.py						\
  python/interface/src/mesos/__init__.py				\
  python/interface/src/mesos/interface/__init__.py			\
  python/interface/src/mesos/v1/__init__.py				\
  python/interface/src/mesos/v1/interface/__init__.py			\
  python/native_common/common.hpp					\
  python/native/src/mesos/__init__.py					\
  python/native/src/mesos/native/__init__.py				\
  python/executor/src/mesos/__init__.py					\
  python/executor/src/mesos/executor/__init__.py			\
  python/executor/src/mesos/executor/mesos_executor_driver_impl.cpp	\
  python/executor/src/mesos/executor/mesos_executor_driver_impl.hpp	\
  python/executor/src/mesos/executor/module.cpp				\
  python/executor/src/mesos/executor/proxy_executor.cpp			\
  python/executor/src/mesos/executor/proxy_executor.hpp			\
  python/scheduler/src/mesos/__init__.py				\
  python/scheduler/src/mesos/scheduler/__init__.py			\
  python/scheduler/src/mesos/scheduler/mesos_scheduler_driver_impl.cpp	\
  python/scheduler/src/mesos/scheduler/mesos_scheduler_driver_impl.hpp	\
  python/scheduler/src/mesos/scheduler/module.cpp			\
  python/scheduler/src/mesos/scheduler/proxy_scheduler.cpp		\
  python/scheduler/src/mesos/scheduler/proxy_scheduler.hpp		\
  python/src/mesos/__init__.py

EXTRA_DIST += $(PYTHON_SOURCE)

if HAS_PYTHON

if WITH_BUNDLED_PROTOBUF
# Build a protobuf Python egg.
PROTOBUF_EGG_SUFFIX = $(PROTOBUF_VERSION)$(PYTHON_EGG_PUREPY_POSTFIX).egg
PROTOBUF_EGG = ../$(PROTOBUF)/python/dist/protobuf-$(PROTOBUF_EGG_SUFFIX)

$(PROTOBUF_EGG):
	@echo "Building protobuf Python egg ..."
	cd ../$(PROTOBUF)/python &&			\
	  CC="$(CC)"					\
	  CXX="$(CXX)"					\
	  CFLAGS="$(CFLAGS)"				\
	  CXXFLAGS="$(CXXFLAGS)"			\
	  PYTHONPATH=$(abs_top_builddir)/$(SETUPTOOLS)	\
	  $(PYTHON) setup.py build bdist_egg

CLEANFILES += $(PROTOBUF_EGG)
endif

# This builds the mesos.native egg against libmesos_no_3rdparty.a that is
# self-contained. It currently depends on the libraries in 3rdparty
# being built as .a's. (If this is changed, the setup.py will need to
# be edited).

# TODO(Charles Reiss): Figure out how to do a proper out-of-source
# build of these Python eggs. Right now we copy the entire source
# directories into the build directory.

# TODO(Charles Reiss): Additionally support building and installing
# Python libraries that link against the system libmesos.so. This
# should be the default mode if we eventually are able to rely on
# system versions of our dependencies (since we won't be able to
# create a standalone egg anyways).

PHONY_TARGETS += $(PYTHON_SOURCE)

$(PYTHON_SOURCE):
	test "$(top_srcdir)" = "$(top_builddir)" ||			\
		($(MKDIR_P) $(@D) && cp -pf $(srcdir)/$@ $@)

# We currently require both eggs and wheels to be built. Eggs can be added to
# the PYTHONPATH and imported. These end up being used for the tests. Wheels
# can be easily uninstalled. They end up being what gets installed/uninstalled.
MESOS_EGGS =										\
  python/dist/mesos-$(PACKAGE_VERSION)$(PYTHON_EGG_PUREPY_POSTFIX).egg			\
  python/dist/mesos.cli-$(PACKAGE_VERSION)$(PYTHON_EGG_PUREPY_POSTFIX).egg		\
  python/dist/mesos.executor-$(PACKAGE_VERSION)$(PYTHON_EGG_POSTFIX).egg		\
  python/dist/mesos.interface-$(PACKAGE_VERSION)$(PYTHON_EGG_PUREPY_POSTFIX).egg	\
  python/dist/mesos.native-$(PACKAGE_VERSION)$(PYTHON_EGG_PUREPY_POSTFIX).egg		\
  python/dist/mesos.scheduler-$(PACKAGE_VERSION)$(PYTHON_EGG_POSTFIX).egg

MESOS_WHLS =										\
  python/dist/mesos-$(PACKAGE_VERSION)$(PYTHON_WHL_PUREPY_POSTFIX).whl			\
  python/dist/mesos.cli-$(PACKAGE_VERSION)$(PYTHON_WHL_PUREPY_POSTFIX).whl		\
  python/dist/mesos.executor-$(PACKAGE_VERSION)$(PYTHON_WHL_POSTFIX).whl		\
  python/dist/mesos.interface-$(PACKAGE_VERSION)$(PYTHON_WHL_PUREPY_POSTFIX).whl	\
  python/dist/mesos.native-$(PACKAGE_VERSION)$(PYTHON_WHL_PUREPY_POSTFIX).whl		\
  python/dist/mesos.scheduler-$(PACKAGE_VERSION)$(PYTHON_WHL_POSTFIX).whl

# The python source is in directories of the form: python/interface. The make
# target is of the form: python/dist/mesos.interface-0.20.0-py2.7.egg. To build
# this, we must first cd into it. The first line of this takes the make target
# and converts it into the correct directory path.
$(MESOS_EGGS) $(MESOS_WHLS):								\
		$(PYTHON_PROTOS)							\
		$(V1_PYTHON_PROTOS)							\
		$(PYTHON_SOURCE)							\
		libmesos_no_3rdparty.la							\
		$(PROTOBUF_EGG)
	@cd python/`echo $@ | awk -F"mesos[.]?" '{print $$2}' | cut -d- -f1` &&		\
	LIBS="$(LIBS)" CC="$(CC)" CXX="$(CXX)"						\
	CFLAGS="$(PYTHON_CFLAGS)" CPPFLAGS="$(PYTHON_CPPFLAGS)"				\
	LDFLAGS="$(PYTHON_LDFLAGS)"							\
	PYTHONPATH=$(abs_top_builddir)/$(SETUPTOOLS):$(abs_top_builddir)/$(WHEEL)	\
	$(PYTHON) setup.py bdist_egg --dist-dir=$(abs_top_builddir)/src/python/dist	\
	bdist_wheel --dist-dir=$(abs_top_builddir)/src/python/dist

CLEANFILES += $(MESOS_EGGS) $(MESOS_WHLS) python/*/build python/*/dist $(PYTHON_SOURCE)

pippythonpath = $(DESTDIR)$(pythondir):$(abs_top_builddir)/$(SETUPTOOLS):$(abs_top_builddir)/$(PIP):$(abs_top_builddir)/$(WHEEL)

# Some python installations create their own distutils.cfg (homebrew, redhat).
# This includes `prefix=` which overrides anything set via. pip during install.
# The file is required to ensure the python packages get installed to the right
# location. It only affects files installed from the python/dist directory.
python/dist/setup.cfg:
	printf "[install]\nprefix=\n" > $@

# See the comment next to AM_DISTCHECK_CONFIGURE_FLAGS in the top level
# Makefile.am for an explanation of this variable.
pipinstallargs=

if !ENABLE_PYTHON_DEPENDENCY_INSTALL
pipinstallargs += --no-deps
endif

# We are using the 3rdparty bundled pip instead of relying on what might exist
# on the system. Because the actual pip scripts have not been created, we just
# use the standard entry point.
install-exec-local:
	cd python/dist &&						\
	for whl in $(MESOS_WHLS); do					\
	  PYTHONPATH=$(pippythonpath)					\
	  PYTHONUSERBASE=$(DESTDIR)$(prefix)				\
	  $(PYTHON) -c "import pip; pip.main()" install			\
	  --user $(pipinstallargs)					\
	  --ignore-installed						\
	  --find-links=file://$(abs_top_builddir)/src/python/dist	\
	  $(abs_top_builddir)/src/$$whl;				\
	done

uninstall-local:
	for whl in $(MESOS_WHLS); do					\
	  PYTHONPATH=$(DESTDIR)$(pythondir):$(pippythonpath)		\
	  $(PYTHON) -c "import pip; pip.main()" uninstall		\
	  --yes $$(echo $$whl | cut -d/ -f3 | cut -d- -f1);		\
	done

# Make sure the egg gets built via 'make all'.
all-local: $(MESOS_EGGS)
endif # HAS_PYTHON

clean-python:
	-find python \( -name "build" -o -name "dist" -o -name "*.pyc"	\
	  -o -name "*.egg-info" \) -exec rm -rf '{}' \+

PHONY_TARGETS += clean-python

# Test (make check) binaries.
check_PROGRAMS += dynamic-reservation-framework
dynamic_reservation_framework_SOURCES = examples/dynamic_reservation_framework.cpp
dynamic_reservation_framework_CPPFLAGS = $(MESOS_CPPFLAGS)
dynamic_reservation_framework_LDADD = libmesos.la $(LDADD)

check_PROGRAMS += test-http-framework
test_http_framework_SOURCES = examples/test_http_framework.cpp
test_http_framework_CPPFLAGS = $(MESOS_CPPFLAGS)
test_http_framework_LDADD = libmesos.la $(LDADD)

check_PROGRAMS += test-csi-user-framework
test_csi_user_framework_SOURCES = examples/test_csi_user_framework.cpp
test_csi_user_framework_CPPFLAGS = $(MESOS_CPPFLAGS)
test_csi_user_framework_LDADD = libmesos.la $(LDADD)

check_PROGRAMS += test-framework
test_framework_SOURCES = examples/test_framework.cpp
test_framework_CPPFLAGS = $(MESOS_CPPFLAGS)
test_framework_LDADD = libmesos.la $(LDADD)

check_PROGRAMS += test-executor
test_executor_SOURCES = examples/test_executor.cpp
test_executor_CPPFLAGS = $(MESOS_CPPFLAGS)
test_executor_LDADD = libmesos.la $(LDADD)

check_PROGRAMS += test-http-executor
test_http_executor_SOURCES = examples/test_http_executor.cpp
test_http_executor_CPPFLAGS = $(MESOS_CPPFLAGS)
test_http_executor_LDADD = libmesos.la $(LDADD)

check_PROGRAMS += operation-feedback-framework
operation_feedback_framework_SOURCES = examples/operation_feedback_framework.cpp
operation_feedback_framework_CPPFLAGS = $(MESOS_CPPFLAGS)
operation_feedback_framework_LDADD = libmesos.la $(LDADD)

check_PROGRAMS += long-lived-framework
long_lived_framework_SOURCES = examples/long_lived_framework.cpp
long_lived_framework_CPPFLAGS = $(MESOS_CPPFLAGS)
long_lived_framework_LDADD = libmesos.la $(LDADD)

check_PROGRAMS += long-lived-executor
long_lived_executor_SOURCES = examples/long_lived_executor.cpp
long_lived_executor_CPPFLAGS = $(MESOS_CPPFLAGS)
long_lived_executor_LDADD = libmesos.la $(LDADD)

check_PROGRAMS += no-executor-framework
no_executor_framework_SOURCES = examples/no_executor_framework.cpp
no_executor_framework_CPPFLAGS = $(MESOS_CPPFLAGS)
no_executor_framework_LDADD = libmesos.la $(LDADD)

check_PROGRAMS += docker-no-executor-framework
docker_no_executor_framework_SOURCES = examples/docker_no_executor_framework.cpp
docker_no_executor_framework_CPPFLAGS = $(MESOS_CPPFLAGS)
docker_no_executor_framework_LDADD = libmesos.la $(LDADD)

check_PROGRAMS += balloon-framework
balloon_framework_SOURCES = examples/balloon_framework.cpp
balloon_framework_CPPFLAGS = $(MESOS_CPPFLAGS)
balloon_framework_LDADD = libmesos.la $(LDADD)

check_PROGRAMS += balloon-executor
balloon_executor_SOURCES = examples/balloon_executor.cpp
balloon_executor_CPPFLAGS = $(MESOS_CPPFLAGS)
balloon_executor_LDADD = libmesos.la $(LDADD)

check_PROGRAMS += inverse-offer-framework
inverse_offer_framework_SOURCES = examples/inverse_offer_framework.cpp
inverse_offer_framework_CPPFLAGS = $(MESOS_CPPFLAGS)
inverse_offer_framework_LDADD = libmesos.la $(LDADD)

check_PROGRAMS += load-generator-framework
load_generator_framework_SOURCES = examples/load_generator_framework.cpp
load_generator_framework_CPPFLAGS = $(MESOS_CPPFLAGS)
load_generator_framework_LDADD = libmesos.la $(LDADD)

check_PROGRAMS += persistent-volume-framework
persistent_volume_framework_SOURCES = examples/persistent_volume_framework.cpp
persistent_volume_framework_CPPFLAGS = $(MESOS_CPPFLAGS)
persistent_volume_framework_LDADD = libmesos.la $(LDADD)

check_PROGRAMS += disk-full-framework
disk_full_framework_SOURCES = examples/disk_full_framework.cpp
disk_full_framework_CPPFLAGS = $(MESOS_CPPFLAGS)
disk_full_framework_LDADD = libmesos.la $(LDADD)

if OS_LINUX
check_PROGRAMS += test-csi-plugin
test_csi_plugin_SOURCES = examples/test_csi_plugin.cpp
test_csi_plugin_CPPFLAGS = $(MESOS_CPPFLAGS)
test_csi_plugin_LDADD = libmesos.la $(LIB_PROTOBUF) $(LIB_GRPC) $(LDADD)
endif

check_PROGRAMS += test-helper
test_helper_SOURCES =						\
  tests/active_user_test_helper.cpp				\
  tests/flags.cpp						\
  tests/http_server_test_helper.cpp				\
  tests/kill_policy_test_helper.cpp				\
  tests/resources_utils.cpp					\
  tests/test_helper_main.cpp					\
  tests/utils.cpp						\
  tests/containerizer/memory_test_helper.cpp
if OS_LINUX
test_helper_SOURCES +=						\
  tests/containerizer/capabilities_test_helper.cpp		\
  tests/containerizer/setns_test_helper.cpp
endif
test_helper_CPPFLAGS = $(mesos_tests_CPPFLAGS)
test_helper_LDADD = libmesos.la $(mesos_tests_LDADD)

check_PROGRAMS += mesos-tests

# LDFLAGS to be used for the module libraries.
MESOS_MODULE_LDFLAGS = -release $(PACKAGE_VERSION) -shared

# Library containing the logrotate container logger.
pkgmodule_LTLIBRARIES += liblogrotate_container_logger.la
liblogrotate_container_logger_la_SOURCES =			\
  slave/container_loggers/logrotate.hpp				\
  slave/container_loggers/lib_logrotate.hpp			\
  slave/container_loggers/lib_logrotate.cpp
liblogrotate_container_logger_la_CPPFLAGS = $(MESOS_CPPFLAGS)
liblogrotate_container_logger_la_LDFLAGS = $(MESOS_MODULE_LDFLAGS)

# Library containing the fixed resource estimator.
pkgmodule_LTLIBRARIES += libfixed_resource_estimator.la
libfixed_resource_estimator_la_SOURCES = slave/resource_estimators/fixed.cpp
libfixed_resource_estimator_la_CPPFLAGS = $(MESOS_CPPFLAGS)
libfixed_resource_estimator_la_LDFLAGS = $(MESOS_MODULE_LDFLAGS)

# Library containing the load qos controller.
pkgmodule_LTLIBRARIES += libload_qos_controller.la
libload_qos_controller_la_SOURCES = slave/qos_controllers/load.hpp
libload_qos_controller_la_SOURCES += slave/qos_controllers/load.cpp
libload_qos_controller_la_CPPFLAGS = $(MESOS_CPPFLAGS)
libload_qos_controller_la_LDFLAGS = $(MESOS_MODULE_LDFLAGS)

# Library containing the URI disk profile adaptor module.
pkgmodule_LTLIBRARIES += liburi_disk_profile_adaptor.la
liburi_disk_profile_adaptor_la_SOURCES =			\
  resource_provider/storage/uri_disk_profile_adaptor.cpp	\
  resource_provider/storage/uri_disk_profile_adaptor.hpp	\
  resource_provider/storage/disk_profile_utils.hpp
liburi_disk_profile_adaptor_la_CPPFLAGS = $(MESOS_CPPFLAGS)
liburi_disk_profile_adaptor_la_LDFLAGS = $(MESOS_MODULE_LDFLAGS)

MESOS_TEST_MODULE_LDFLAGS = $(MESOS_MODULE_LDFLAGS)

# Even if we are not installing the test suite, we still need to build
# the test module libraries as shared libraries for running the test suite.
# When we use the 'noinst_' prefix to ensure these libraries are not
# installed, automake skips building the shared libraries. The workaround is
# to use '-rpath /nowhere' to force libtool to always build shared libraries.
if !INSTALL_TESTS
MESOS_TEST_MODULE_LDFLAGS += -rpath /nowhere
endif

MESOS_TEST_MODULES =		\
  libexamplemodule.la		\
  libtestallocator.la		\
  libtestanonymous.la		\
  libtestauthentication.la	\
  libtestauthorizer.la		\
  libtestcontainer_logger.la	\
  libtesthook.la		\
  libtesthttpauthenticator.la	\
  libtestisolator.la		\
  libtestmastercontender.la	\
  libtestmasterdetector.la	\
  libtestqos_controller.la	\
  libtestresource_estimator.la

# Library containing an example module.
libexamplemodule_la_SOURCES = examples/example_module_impl.cpp
libexamplemodule_la_CPPFLAGS = $(MESOS_CPPFLAGS)
libexamplemodule_la_LDFLAGS = $(MESOS_TEST_MODULE_LDFLAGS)

# Library containing the test anonymous modules.
libtestanonymous_la_SOURCES = examples/test_anonymous_module.cpp
libtestanonymous_la_CPPFLAGS = $(MESOS_CPPFLAGS)
libtestanonymous_la_LDFLAGS = $(MESOS_TEST_MODULE_LDFLAGS)

# Library containing the test CRAM-MD5 authentication modules.
# TODO(tillt): Add cyrus-sasl2 dependency while removing it from libmesos.
# TODO(tillt): Enable optional building of this module library.
libtestauthentication_la_SOURCES = examples/test_authentication_modules.cpp
libtestauthentication_la_CPPFLAGS = $(MESOS_CPPFLAGS)
libtestauthentication_la_LDFLAGS = $(MESOS_TEST_MODULE_LDFLAGS)

# Library containing the test authorizer module.
libtestauthorizer_la_SOURCES = examples/test_authorizer_module.cpp
libtestauthorizer_la_CPPFLAGS = $(MESOS_CPPFLAGS)
libtestauthorizer_la_LDFLAGS = $(MESOS_TEST_MODULE_LDFLAGS)

# Library containing the test http authenticator module.
libtesthttpauthenticator_la_SOURCES = examples/test_http_authenticator_module.cpp
libtesthttpauthenticator_la_CPPFLAGS = $(MESOS_CPPFLAGS)
libtesthttpauthenticator_la_LDFLAGS = $(MESOS_TEST_MODULE_LDFLAGS)

# Library containing the test DRF allocator module.
libtestallocator_la_SOURCES = examples/test_allocator_module.cpp
libtestallocator_la_CPPFLAGS = $(MESOS_CPPFLAGS)
libtestallocator_la_LDFLAGS = $(MESOS_TEST_MODULE_LDFLAGS)

# Library containing example test sandbox container logger module.
libtestcontainer_logger_la_SOURCES =		\
  examples/test_container_logger_module.cpp
libtestcontainer_logger_la_CPPFLAGS = $(MESOS_CPPFLAGS)
libtestcontainer_logger_la_LDFLAGS = $(MESOS_TEST_MODULE_LDFLAGS)

# Library containing the test contender module.
libtestmastercontender_la_SOURCES = examples/test_master_contender_module.cpp
libtestmastercontender_la_CPPFLAGS = $(MESOS_CPPFLAGS)
libtestmastercontender_la_LDFLAGS = $(MESOS_TEST_MODULE_LDFLAGS)

# Library containing the test detector module.
libtestmasterdetector_la_SOURCES = examples/test_master_detector_module.cpp
libtestmasterdetector_la_CPPFLAGS = $(MESOS_CPPFLAGS)
libtestmasterdetector_la_LDFLAGS = $(MESOS_TEST_MODULE_LDFLAGS)

# Library containing test Hook module.
libtesthook_la_SOURCES = examples/test_hook_module.cpp
libtesthook_la_CPPFLAGS = $(MESOS_CPPFLAGS)
libtesthook_la_LDFLAGS = $(MESOS_TEST_MODULE_LDFLAGS)

# Library containing test CPU and memory isolator modules.
libtestisolator_la_SOURCES = examples/test_isolator_module.cpp
libtestisolator_la_CPPFLAGS = $(MESOS_CPPFLAGS)
libtestisolator_la_LDFLAGS = $(MESOS_TEST_MODULE_LDFLAGS)

# Library containing example test noop resource estimator module.
libtestresource_estimator_la_SOURCES =		\
  examples/test_resource_estimator_module.cpp
libtestresource_estimator_la_CPPFLAGS = $(MESOS_CPPFLAGS)
libtestresource_estimator_la_LDFLAGS = $(MESOS_TEST_MODULE_LDFLAGS)

# Library containing example test noop qos controller module.
libtestqos_controller_la_SOURCES =		\
  examples/test_qos_controller_module.cpp
libtestqos_controller_la_CPPFLAGS = $(MESOS_CPPFLAGS)
libtestqos_controller_la_LDFLAGS = $(MESOS_TEST_MODULE_LDFLAGS)

if INSTALL_TESTS
pkgmodule_LTLIBRARIES += $(MESOS_TEST_MODULES)
else
noinst_LTLIBRARIES += $(MESOS_TEST_MODULES)
endif

mesos_tests_SOURCES =						\
  slave/qos_controllers/load.cpp				\
  tests/active_user_test_helper.cpp				\
  tests/active_user_test_helper.hpp				\
  tests/agent_container_api_tests.cpp				\
  tests/agent_operation_feedback_tests.cpp			\
  tests/allocator.cpp				    \
  tests/allocator.hpp						\
  tests/anonymous_tests.cpp					\
  tests/api_tests.cpp						\
  tests/attributes_tests.cpp					\
  tests/authentication_tests.cpp				\
  tests/authorization_tests.cpp					\
  tests/check_tests.cpp						\
  tests/cluster.cpp						\
  tests/cluster.hpp						\
  tests/command_executor_tests.cpp				\
  tests/common_validation_tests.cpp				\
  tests/consumption_metrics_tests.cpp				\
  tests/container_daemon_tests.cpp				\
  tests/container_logger_tests.cpp				\
  tests/containerizer.cpp					\
  tests/containerizer.hpp					\
  tests/cram_md5_authentication_tests.cpp			\
  tests/credentials_tests.cpp					\
  tests/csi_client_tests.cpp					\
  tests/csi_utils_tests.cpp					\
  tests/default_executor_tests.cpp				\
  tests/disk_profile_adaptor_tests.cpp				\
  tests/disk_profile_server.hpp					\
  tests/disk_quota_tests.cpp					\
  tests/dynamic_weights_tests.cpp				\
  tests/environment.cpp						\
  tests/environment.hpp						\
  tests/examples_tests.cpp					\
  tests/exception_tests.cpp					\
  tests/executor_http_api_tests.cpp				\
  tests/fault_tolerance_tests.cpp				\
  tests/fetcher_cache_tests.cpp					\
  tests/fetcher_tests.cpp					\
  tests/files_tests.cpp						\
  tests/flags.cpp						\
  tests/flags.hpp						\
  tests/gc_tests.cpp						\
  tests/hdfs_tests.cpp						\
  tests/health_check_tests.cpp					\
  tests/hierarchical_allocator_benchmarks.cpp			\
  tests/hierarchical_allocator_tests.cpp			\
  tests/hook_tests.cpp						\
  tests/http_authentication_tests.cpp				\
  tests/http_fault_tolerance_tests.cpp				\
  tests/http_server_test_helper.cpp				\
  tests/http_server_test_helper.hpp				\
  tests/kill_policy_test_helper.cpp				\
  tests/kill_policy_test_helper.hpp				\
  tests/limiter.hpp						\
  tests/log_tests.cpp						\
  tests/logging_tests.cpp					\
  tests/main.cpp						\
  tests/master_allocator_tests.cpp				\
  tests/master_authorization_tests.cpp				\
  tests/master_benchmarks.cpp					\
  tests/master_contender_detector_tests.cpp			\
  tests/master_draining_tests.cpp				\
  tests/master_load_tests.cpp					\
  tests/master_maintenance_tests.cpp				\
  tests/master_quota_tests.cpp					\
  tests/master_slave_reconciliation_tests.cpp			\
  tests/master_tests.cpp					\
  tests/master/mock_master_api_subscriber.cpp			\
  tests/master/mock_master_api_subscriber.hpp			\
  tests/master/offer_constraints_filter_tests.cpp		\
  tests/master/update_framework_tests.cpp			\
  tests/master_validation_tests.cpp				\
  tests/mesos.cpp						\
  tests/mesos.hpp						\
  tests/metrics_tests.cpp					\
  tests/mock_csi_plugin.cpp					\
  tests/mock_csi_plugin.hpp					\
  tests/mock_docker.cpp						\
  tests/mock_docker.hpp						\
  tests/mock_fetcher.cpp					\
  tests/mock_fetcher.hpp					\
  tests/mock_registrar.cpp					\
  tests/mock_registrar.hpp					\
  tests/mock_slave.cpp						\
  tests/mock_slave.hpp						\
  tests/module.cpp						\
  tests/module.hpp						\
  tests/module_tests.cpp					\
  tests/operation_reconciliation_tests.cpp			\
  tests/operation_status_update_manager_tests.cpp		\
  tests/oversubscription_tests.cpp				\
  tests/partition_tests.cpp					\
  tests/paths_tests.cpp						\
  tests/persistent_volume_endpoints_tests.cpp			\
  tests/persistent_volume_tests.cpp				\
  tests/protobuf_io_tests.cpp					\
  tests/protobuf_utils_tests.cpp				\
  tests/rate_limiting_tests.cpp					\
  tests/reconciliation_tests.cpp				\
  tests/registrar_tests.cpp					\
  tests/reservation_endpoints_tests.cpp				\
  tests/reservation_tests.cpp					\
  tests/resource_offers_tests.cpp				\
  tests/resource_provider_manager_tests.cpp			\
  tests/resource_provider_validation_tests.cpp			\
  tests/resource_quantities_tests.cpp			        \
  tests/resources_tests.cpp					\
  tests/resources_utils.cpp					\
  tests/resources_utils.hpp					\
  tests/role_tests.cpp						\
  tests/scheduler_driver_tests.cpp				\
  tests/scheduler_event_call_tests.cpp				\
  tests/scheduler_http_api_tests.cpp				\
  tests/scheduler_tests.cpp					\
  tests/script.cpp						\
  tests/script.hpp						\
  tests/slave_authorization_tests.cpp				\
  tests/slave_compatibility_tests.cpp				\
  tests/slave_recovery_tests.cpp				\
  tests/slave_tests.cpp						\
  tests/slave_validation_tests.cpp				\
  tests/sorter_tests.cpp					\
  tests/state_tests.cpp						\
  tests/task_status_update_manager_tests.cpp			\
  tests/teardown_tests.cpp					\
  tests/upgrade_tests.cpp					\
  tests/uri_fetcher_tests.cpp					\
  tests/uri_tests.cpp						\
  tests/utils.cpp						\
  tests/utils.hpp						\
  tests/values_tests.cpp					\
  tests/volume_gid_manager_tests.cpp				\
  tests/zookeeper.hpp						\
  tests/zookeeper_test_server.hpp				\
  tests/zookeeper_url_tests.cpp					\
  tests/common/command_utils_tests.cpp				\
  tests/common/future_tracker_tests.cpp				\
  tests/common/http_tests.cpp					\
  tests/common/http_tests.pb.cc					\
  tests/common/http_tests.pb.h					\
  tests/common/http_tests.proto					\
  tests/common/recordio_tests.cpp				\
  tests/common/type_utils_tests.cpp				\
  tests/containerizer/appc_spec_tests.cpp			\
  tests/containerizer/capabilities_test_helper.hpp		\
  tests/containerizer/composing_containerizer_tests.cpp		\
  tests/containerizer/containerizer_tests.cpp			\
  tests/containerizer/cpu_isolator_tests.cpp			\
  tests/containerizer/docker_archive.hpp			\
  tests/containerizer/docker_common.hpp				\
  tests/containerizer/docker_containerizer_tests.cpp		\
  tests/containerizer/docker_spec_tests.cpp			\
  tests/containerizer/docker_tests.cpp				\
  tests/containerizer/environment_secret_isolator_tests.cpp	\
  tests/containerizer/io_switchboard_tests.cpp			\
  tests/containerizer/isolator.hpp				\
  tests/containerizer/isolator_tests.cpp			\
  tests/containerizer/launcher.cpp				\
  tests/containerizer/launcher.hpp				\
  tests/containerizer/memory_isolator_tests.cpp			\
  tests/containerizer/memory_test_helper.cpp			\
  tests/containerizer/memory_test_helper.hpp			\
  tests/containerizer/mesos_containerizer_paths_tests.cpp	\
  tests/containerizer/mesos_containerizer_tests.cpp		\
  tests/containerizer/mock_containerizer.hpp			\
  tests/containerizer/oci_spec_tests.cpp			\
  tests/containerizer/posix_rlimits_isolator_tests.cpp		\
  tests/containerizer/provisioner_appc_tests.cpp		\
  tests/containerizer/provisioner_backend_tests.cpp		\
  tests/containerizer/provisioner_docker_tests.cpp		\
  tests/containerizer/provisioner_paths_tests.cpp		\
  tests/containerizer/rootfs.hpp				\
  tests/containerizer/setns_test_helper.hpp			\
  tests/containerizer/volume_sandbox_path_isolator_tests.cpp

if ENABLE_XFS_DISK_ISOLATOR
mesos_tests_SOURCES +=						\
  tests/containerizer/xfs_quota_tests.cpp
endif

if ENABLE_SECCOMP_ISOLATOR
mesos_tests_SOURCES +=						\
  tests/containerizer/linux_seccomp_parser_tests.cpp		\
  tests/containerizer/linux_seccomp_isolator_tests.cpp
endif

mesos_tests_CPPFLAGS = $(MESOS_CPPFLAGS)
mesos_tests_CPPFLAGS += -DSOURCE_DIR=\"$(abs_top_srcdir)\"
mesos_tests_CPPFLAGS += -DBUILD_DIR=\"$(abs_top_builddir)\"
mesos_tests_CPPFLAGS += -I../$(GTEST)/include
mesos_tests_CPPFLAGS += -I../$(GMOCK)/include
mesos_tests_CPPFLAGS += -DTESTLIBEXECDIR=\"$(testlibexecdir)\"
mesos_tests_CPPFLAGS += -DSBINDIR=\"$(sbindir)\"

mesos_tests_LDADD =                                             \
  ../3rdparty/libgmock.la                                       \
  libmesos.la                                                   \
  liburi_disk_profile_adaptor.la                                \
  $(LIB_GRPC)                                                   \
  $(LDADD)

if !OS_FREEBSD
  mesos_tests_LDADD += -ldl # FreeBSD includes dynamic lib utils in libc.
endif

mesos_tests_DEPENDENCIES =					\
  ../3rdparty/libgmock.la					\
  libmesos.la							\
  liburi_disk_profile_adaptor.la                                \
  $(MESOS_TEST_MODULES)

if OS_LINUX
mesos_tests_SOURCES +=						\
  tests/agent_resource_provider_config_api_tests.cpp		\
  tests/ldcache_tests.cpp					\
  tests/ldd_tests.cpp						\
  tests/storage_local_resource_provider_tests.cpp		\
  tests/containerizer/linux_capabilities_isolator_tests.cpp	\
  tests/containerizer/capabilities_tests.cpp			\
  tests/containerizer/capabilities_test_helper.cpp		\
  tests/containerizer/cgroups_isolator_tests.cpp		\
  tests/containerizer/cgroups_tests.cpp				\
  tests/containerizer/cni_isolator_tests.cpp			\
  tests/containerizer/docker_volume_isolator_tests.cpp		\
  tests/containerizer/linux_devices_isolator_tests.cpp		\
  tests/containerizer/linux_filesystem_isolator_tests.cpp	\
  tests/containerizer/linux_nnp_isolator_tests.cpp		\
  tests/containerizer/fs_tests.cpp				\
  tests/containerizer/memory_pressure_tests.cpp			\
  tests/containerizer/nested_mesos_containerizer_tests.cpp	\
  tests/containerizer/ns_tests.cpp				\
  tests/containerizer/nvidia_gpu_isolator_tests.cpp		\
  tests/containerizer/perf_tests.cpp				\
  tests/containerizer/rootfs.cpp				\
  tests/containerizer/runtime_isolator_tests.cpp		\
  tests/containerizer/sched_tests.cpp				\
  tests/containerizer/setns_test_helper.cpp			\
  tests/containerizer/volume_csi_isolator_tests.cpp		\
  tests/containerizer/volume_host_path_isolator_tests.cpp	\
  tests/containerizer/volume_image_isolator_tests.cpp		\
  tests/containerizer/volume_secret_isolator_tests.cpp
endif

if ENABLE_LAUNCHER_SEALING
mesos_tests_SOURCES +=						\
  tests/containerizer/linux_memfd_tests.cpp
endif

if ENABLE_LINUX_ROUTING
mesos_tests_SOURCES +=						\
  tests/containerizer/routing_tests.cpp
endif

if ENABLE_NETWORK_PORTS_ISOLATOR
mesos_tests_SOURCES +=						\
  tests/containerizer/ports_isolator_tests.cpp
endif

if ENABLE_PORT_MAPPING_ISOLATOR
mesos_tests_SOURCES +=						\
  tests/containerizer/port_mapping_tests.cpp
endif

if ENABLE_SSL
mesos_tests_SOURCES +=						\
  tests/secret_generator_tests.cpp
endif

if HAS_JAVA
mesos_tests_SOURCES +=						\
  tests/group_tests.cpp						\
  tests/registrar_zookeeper_tests.cpp				\
  tests/zookeeper.cpp						\
  tests/zookeeper_test_server.cpp				\
  tests/zookeeper_tests.cpp

mesos_tests_CPPFLAGS += $(JAVA_CPPFLAGS)
mesos_tests_CPPFLAGS += -DZOOKEEPER_VERSION=\"$(ZOOKEEPER_VERSION)\"
mesos_tests_DEPENDENCIES += $(EXAMPLES_JAR)

EXAMPLESCRIPTSJAVA =						\
  examples/java/test-executor					\
  examples/java/test-exception-framework			\
  examples/java/test-framework					\
  examples/java/test-log					\
  examples/java/test-multiple-executors-framework		\
  examples/java/v1-test-framework

check_SCRIPTS += $(EXAMPLESCRIPTSJAVA)
mesos_tests_DEPENDENCIES += $(EXAMPLESCRIPTSJAVA)
endif

if HAS_PYTHON
mesos_tests_DEPENDENCIES += $(MESOS_EGG)

EXAMPLESCRIPTSPYTHON =						\
  examples/python/test_executor.py				\
  examples/python/test-executor					\
  examples/python/test_framework.py				\
  examples/python/test-framework

check_SCRIPTS += $(EXAMPLESCRIPTSPYTHON)
mesos_tests_DEPENDENCIES += $(EXAMPLESCRIPTSPYTHON)
endif

EXTRA_DIST +=							\
  examples/python/test_executor.py				\
  examples/python/test_framework.py


dist_check_SCRIPTS +=						\
  tests/balloon_framework_test.sh				\
  tests/disk_full_framework_test.sh				\
  tests/dynamic_reservation_framework_test.sh			\
  tests/java_exception_test.sh					\
  tests/java_framework_test.sh					\
  tests/java_log_test.sh					\
  tests/java_v0_framework_test.sh				\
  tests/java_v1_framework_test.sh				\
  tests/no_executor_framework_test.sh				\
  tests/operation_feedback_framework_test.sh			\
  tests/persistent_volume_framework_test.sh			\
  tests/python_framework_test.sh				\
  tests/test_http_framework_test.sh				\
  tests/test_framework_test.sh

../3rdparty/libgmock.la:
	@cd ../3rdparty && $(MAKE) $(AM_MAKEFLAGS) libgmock.la

if INSTALL_TESTS
testlibexec_PROGRAMS = $(check_PROGRAMS)
dist_testlibexec_SCRIPTS =			\
  tests/balloon_framework_test.sh		\
  tests/disk_full_framework_test.sh		\
  tests/java_exception_test.sh			\
  tests/java_framework_test.sh			\
  tests/java_log_test.sh			\
  tests/no_executor_framework_test.sh		\
  tests/persistent_volume_framework_test.sh	\
  tests/python_framework_test.sh		\
  tests/test_http_framework_test.sh		\
  tests/test_framework_test.sh

endif

# This target will do the following two things:
# 1. Copy mesos-agent-env.sh.template to mesos-slave-env.sh.template.
# 2. Create "slave->agent" symlink in the agent headers installation
#    path.
#
# TODO(tomxing): Remove this target once the
# slave->agent rename is complete (MESOS-3782 & MESOS-5230).
copy-template-and-create-symlink:
	cd $(DESTDIR)/$(pkgsysconfdir) && \
	  $(LN_S) mesos-agent-env.sh.template mesos-slave-env.sh.template
	cd $(DESTDIR)/$(includedir)/mesos && $(LN_S) agent slave

PHONY_TARGETS += copy-template-and-create-symlink

# Install compatibility symlinks for modules that used to be in $(LIBDIR)
# but are now in $(PKGMODULEDIR). We use install-data-hook because it
# runs late in the install process after the target directories have
# been created.
install-data-hook: copy-template-and-create-symlink
	cd $(DESTDIR)/$(libdir) && 				\
	for name in libfixed_resource_estimator 		\
	    libload_qos_controller 				\
	    liblogrotate_container_logger; do 			\
		for lib in `cd $(DESTDIR)/$(pkgmoduledir) && ls $${name}*`; do \
		  rm -f $$lib; 					\
		  $(LN_S) $(relative_pkgmoduledir)/$$lib $$lib; \
		done;						\
	done

install-exec-hook:
	cd $(DESTDIR)/$(sbindir) && $(LN_S) mesos-agent mesos-slave

# TODO(tomxing): Remove this hook once the
# slave->agent rename is complete(MESOS-3782 & MESOS 5230).
uninstall-hook:
	rm -f $(DESTDIR)/$(pkgsysconfdir)/mesos-slave-env.sh.template
	rm -f $(DESTDIR)/$(includedir)/mesos/slave
	rm -f $(DESTDIR)/$(sbindir)/mesos-slave

# We use a check-local target for now to avoid the parallel test
# runner that ships with newer versions of autotools.
# See the following discussion for the workaround:
# http://lists.gnu.org/archive/html/automake/2013-01/msg00051.html
check-local: tests
	$(TEST_DRIVER) ./mesos-tests

if ENABLE_NEW_CLI
	source $(builddir)/.virtualenv/bin/activate;		\
	source $(builddir)/.virtualenv/bin/postactivate;	\
	PATH=$(builddir):$$PATH mesos-cli-tests

	tox -c $(srcdir)/python/lib/tox.ini
endif

if INSTALL_TESTS
# If we enabled test installation, we can run the tests from the
# installation tree.
installcheck-local:
	$(testlibexecdir)/mesos-tests --build_dir=/nowhere --source_dir=/nowhere
endif

clean-local: clean-java clean-python

tests: all $(check_PROGRAMS) mesos-tests

.PHONY: $(PHONY_TARGETS)
