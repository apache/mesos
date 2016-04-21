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
include ../3rdparty/libprocess/3rdparty/versions.am

# TODO(charles): Move these into an included automakefile and have
# them include $(top_builddir) as appropriate.
DISTRIBUTE = 3rdparty/distribute-$(DISTRIBUTE_VERSION)
PIP = 3rdparty/pip-$(PIP_VERSION)
WHEEL = 3rdparty/wheel-$(WHEEL_VERSION)
LEVELDB = 3rdparty/leveldb-$(LEVELDB_VERSION)
ZOOKEEPER = 3rdparty/zookeeper-$(ZOOKEEPER_VERSION)/src/c
ZOOKEEPER_JAR = 3rdparty/zookeeper-$(ZOOKEEPER_VERSION)/zookeeper-$(ZOOKEEPER_VERSION).jar
LIBPROCESS = 3rdparty/libprocess
STOUT = $(LIBPROCESS)/3rdparty/stout
BOOST = $(LIBPROCESS)/3rdparty/boost-$(BOOST_VERSION)
GLOG = $(LIBPROCESS)/3rdparty/glog-$(GLOG_VERSION)
GMOCK = $(LIBPROCESS)/3rdparty/gmock-$(GMOCK_VERSION)
GTEST = $(GMOCK)/gtest
PROTOBUF = $(LIBPROCESS)/3rdparty/protobuf-$(PROTOBUF_VERSION)
PICOJSON = $(LIBPROCESS)/3rdparty/picojson-$(PICOJSON_VERSION)


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

if WITH_BUNDLED_PROTOBUF
# Protocol buffer compiler.
PROTOC = ../$(PROTOBUF)/src/protoc
else
PROTOC = @PROTOCOMPILER@
endif

PROTOCFLAGS = -I$(top_srcdir)/include -I$(srcdir)

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

# Use -Wl,--as-needed to AM_LDFLAGS since we link against all the
# libraries programs may depend upon, not the exact one. --as-needed
# causes the linker to only link to the ones a program actually uses,
# not the full list on the command line.
if OS_LINUX
AM_LDFLAGS=-Wl,--as-needed
endif

# Add compiler and linker flags for pthreads.
AM_CXXFLAGS = $(PTHREAD_CFLAGS)
AM_LIBS = $(PTHREAD_LIBS)

# Setup CPPFLAGS that are used for most source files.
MESOS_CPPFLAGS = $(AM_CPPFLAGS)
MESOS_CPPFLAGS += -Wall -Werror
MESOS_CPPFLAGS += -DLIBDIR=\"$(libdir)\"
MESOS_CPPFLAGS += -DPKGLIBEXECDIR=\"$(pkglibexecdir)\"
MESOS_CPPFLAGS += -DPKGDATADIR=\"$(pkgdatadir)\"
MESOS_CPPFLAGS += -DPKGMODULEDIR=\"$(pkgmoduledir)\"
MESOS_CPPFLAGS += -I$(top_srcdir)/include
MESOS_CPPFLAGS += -I$(top_srcdir)/$(LIBPROCESS)/include
MESOS_CPPFLAGS += -I$(top_srcdir)/$(STOUT)/include
MESOS_CPPFLAGS += -I../include

# Protobuf headers that depend on mesos.pb.h need this.
MESOS_CPPFLAGS += -I../include/mesos

# Header only dependencies will be ignored for --disable-bundled.
#
# We use "-isystem" instead of "-I" to add Boost to the include search
# path. This disables compiler warnings inside Boost headers since we
# can't easily fix them. See MESOS-3799.
MESOS_CPPFLAGS += -isystem ../$(BOOST)
MESOS_CPPFLAGS += -I../$(PICOJSON)

# Enable the INT64 support for PicoJson.
# NOTE: PicoJson requires __STDC_FORMAT_MACROS to be defined before importing
# 'inttypes.h'.  Since other libraries may also import this header, it must
# be globally defined so that PicoJson has access to the macros, regardless
# of the order of inclusion.
MESOS_CPPFLAGS += -DPICOJSON_USE_INT64
MESOS_CPPFLAGS += -D__STDC_FORMAT_MACROS

MESOS_CPPFLAGS += -I../$(PROTOBUF)/src
MESOS_CPPFLAGS += -I../$(GLOG)/src

if WITH_BUNDLED_GLOG
MESOS_CPPFLAGS += -I../$(GLOG)/src
endif

if WITH_BUNDLED_LEVELDB
MESOS_CPPFLAGS += -I../$(LEVELDB)/include
endif

if WITH_BUNDLED_ZOOKEEPER
MESOS_CPPFLAGS += -I../$(ZOOKEEPER)/include
MESOS_CPPFLAGS += -I../$(ZOOKEEPER)/generated
endif

if WITH_BUNDLED_PROTOBUF
MESOS_CPPFLAGS += -I../$(PROTOBUF)/src
endif

# README: we build the Mesos library out of a collection of
# convenience libraries (that is, libraries that do not get installed
# but we can use as building blocks to vary compile flags as necessary
# and then aggregate into final archives): libmesos_no_3rdparty.la
# libbuild.la, liblog.la, libjava.la.

# First, let's define necessary protocol buffer files.

ACLS_PROTO = $(top_srcdir)/include/mesos/authorizer/acls.proto
ALLOCATOR_PROTO = $(top_srcdir)/include/mesos/master/allocator.proto
APPC_SPEC_PROTO = $(top_srcdir)/include/mesos/appc/spec.proto
AUTHENTICATION_PROTO = $(top_srcdir)/include/mesos/authentication/authentication.proto
AUTHORIZATION_PROTO = $(top_srcdir)/include/mesos/authorizer/authorizer.proto
CONTAINERIZER_PROTO = $(top_srcdir)/include/mesos/containerizer/containerizer.proto
DOCKER_SPEC_PROTO = $(top_srcdir)/include/mesos/docker/spec.proto
DOCKER_V1_PROTO = $(top_srcdir)/include/mesos/docker/v1.proto
DOCKER_V2_PROTO = $(top_srcdir)/include/mesos/docker/v2.proto
EXECUTOR_PROTO = $(top_srcdir)/include/mesos/executor/executor.proto
FETCHER_PROTO = $(top_srcdir)/include/mesos/fetcher/fetcher.proto
MAINTENANCE_PROTO = $(top_srcdir)/include/mesos/maintenance/maintenance.proto
MESOS_PROTO = $(top_srcdir)/include/mesos/mesos.proto
MODULE_PROTO = $(top_srcdir)/include/mesos/module/module.proto
QUOTA_PROTO = $(top_srcdir)/include/mesos/quota/quota.proto
SCHEDULER_PROTO = $(top_srcdir)/include/mesos/scheduler/scheduler.proto
STATE_PROTO = $(top_srcdir)/include/mesos/state/state.proto
ISOLATOR_PROTO = $(top_srcdir)/include/mesos/slave/isolator.proto
OVERSUBSCRIPTION_PROTO = $(top_srcdir)/include/mesos/slave/oversubscription.proto
URI_PROTO = $(top_srcdir)/include/mesos/uri/uri.proto
V1_EXECUTOR_PROTO = $(top_srcdir)/include/mesos/v1/executor/executor.proto
V1_MESOS_PROTO = $(top_srcdir)/include/mesos/v1/mesos.proto
V1_SCHEDULER_PROTO = $(top_srcdir)/include/mesos/v1/scheduler/scheduler.proto


CXX_PROTOS =								\
  ../include/mesos/mesos.pb.cc						\
  ../include/mesos/mesos.pb.h						\
  ../include/mesos/appc/spec.pb.cc					\
  ../include/mesos/appc/spec.pb.h					\
  ../include/mesos/authentication/authentication.pb.cc			\
  ../include/mesos/authentication/authentication.pb.h			\
  ../include/mesos/authorizer/acls.pb.cc				\
  ../include/mesos/authorizer/acls.pb.h					\
  ../include/mesos/authorizer/authorizer.pb.cc				\
  ../include/mesos/authorizer/authorizer.pb.h				\
  ../include/mesos/containerizer/containerizer.pb.cc			\
  ../include/mesos/containerizer/containerizer.pb.h			\
  ../include/mesos/docker/spec.pb.cc					\
  ../include/mesos/docker/spec.pb.h					\
  ../include/mesos/docker/v1.pb.cc					\
  ../include/mesos/docker/v1.pb.h					\
  ../include/mesos/docker/v2.pb.cc					\
  ../include/mesos/docker/v2.pb.h					\
  ../include/mesos/executor/executor.pb.cc				\
  ../include/mesos/executor/executor.pb.h				\
  ../include/mesos/fetcher/fetcher.pb.cc				\
  ../include/mesos/fetcher/fetcher.pb.h					\
  ../include/mesos/maintenance/maintenance.pb.cc			\
  ../include/mesos/maintenance/maintenance.pb.h				\
  ../include/mesos/master/allocator.pb.cc				\
  ../include/mesos/master/allocator.pb.h				\
  ../include/mesos/module/module.pb.cc					\
  ../include/mesos/module/module.pb.h					\
  ../include/mesos/quota/quota.pb.cc					\
  ../include/mesos/quota/quota.pb.h					\
  ../include/mesos/scheduler/scheduler.pb.cc				\
  ../include/mesos/scheduler/scheduler.pb.h				\
  ../include/mesos/slave/isolator.pb.cc					\
  ../include/mesos/slave/isolator.pb.h					\
  ../include/mesos/slave/oversubscription.pb.cc				\
  ../include/mesos/slave/oversubscription.pb.h				\
  ../include/mesos/state/state.pb.cc					\
  ../include/mesos/state/state.pb.h					\
  ../include/mesos/uri/uri.pb.cc					\
  ../include/mesos/uri/uri.pb.h						\
  ../include/mesos/v1/mesos.pb.cc					\
  ../include/mesos/v1/mesos.pb.h					\
  ../include/mesos/v1/executor/executor.pb.cc				\
  ../include/mesos/v1/executor/executor.pb.h				\
  ../include/mesos/v1/scheduler/scheduler.pb.cc				\
  ../include/mesos/v1/scheduler/scheduler.pb.h

CXX_PROTOS +=								\
  master/registry.pb.cc							\
  master/registry.pb.h							\
  messages/flags.pb.cc							\
  messages/flags.pb.h							\
  messages/messages.pb.cc						\
  messages/messages.pb.h						\
  slave/containerizer/mesos/provisioner/docker/message.pb.cc		\
  slave/containerizer/mesos/provisioner/docker/message.pb.h		\
  slave/containerizer/mesos/isolators/docker/volume/state.pb.cc		\
  slave/containerizer/mesos/isolators/docker/volume/state.pb.h		\
  slave/containerizer/mesos/isolators/network/cni/spec.pb.cc		\
  slave/containerizer/mesos/isolators/network/cni/spec.pb.h

CXX_LOG_PROTOS =							\
  messages/log.pb.cc							\
  messages/log.pb.h

CXX_STATE_PROTOS =							\
  messages/state.pb.cc							\
  messages/state.pb.h

JAVA_PROTOS =								\
  java/generated/org/apache/mesos/Protos.java				\
  java/generated/org/apache/mesos/containerizer/Protos.java

PYTHON_PROTOS =								\
  python/interface/src/mesos/interface/mesos_pb2.py			\
  python/interface/src/mesos/interface/containerizer_pb2.py

V1_JAVA_PROTOS =							\
  java/generated/org/apache/mesos/v1/Protos.java			\
  java/generated/org/apache/mesos/v1/scheduler/Protos.java

V1_PYTHON_PROTOS =							\
  python/interface/src/mesos/v1/interface/mesos_pb2.py			\
  python/interface/src/mesos/v1/interface/scheduler_pb2.py


BUILT_SOURCES +=							\
  $(CXX_LOG_PROTOS)							\
  $(CXX_PROTOS)								\
  $(CXX_STATE_PROTOS)							\
  $(JAVA_PROTOS)							\
  $(PYTHON_PROTOS)							\
  $(V1_JAVA_PROTOS)							\
  $(V1_PYTHON_PROTOS)

CLEANFILES +=								\
  $(CXX_LOG_PROTOS)							\
  $(CXX_PROTOS)								\
  $(CXX_STATE_PROTOS)							\
  $(JAVA_PROTOS)							\
  $(PYTHON_PROTOS)							\
  $(V1_JAVA_PROTOS)							\
  $(V1_PYTHON_PROTOS)


# Targets for generating C++ protocol buffer code.
../include/mesos/%.pb.cc ../include/mesos/%.pb.h: $(top_srcdir)/include/mesos/%.proto
	$(PROTOC) $(PROTOCFLAGS) --cpp_out=../include $^

%.pb.cc %.pb.h: %.proto
	$(PROTOC) $(PROTOCFLAGS) --cpp_out=. $^


# Targets for generating Java protocol buffer code.
java/generated/org/apache/mesos/containerizer/Protos.java: $(CONTAINERIZER_PROTO)
	$(MKDIR_P) $(@D)
	$(PROTOC) $(PROTOCFLAGS) --java_out=java/generated $^

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

java/generated/org/apache/mesos/v1/executor/Protos.java: $(V1_EXECUTOR_PROTO)
	$(MKDIR_P) $(@D)
	$(PROTOC) $(PROTOCFLAGS) --java_out=java/generated $^

java/generated/org/apache/mesos/v1/Protos.java: $(V1_MESOS_PROTO)
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
python/interface/src/mesos/interface/containerizer_pb2.py: $(CONTAINERIZER_PROTO)
	$(MKDIR_P) $(@D)
	$(PROTOC) -I$(top_srcdir)/include/mesos/containerizer $(PROTOCFLAGS)	\
		--python_out=python/interface/src/mesos/interface $^
	$(SED) -i.bak 's/mesos\.mesos_pb2/mesos_pb2/' $@ && rm $@.bak

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

python/interface/src/mesos/v1/interface/executor_pb2.py: $(V1_EXECUTOR_PROTO)
	$(MKDIR_P) $(@D)
	$(PROTOC) -I$(top_srcdir)/include/mesos/v1/executor $(PROTOCFLAGS)	\
		--python_out=python/interface/src/mesos/v1/interface $^
	$(SED) -i.bak 's/mesos\.mesos_pb2/mesos_pb2/' $@ && rm $@.bak

python/interface/src/mesos/v1/interface/mesos_pb2.py: $(V1_MESOS_PROTO)
	$(MKDIR_P) $(@D)
	$(PROTOC) -I$(top_srcdir)/include/mesos/v1 $(PROTOCFLAGS)		\
		--python_out=python/interface/src/mesos/v1/interface $^

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
  $(top_srcdir)/include/mesos/roles.hpp					\
  $(top_srcdir)/include/mesos/scheduler.hpp				\
  $(top_srcdir)/include/mesos/type_utils.hpp				\
  $(top_srcdir)/include/mesos/values.hpp

nodist_pkginclude_HEADERS =						\
  ../include/mesos/version.hpp						\
  ../include/mesos/mesos.pb.h

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
  $(top_srcdir)/include/mesos/authentication/http/basic_authenticator_factory.hpp

nodist_authentication_HEADERS =						\
  ../include/mesos/authentication/authentication.pb.h

authorizerdir = $(pkgincludedir)/authorizer

authorizer_HEADERS =							\
  $(top_srcdir)/include/mesos/authorizer/acls.hpp			\
  $(top_srcdir)/include/mesos/authorizer/acls.proto			\
  $(top_srcdir)/include/mesos/authorizer/authorizer.hpp			\
  $(top_srcdir)/include/mesos/authorizer/authorizer.proto

nodist_authorizer_HEADERS =						\
  ../include/mesos/authorizer/acls.pb.h					\
  ../include/mesos/authorizer/authorizer.pb.h

containerizerdir = $(pkgincludedir)/containerizer

containerizer_HEADERS =							\
  $(top_srcdir)/include/mesos/containerizer/containerizer.hpp		\
  $(top_srcdir)/include/mesos/containerizer/containerizer.proto

nodist_containerizer_HEADERS =						\
  ../include/mesos/containerizer/containerizer.pb.h

dockerdir = $(pkgincludedir)/docker

docker_HEADERS =							\
  $(top_srcdir)/include/mesos/docker/spec.hpp				\
  $(top_srcdir)/include/mesos/docker/spec.proto				\
  $(top_srcdir)/include/mesos/docker/v1.hpp				\
  $(top_srcdir)/include/mesos/docker/v1.proto				\
  $(top_srcdir)/include/mesos/docker/v2.hpp				\
  $(top_srcdir)/include/mesos/docker/v2.proto

nodist_docker_HEADERS =							\
  ../include/mesos/docker/spec.pb.h					\
  ../include/mesos/docker/v1.pb.h					\
  ../include/mesos/docker/v2.pb.h

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
  $(top_srcdir)/include/mesos/master/allocator.hpp			\
  $(top_srcdir)/include/mesos/master/allocator.proto			\
  $(top_srcdir)/include/mesos/master/contender.hpp			\
  $(top_srcdir)/include/mesos/master/detector.hpp

nodist_master_HEADERS =							\
  ../include/mesos/master/allocator.pb.h

moduledir = $(pkgincludedir)/module

module_HEADERS =							\
  $(top_srcdir)/include/mesos/module/allocator.hpp			\
  $(top_srcdir)/include/mesos/module/anonymous.hpp			\
  $(top_srcdir)/include/mesos/module/authenticatee.hpp			\
  $(top_srcdir)/include/mesos/module/authenticator.hpp			\
  $(top_srcdir)/include/mesos/module/authorizer.hpp			\
  $(top_srcdir)/include/mesos/module/container_logger.hpp		\
  $(top_srcdir)/include/mesos/module/hook.hpp				\
  $(top_srcdir)/include/mesos/module/http_authenticator.hpp		\
  $(top_srcdir)/include/mesos/module/isolator.hpp			\
  $(top_srcdir)/include/mesos/module/module.hpp				\
  $(top_srcdir)/include/mesos/module/module.proto			\
  $(top_srcdir)/include/mesos/module/qos_controller.hpp			\
  $(top_srcdir)/include/mesos/module/resource_estimator.hpp		\
  $(top_srcdir)/include/mesos/module/contender.hpp			\
  $(top_srcdir)/include/mesos/module/detector.hpp

nodist_module_HEADERS =							\
  ../include/mesos/module/module.pb.h

quotadir = $(pkgincludedir)/quota

quota_HEADERS =								\
  $(top_srcdir)/include/mesos/quota/quota.hpp				\
  $(top_srcdir)/include/mesos/quota/quota.proto

nodist_quota_HEADERS =							\
  ../include/mesos/quota/quota.pb.h

schedulerdir = $(pkgincludedir)/scheduler

scheduler_HEADERS =							\
  $(top_srcdir)/include/mesos/scheduler/scheduler.hpp			\
  $(top_srcdir)/include/mesos/scheduler/scheduler.proto

nodist_scheduler_HEADERS =						\
  ../include/mesos/scheduler/scheduler.pb.h

slavedir = $(pkgincludedir)/slave

slave_HEADERS =								\
  $(top_srcdir)/include/mesos/slave/container_logger.hpp		\
  $(top_srcdir)/include/mesos/slave/isolator.hpp			\
  $(top_srcdir)/include/mesos/slave/isolator.proto			\
  $(top_srcdir)/include/mesos/slave/oversubscription.hpp		\
  $(top_srcdir)/include/mesos/slave/oversubscription.proto		\
  $(top_srcdir)/include/mesos/slave/qos_controller.hpp			\
  $(top_srcdir)/include/mesos/slave/resource_estimator.hpp

nodist_slave_HEADERS =							\
  ../include/mesos/slave/isolator.pb.h					\
  ../include/mesos/slave/oversubscription.pb.h

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
  $(top_srcdir)/include/mesos/v1/scheduler.hpp				\
  $(top_srcdir)/include/mesos/v1/values.hpp

nodist_v1_HEADERS =							\
  ../include/mesos/v1/mesos.pb.h

v1executordir = $(pkgincludedir)/v1/executor

v1executor_HEADERS =							\
  $(top_srcdir)/include/mesos/v1/executor/executor.hpp			\
  $(top_srcdir)/include/mesos/v1/executor/executor.proto

nodist_v1executor_HEADERS =						\
  ../include/mesos/v1/executor/executor.pb.h

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
# exclude third party libraries so setuptools/distribute can build a
# self-contained Python library and statically link in the third party
# libraries themselves.
noinst_LTLIBRARIES += libmesos_no_3rdparty.la

nodist_libmesos_no_3rdparty_la_SOURCES = $(CXX_PROTOS)


libmesos_no_3rdparty_la_SOURCES =					\
  master/registry.proto							\
  messages/flags.proto							\
  messages/messages.proto						\
  slave/containerizer/mesos/provisioner/docker/message.proto		\
  slave/containerizer/mesos/isolators/docker/volume/state.proto		\
  slave/containerizer/mesos/isolators/network/cni/spec.proto

# TODO(tillt): Remove authentication/cram_md5/* which will enable us to
# lose the immediate cyrus-sasl2 dependency.
libmesos_no_3rdparty_la_SOURCES +=					\
  appc/spec.cpp								\
  authentication/cram_md5/authenticatee.cpp				\
  authentication/cram_md5/authenticator.cpp				\
  authentication/cram_md5/auxprop.cpp					\
  authentication/http/basic_authenticator_factory.cpp			\
  authorizer/acls.cpp							\
  authorizer/authorizer.cpp						\
  authorizer/local/authorizer.cpp					\
  common/attributes.cpp							\
  common/command_utils.cpp						\
  common/http.cpp							\
  common/protobuf_utils.cpp						\
  common/resources.cpp							\
  common/resources_utils.cpp						\
  common/roles.cpp							\
  common/type_utils.cpp							\
  common/values.cpp							\
  docker/docker.cpp							\
  docker/spec.cpp							\
  exec/exec.cpp								\
  executor/executor.cpp							\
  files/files.cpp							\
  hdfs/hdfs.cpp								\
  hook/manager.cpp							\
  internal/devolve.cpp							\
  internal/evolve.cpp							\
  local/local.cpp							\
  logging/flags.cpp							\
  logging/logging.cpp							\
  master/flags.cpp							\
  master/http.cpp							\
  master/maintenance.cpp						\
  master/master.cpp							\
  master/metrics.cpp							\
  master/quota.cpp							\
  master/quota_handler.cpp						\
  master/registrar.cpp							\
  master/repairer.cpp							\
  master/validation.cpp							\
  master/weights.cpp							\
  master/weights_handler.cpp						\
  master/allocator/allocator.cpp					\
  master/allocator/mesos/hierarchical.cpp				\
  master/allocator/mesos/metrics.cpp					\
  master/allocator/sorter/drf/metrics.cpp				\
  master/allocator/sorter/drf/sorter.cpp				\
  master/contender/contender.cpp					\
  master/contender/standalone.cpp					\
  master/contender/zookeeper.cpp					\
  master/detector/detector.cpp						\
  master/detector/standalone.cpp					\
  master/detector/zookeeper.cpp						\
  messages/messages.cpp							\
  module/manager.cpp							\
  sched/sched.cpp							\
  scheduler/scheduler.cpp						\
  slave/constants.cpp							\
  slave/container_logger.cpp						\
  slave/flags.cpp							\
  slave/gc.cpp								\
  slave/http.cpp							\
  slave/metrics.cpp							\
  slave/paths.cpp							\
  slave/qos_controller.cpp						\
  slave/qos_controllers/noop.cpp					\
  slave/resource_estimator.cpp						\
  slave/slave.cpp							\
  slave/state.cpp							\
  slave/status_update_manager.cpp					\
  slave/validation.cpp							\
  slave/container_loggers/sandbox.cpp					\
  slave/containerizer/composing.cpp					\
  slave/containerizer/containerizer.cpp					\
  slave/containerizer/docker.cpp					\
  slave/containerizer/external_containerizer.cpp			\
  slave/containerizer/fetcher.cpp					\
  slave/containerizer/mesos/containerizer.cpp				\
  slave/containerizer/mesos/isolator.cpp				\
  slave/containerizer/mesos/launch.cpp					\
  slave/containerizer/mesos/launcher.cpp				\
  slave/containerizer/mesos/mount.cpp					\
  slave/containerizer/mesos/isolators/filesystem/posix.cpp		\
  slave/containerizer/mesos/isolators/docker/volume/driver.cpp		\
  slave/containerizer/mesos/isolators/docker/volume/paths.cpp		\
  slave/containerizer/mesos/isolators/network/cni/paths.cpp		\
  slave/containerizer/mesos/isolators/network/cni/spec.cpp		\
  slave/containerizer/mesos/isolators/posix/disk.cpp			\
  slave/containerizer/mesos/provisioner/backend.cpp			\
  slave/containerizer/mesos/provisioner/paths.cpp			\
  slave/containerizer/mesos/provisioner/provisioner.cpp			\
  slave/containerizer/mesos/provisioner/store.cpp			\
  slave/containerizer/mesos/provisioner/appc/cache.cpp			\
  slave/containerizer/mesos/provisioner/appc/fetcher.cpp		\
  slave/containerizer/mesos/provisioner/appc/paths.cpp			\
  slave/containerizer/mesos/provisioner/appc/store.cpp			\
  slave/containerizer/mesos/provisioner/backends/copy.cpp		\
  slave/containerizer/mesos/provisioner/docker/local_puller.cpp		\
  slave/containerizer/mesos/provisioner/docker/metadata_manager.cpp	\
  slave/containerizer/mesos/provisioner/docker/paths.cpp		\
  slave/containerizer/mesos/provisioner/docker/puller.cpp		\
  slave/containerizer/mesos/provisioner/docker/registry_puller.cpp	\
  slave/containerizer/mesos/provisioner/docker/store.cpp		\
  slave/resource_estimators/noop.cpp					\
  uri/fetcher.cpp							\
  uri/utils.cpp								\
  uri/fetchers/copy.cpp							\
  uri/fetchers/curl.cpp							\
  uri/fetchers/docker.cpp						\
  uri/fetchers/hadoop.cpp						\
  usage/usage.cpp							\
  v1/attributes.cpp							\
  v1/mesos.cpp								\
  v1/resources.cpp							\
  v1/values.cpp								\
  version/version.cpp							\
  watcher/whitelist_watcher.cpp						\
  zookeeper/authentication.cpp						\
  zookeeper/contender.cpp						\
  zookeeper/detector.cpp						\
  zookeeper/zookeeper.cpp						\
  zookeeper/group.cpp

libmesos_no_3rdparty_la_SOURCES +=					\
  authentication/cram_md5/authenticatee.hpp				\
  authentication/cram_md5/authenticator.hpp				\
  authentication/cram_md5/auxprop.hpp					\
  authorizer/local/authorizer.hpp					\
  common/build.hpp							\
  common/command_utils.hpp						\
  common/http.hpp							\
  common/parse.hpp							\
  common/protobuf_utils.hpp						\
  common/recordio.hpp							\
  common/resources_utils.hpp						\
  common/status_utils.hpp						\
  credentials/credentials.hpp						\
  docker/docker.hpp							\
  docker/executor.hpp							\
  examples/test_anonymous_module.hpp					\
  examples/test_module.hpp						\
  examples/utils.hpp							\
  files/files.hpp							\
  hdfs/hdfs.hpp								\
  hook/manager.hpp							\
  internal/devolve.hpp							\
  internal/evolve.hpp							\
  local/flags.hpp							\
  local/local.hpp							\
  logging/flags.hpp							\
  logging/logging.hpp							\
  master/constants.hpp							\
  master/flags.hpp							\
  master/machine.hpp							\
  master/maintenance.hpp						\
  master/master.hpp							\
  master/metrics.hpp							\
  master/quota.hpp							\
  master/registrar.hpp							\
  master/registry.hpp							\
  master/repairer.hpp							\
  master/validation.hpp							\
  master/weights.hpp							\
  master/allocator/mesos/allocator.hpp					\
  master/allocator/mesos/hierarchical.hpp				\
  master/allocator/mesos/metrics.hpp					\
  master/allocator/sorter/sorter.hpp					\
  master/allocator/sorter/drf/metrics.hpp				\
  master/allocator/sorter/drf/sorter.hpp				\
  master/contender/standalone.hpp					\
  master/contender/zookeeper.hpp					\
  master/detector/standalone.hpp					\
  master/detector/zookeeper.hpp						\
  messages/flags.hpp							\
  messages/messages.hpp							\
  module/manager.hpp							\
  sched/constants.hpp							\
  sched/flags.hpp							\
  slave/constants.hpp							\
  slave/flags.hpp							\
  slave/gc.hpp								\
  slave/metrics.hpp							\
  slave/paths.hpp							\
  slave/slave.hpp							\
  slave/state.hpp							\
  slave/status_update_manager.hpp					\
  slave/validation.hpp							\
  slave/container_loggers/sandbox.hpp					\
  slave/containerizer/composing.hpp					\
  slave/containerizer/containerizer.hpp					\
  slave/containerizer/docker.hpp					\
  slave/containerizer/external_containerizer.hpp			\
  slave/containerizer/fetcher.hpp					\
  slave/containerizer/mesos/containerizer.hpp				\
  slave/containerizer/mesos/isolator.hpp				\
  slave/containerizer/mesos/launch.hpp					\
  slave/containerizer/mesos/launcher.hpp				\
  slave/containerizer/mesos/mount.hpp					\
  slave/containerizer/mesos/isolators/posix.hpp				\
  slave/containerizer/mesos/isolators/filesystem/posix.hpp		\
  slave/containerizer/mesos/isolators/posix/disk.hpp			\
  slave/containerizer/mesos/isolators/docker/volume/driver.hpp		\
  slave/containerizer/mesos/isolators/docker/volume/paths.hpp		\
  slave/containerizer/mesos/isolators/docker/volume/state.hpp		\
  slave/containerizer/mesos/isolators/network/cni/paths.hpp		\
  slave/containerizer/mesos/isolators/network/cni/spec.hpp		\
  slave/containerizer/mesos/isolators/network/cni/spec.hpp		\
  slave/containerizer/mesos/provisioner/backend.hpp			\
  slave/containerizer/mesos/provisioner/paths.hpp			\
  slave/containerizer/mesos/provisioner/provisioner.hpp			\
  slave/containerizer/mesos/provisioner/store.hpp			\
  slave/containerizer/mesos/provisioner/appc/cache.hpp			\
  slave/containerizer/mesos/provisioner/appc/fetcher.hpp		\
  slave/containerizer/mesos/provisioner/appc/paths.hpp			\
  slave/containerizer/mesos/provisioner/appc/store.hpp			\
  slave/containerizer/mesos/provisioner/backends/copy.hpp		\
  slave/containerizer/mesos/provisioner/docker/local_puller.hpp		\
  slave/containerizer/mesos/provisioner/docker/message.hpp		\
  slave/containerizer/mesos/provisioner/docker/metadata_manager.hpp	\
  slave/containerizer/mesos/provisioner/docker/paths.hpp		\
  slave/containerizer/mesos/provisioner/docker/puller.hpp		\
  slave/containerizer/mesos/provisioner/docker/registry_puller.hpp	\
  slave/containerizer/mesos/provisioner/docker/store.hpp		\
  slave/qos_controllers/noop.hpp					\
  slave/resource_estimators/noop.hpp					\
  tests/allocator.hpp							\
  tests/cluster.hpp							\
  tests/containerizer.hpp						\
  tests/environment.hpp							\
  tests/flags.hpp							\
  tests/limiter.hpp							\
  tests/mesos.hpp							\
  tests/module.hpp							\
  tests/script.hpp							\
  tests/utils.hpp							\
  tests/zookeeper.hpp							\
  tests/zookeeper_test_server.hpp					\
  tests/containerizer/docker_archive.hpp				\
  tests/containerizer/isolator.hpp					\
  tests/containerizer/launcher.hpp					\
  tests/containerizer/memory_test_helper.hpp				\
  tests/containerizer/rootfs.hpp					\
  tests/containerizer/setns_test_helper.hpp				\
  tests/containerizer/store.hpp						\
  uri/fetcher.hpp							\
  uri/utils.hpp								\
  uri/fetchers/copy.hpp							\
  uri/fetchers/curl.hpp							\
  uri/fetchers/docker.hpp						\
  uri/fetchers/hadoop.hpp						\
  uri/schemes/docker.hpp						\
  uri/schemes/file.hpp							\
  uri/schemes/hdfs.hpp							\
  uri/schemes/http.hpp							\
  usage/usage.hpp							\
  version/version.hpp							\
  watcher/whitelist_watcher.hpp

MESOS_LINUX_FILES =							\
  linux/cgroups.cpp							\
  linux/fs.cpp								\
  linux/perf.cpp							\
  linux/systemd.cpp							\
  slave/containerizer/mesos/linux_launcher.cpp				\
  slave/containerizer/mesos/isolators/cgroups/cpushare.cpp		\
  slave/containerizer/mesos/isolators/cgroups/mem.cpp			\
  slave/containerizer/mesos/isolators/cgroups/net_cls.cpp		\
  slave/containerizer/mesos/isolators/cgroups/perf_event.cpp		\
  slave/containerizer/mesos/isolators/docker/runtime.cpp		\
  slave/containerizer/mesos/isolators/docker/volume/isolator.cpp	\
  slave/containerizer/mesos/isolators/filesystem/linux.cpp		\
  slave/containerizer/mesos/isolators/filesystem/shared.cpp		\
  slave/containerizer/mesos/isolators/namespaces/pid.cpp		\
  slave/containerizer/mesos/isolators/network/cni/cni.cpp		\
  slave/containerizer/mesos/provisioner/backends/bind.cpp		\
  slave/containerizer/mesos/provisioner/backends/overlay.cpp

MESOS_LINUX_FILES +=							\
  linux/cgroups.hpp							\
  linux/fs.hpp								\
  linux/ns.hpp								\
  linux/perf.hpp							\
  linux/sched.hpp							\
  linux/systemd.hpp							\
  slave/containerizer/mesos/linux_launcher.hpp				\
  slave/containerizer/mesos/isolators/cgroups/constants.hpp		\
  slave/containerizer/mesos/isolators/cgroups/cpushare.hpp		\
  slave/containerizer/mesos/isolators/cgroups/mem.hpp			\
  slave/containerizer/mesos/isolators/cgroups/net_cls.hpp		\
  slave/containerizer/mesos/isolators/cgroups/perf_event.hpp		\
  slave/containerizer/mesos/isolators/docker/runtime.hpp		\
  slave/containerizer/mesos/isolators/docker/volume/isolator.hpp	\
  slave/containerizer/mesos/isolators/filesystem/linux.hpp		\
  slave/containerizer/mesos/isolators/filesystem/shared.hpp		\
  slave/containerizer/mesos/isolators/namespaces/pid.hpp		\
  slave/containerizer/mesos/isolators/network/cni/cni.hpp		\
  slave/containerizer/mesos/provisioner/backends/bind.hpp		\
  slave/containerizer/mesos/provisioner/backends/overlay.hpp

if ENABLE_XFS_DISK_ISOLATOR
MESOS_LINUX_FILES +=                                                    \
  slave/containerizer/mesos/isolators/xfs/utils.cpp                     \
  slave/containerizer/mesos/isolators/xfs/utils.hpp                     \
  slave/containerizer/mesos/isolators/xfs/disk.cpp                      \
  slave/containerizer/mesos/isolators/xfs/disk.hpp
endif

MESOS_NETWORK_ISOLATOR_FILES =						\
  linux/routing/handle.cpp						\
  linux/routing/route.cpp						\
  linux/routing/utils.cpp						\
  linux/routing/diagnosis/diagnosis.cpp					\
  linux/routing/filter/basic.cpp					\
  linux/routing/filter/icmp.cpp						\
  linux/routing/filter/ip.cpp						\
  linux/routing/link/link.cpp						\
  linux/routing/queueing/fq_codel.cpp					\
  linux/routing/queueing/htb.cpp					\
  linux/routing/queueing/ingress.cpp					\
  slave/containerizer/mesos/isolators/network/port_mapping.cpp

MESOS_NETWORK_ISOLATOR_FILES +=						\
  linux/routing/handle.hpp						\
  linux/routing/internal.hpp						\
  linux/routing/route.hpp						\
  linux/routing/utils.hpp						\
  linux/routing/diagnosis/diagnosis.hpp					\
  linux/routing/filter/action.hpp					\
  linux/routing/filter/basic.hpp					\
  linux/routing/filter/filter.hpp					\
  linux/routing/filter/handle.hpp					\
  linux/routing/filter/icmp.hpp						\
  linux/routing/filter/internal.hpp					\
  linux/routing/filter/ip.hpp						\
  linux/routing/filter/priority.hpp					\
  linux/routing/link/internal.hpp					\
  linux/routing/link/link.hpp						\
  linux/routing/queueing/discipline.hpp					\
  linux/routing/queueing/fq_codel.hpp					\
  linux/routing/queueing/htb.hpp					\
  linux/routing/queueing/ingress.hpp					\
  linux/routing/queueing/internal.hpp					\
  linux/routing/queueing/statistics.hpp					\
  slave/containerizer/mesos/isolators/network/port_mapping.hpp

MESOS_NVIDIA_GPU_ISOLATOR_FILES =					\
  slave/containerizer/mesos/isolators/cgroups/devices/gpus/nvidia.cpp

MESOS_NVIDIA_GPU_ISOLATOR_FILES +=					\
  slave/containerizer/mesos/isolators/cgroups/devices/gpus/nvidia.hpp

if OS_LINUX
libmesos_no_3rdparty_la_SOURCES += $(MESOS_LINUX_FILES)
else
EXTRA_DIST += $(MESOS_LINUX_FILES)
endif

if WITH_NETWORK_ISOLATOR
libmesos_no_3rdparty_la_SOURCES += $(MESOS_NETWORK_ISOLATOR_FILES)
else
EXTRA_DIST += $(MESOS_NETWORK_ISOLATOR_FILES)
endif

if ENABLE_NVIDIA_GPU_SUPPORT
libmesos_no_3rdparty_la_SOURCES += $(MESOS_NVIDIA_GPU_ISOLATOR_FILES)
else
EXTRA_DIST += $(MESOS_NVIDIA_GPU_ISOLATOR_FILES)
endif

libmesos_no_3rdparty_la_CPPFLAGS = $(MESOS_CPPFLAGS)

libmesos_no_3rdparty_la_LIBADD = # Initialized to enable using +=.

# Convenience library that *always* gets rebuilt to ensure accurate info.
noinst_LTLIBRARIES += libbuild.la
libbuild_la_SOURCES = common/build.cpp
libbuild_la_CPPFLAGS = $(AM_CPPFLAGS)
libbuild_la_CPPFLAGS += $(MESOS_CPPFLAGS)
libbuild_la_CPPFLAGS += -DBUILD_DATE="\"$$(date '+%Y-%m-%d %H:%M:%S')\""
libbuild_la_CPPFLAGS += -DBUILD_TIME="\"$$(date '+%s')\""
libbuild_la_CPPFLAGS += -DBUILD_USER="\"$$USER\""
libbuild_la_CPPFLAGS += -DBUILD_JAVA_JVM_LIBRARY=\"$(JAVA_JVM_LIBRARY)\"

if GIT_REPO
BUILD_GIT_SHA=$$(sh -c 'cd $(top_srcdir); \
  SHA=`git log -n 1 --format=%H 2> /dev/null` && \
  echo -DBUILD_GIT_SHA=\"$$SHA\"')
libbuild_la_CPPFLAGS += $(BUILD_GIT_SHA)

BUILD_GIT_BRANCH=$$(sh -c 'cd $(top_srcdir); \
  BRANCH=`git symbolic-ref HEAD 2> /dev/null` && \
  echo -DBUILD_GIT_BRANCH=\"$$BRANCH\"')
libbuild_la_CPPFLAGS += $(BUILD_GIT_BRANCH)

BUILD_GIT_TAG=$$(sh -c 'cd $(top_srcdir); \
  TAG=`git describe --exact --tags 2> /dev/null` && \
  echo -DBUILD_GIT_TAG=\"$$TAG\"')
libbuild_la_CPPFLAGS += $(BUILD_GIT_TAG)
endif

# We need to escape the build flags properly.
BUILD_FLAGS = $(echo $(MESOS_CPPFLAGS) $(CPPFLAGS) | $(SED) 's/\"/\\\"/g')	\
              $(echo $(AM_CFLAGS) $(CFLAGS) | $(SED) 's/\"/\\\"/g')		\
              $(echo $(AM_CXXFLAGS) $(CXXFLAGS) | $(SED) 's/\"/\\\"/g')

# TODO(benh): Provide other build flags.

libbuild_la_CPPFLAGS += -DBUILD_FLAGS="\"$$BUILD_FLAGS\""

libmesos_no_3rdparty_la_LIBADD += libbuild.la


# Convenience library for building the replicated log in order to
# include the leveldb headers.
noinst_LTLIBRARIES += liblog.la
liblog_la_SOURCES =							\
  log/catchup.cpp							\
  log/consensus.cpp							\
  log/coordinator.cpp							\
  log/leveldb.cpp							\
  log/log.cpp								\
  log/recover.cpp							\
  log/replica.cpp							\
  log/tool/benchmark.cpp						\
  log/tool/initialize.cpp						\
  log/tool/read.cpp							\
  log/tool/replica.cpp
liblog_la_SOURCES +=							\
  log/catchup.hpp							\
  log/consensus.hpp							\
  log/coordinator.hpp							\
  log/leveldb.hpp							\
  log/log.hpp								\
  log/network.hpp							\
  log/recover.hpp							\
  log/replica.hpp							\
  log/storage.hpp							\
  log/tool.hpp								\
  log/tool/benchmark.hpp						\
  log/tool/initialize.hpp						\
  log/tool/read.hpp							\
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
  state/in_memory.cpp							\
  state/leveldb.cpp							\
  state/log.cpp								\
  state/zookeeper.cpp
libstate_la_SOURCES +=							\
  messages/state.hpp							\
  messages/state.proto
nodist_libstate_la_SOURCES = $(CXX_STATE_PROTOS)
libstate_la_CPPFLAGS = $(MESOS_CPPFLAGS)

libmesos_no_3rdparty_la_LIBADD += libstate.la


# The final result!
lib_LTLIBRARIES += libmesos.la

# Include as part of the distribution.
libmesos_la_SOURCES =							\
  $(ACLS_PROTO)								\
  $(ALLOCATOR_PROTO)							\
  $(APPC_SPEC_PROTO)							\
  $(AUTHENTICATION_PROTO)						\
  $(AUTHORIZATION_PROTO)						\
  $(CONTAINERIZER_PROTO)						\
  $(EXECUTOR_PROTO)							\
  $(DOCKER_SPEC_PROTO)							\
  $(DOCKER_V1_PROTO)							\
  $(DOCKER_V2_PROTO)							\
  $(FETCHER_PROTO)							\
  $(ISOLATOR_PROTO)							\
  $(MAINTENANCE_PROTO)							\
  $(MESOS_PROTO)							\
  $(MODULE_PROTO)							\
  $(OVERSUBSCRIPTION_PROTO)						\
  $(QUOTA_PROTO)							\
  $(SCHEDULER_PROTO)							\
  $(STATE_PROTO)							\
  $(URI_PROTO)								\
  $(V1_EXECUTOR_PROTO)							\
  $(V1_MESOS_PROTO)							\
  $(V1_SCHEDULER_PROTO)

libmesos_la_LDFLAGS = -release $(PACKAGE_VERSION)

# Since we just include the convenience library (and no sources), we
# need to tell libtool to build this as a C++ library.
libmesos_la_LIBTOOLFLAGS = --tag=CXX

# Add the convenience library.
libmesos_la_LIBADD =							\
  libmesos_no_3rdparty.la						\
  -lsvn_subr-1								\
  -lsvn_delta-1								\
  -lapr-1

libmesos_la_LIBADD += ../$(LIBPROCESS)/libprocess.la

# For non-convenience libraries we need to link them in to make the shared
# library each time. (Currently, we don't support platforms where this is not
# possible.)
if WITH_BUNDLED_GLOG
libmesos_la_LIBADD += ../$(GLOG)/libglog.la
else
libmesos_la_LIBADD += -lglog
LDADD += -lglog
endif

if WITH_BUNDLED_LEVELDB
# We need to directly include the leveldb library in order to avoid
# the installed libmesos.la file to include leveldb in
# 'dependency_libs' (via '-L../3rdparty/leveldb -lleveldb').

libmesos_la_LIBADD += ../$(LEVELDB)/libleveldb.a
else
libmesos_la_LIBADD += -lleveldb
LDADD += -lleveldb
endif

if WITH_BUNDLED_ZOOKEEPER
libmesos_la_LIBADD += ../$(ZOOKEEPER)/libzookeeper_mt.la
else
libmesos_la_LIBADD += -lzookeeper_mt
LDADD += -lzookeeper_mt
endif

if WITH_BUNDLED_PROTOBUF
libmesos_la_LIBADD += ../$(PROTOBUF)/src/libprotobuf.la
else
libmesos_la_LIBADD += -lprotobuf
LDADD += -lprotobuf
endif

if ENABLE_NVIDIA_GPU_SUPPORT
libmesos_la_LIBADD += -lnvidia-ml
LDADD += -lnvidia-ml
endif


# Binaries.
sbin_PROGRAMS += mesos-agent
mesos_agent_SOURCES = slave/main.cpp
mesos_agent_CPPFLAGS = $(MESOS_CPPFLAGS)
mesos_agent_LDADD = libmesos.la $(LDADD)

sbin_PROGRAMS += mesos-master
mesos_master_SOURCES = master/main.cpp
mesos_master_CPPFLAGS = $(MESOS_CPPFLAGS)
mesos_master_LDADD = libmesos.la $(LDADD)

# TODO(tomxing): Remove this binary once the
# slave->agent rename is complete(MESOS-3782).
sbin_PROGRAMS += mesos-slave
mesos_slave_SOURCES = slave/main.cpp
mesos_slave_CPPFLAGS = $(MESOS_CPPFLAGS)
mesos_slave_LDADD = libmesos.la $(LDADD)

bin_PROGRAMS += mesos-local
mesos_local_SOURCES = local/main.cpp
mesos_local_CPPFLAGS = $(MESOS_CPPFLAGS)
mesos_local_LDADD = libmesos.la $(LDADD)

pkglibexec_PROGRAMS += mesos-fetcher
mesos_fetcher_SOURCES = launcher/fetcher.cpp
mesos_fetcher_CPPFLAGS = $(MESOS_CPPFLAGS)
mesos_fetcher_LDADD = libmesos.la $(LDADD)

pkglibexec_PROGRAMS += mesos-executor
mesos_executor_SOURCES = launcher/executor.cpp
mesos_executor_CPPFLAGS = $(MESOS_CPPFLAGS)
mesos_executor_LDADD = libmesos.la $(LDADD)

pkglibexec_PROGRAMS += mesos-http-executor
mesos_http_executor_SOURCES = launcher/http_command_executor.cpp
mesos_http_executor_CPPFLAGS = $(MESOS_CPPFLAGS)
mesos_http_executor_LDADD = libmesos.la $(LDADD)

pkglibexec_PROGRAMS += mesos-containerizer
mesos_containerizer_SOURCES = slave/containerizer/mesos/main.cpp
mesos_containerizer_CPPFLAGS = $(MESOS_CPPFLAGS)
mesos_containerizer_LDADD = libmesos.la $(LDADD)

pkglibexec_PROGRAMS += mesos-logrotate-logger
mesos_logrotate_logger_SOURCES =		\
  slave/container_loggers/logrotate.hpp		\
  slave/container_loggers/logrotate.cpp
mesos_logrotate_logger_CPPFLAGS = $(MESOS_CPPFLAGS)
mesos_logrotate_logger_LDADD = libmesos.la $(LDADD)

if WITH_NETWORK_ISOLATOR
pkglibexec_PROGRAMS += mesos-network-helper
mesos_network_helper_SOURCES = slave/containerizer/mesos/isolators/network/helper.cpp
mesos_network_helper_CPPFLAGS = $(MESOS_CPPFLAGS)
mesos_network_helper_LDADD = libmesos.la $(LDADD)
endif

pkglibexec_PROGRAMS += mesos-health-check
mesos_health_check_SOURCES = health-check/main.cpp
mesos_health_check_CPPFLAGS = $(MESOS_CPPFLAGS)
mesos_health_check_LDADD = libmesos.la $(LDADD)

pkglibexec_PROGRAMS += mesos-usage
mesos_usage_SOURCES = usage/main.cpp
mesos_usage_CPPFLAGS = $(MESOS_CPPFLAGS)
mesos_usage_LDADD = libmesos.la $(LDADD)

pkglibexec_PROGRAMS += mesos-docker-executor
mesos_docker_executor_SOURCES = docker/executor.cpp
mesos_docker_executor_CPPFLAGS = $(MESOS_CPPFLAGS)
mesos_docker_executor_LDADD = libmesos.la $(LDADD)

bin_PROGRAMS += mesos-log
mesos_log_SOURCES = log/main.cpp
mesos_log_CPPFLAGS = $(MESOS_CPPFLAGS)
mesos_log_LDADD = libmesos.la $(LDADD)

bin_PROGRAMS += mesos
mesos_SOURCES = cli/mesos.cpp
mesos_CPPFLAGS = $(MESOS_CPPFLAGS)
mesos_LDADD = libmesos.la $(LDADD)

bin_PROGRAMS += mesos-execute
mesos_execute_SOURCES = cli/execute.cpp
mesos_execute_CPPFLAGS = $(MESOS_CPPFLAGS)
mesos_execute_LDADD = libmesos.la $(LDADD)

bin_PROGRAMS += mesos-resolve
mesos_resolve_SOURCES = cli/resolve.cpp
mesos_resolve_CPPFLAGS = $(MESOS_CPPFLAGS)
mesos_resolve_LDADD = libmesos.la $(LDADD)

# Need to distribute and install mesos-*.
dist_bin_SCRIPTS +=							\
  cli/mesos-cat								\
  cli/mesos-ps								\
  cli/mesos-scp								\
  cli/mesos-tail

# Need to distribute/install webui javascript. We use 'pkgdatadir'
# instead of 'datadir' as the install directory so we get the
# package name (i.e., 'mesos') as part of the path (i.e.,
# /path/to/mesos/webui versus something like /path/to/webui). Note
# that 'datadir' (e.g., /usr/local/share) is for read-only "data" and
# 'sysconfdir' (e.g., /usr/local/var) is for modifiable "data".
nobase_dist_pkgdata_DATA +=						\
  webui/master/static/js/app.js						\
  webui/master/static/js/controllers.js					\
  webui/master/static/js/jquery.pailer.js				\
  webui/master/static/js/services.js

# Need to distribute/install webui CSS.
nobase_dist_pkgdata_DATA +=						\
  webui/master/static/css/bootstrap-3.3.6.min.css			\
  webui/master/static/css/mesos.css

# Need to distribute/install webui HTML.
nobase_dist_pkgdata_DATA +=						\
  webui/master/static/browse.html					\
  webui/master/static/framework.html					\
  webui/master/static/frameworks.html					\
  webui/master/static/home.html						\
  webui/master/static/index.html					\
  webui/master/static/offers.html					\
  webui/master/static/pailer.html					\
  webui/master/static/slave.html					\
  webui/master/static/slave_executor.html				\
  webui/master/static/slave_framework.html				\
  webui/master/static/slaves.html					\
  webui/master/static/directives/pagination.html			\
  webui/master/static/directives/tableHeader.html			\
  webui/master/static/directives/timestamp.html

# Need to distribute/install webui images.
nobase_dist_pkgdata_DATA +=						\
  webui/master/static/ico/favicon.ico					\
  webui/master/static/img/loading.gif

# Need to distribute/install webui fonts.
nobase_dist_pkgdata_DATA +=						\
  webui/master/static/fonts/glyphicons-halflings-regular.eot		\
  webui/master/static/fonts/glyphicons-halflings-regular.svg		\
  webui/master/static/fonts/glyphicons-halflings-regular.ttf		\
  webui/master/static/fonts/glyphicons-halflings-regular.woff

# Need to distribute/install third-party javascript.
nobase_dist_pkgdata_DATA +=						\
  webui/master/static/js/angular-1.2.3.js				\
  webui/master/static/js/angular-1.2.3.min.js				\
  webui/master/static/js/angular-route-1.2.3.js				\
  webui/master/static/js/angular-route-1.2.3.min.js			\
  webui/master/static/js/jquery-1.7.1.js				\
  webui/master/static/js/jquery-1.7.1.min.js				\
  webui/master/static/js/relative-date.js				\
  webui/master/static/js/ui-bootstrap-tpls-0.9.0.js			\
  webui/master/static/js/ui-bootstrap-tpls-0.9.0.min.js			\
  webui/master/static/js/underscore-1.4.3.js				\
  webui/master/static/js/underscore-1.4.3.min.js			\
  webui/master/static/js/zeroclipboard-1.1.7.js				\
  webui/master/static/js/zeroclipboard-1.1.7.min.js			\
  webui/master/static/obj/zeroclipboard-1.1.7.swf

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
  $(srcdir)/java/src/org/apache/mesos/state/ZooKeeperState.java
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
  $(srcdir)/examples/java/TestMultipleExecutorsFramework.java
EXTRA_DIST += $(EXAMPLES_SOURCE)


if HAS_JAVA

$(MESOS_JAR): $(MESOS_JAR_SOURCE) $(MESOS_JAR_GENERATED) java/mesos.pom
	@echo "Building mesos-$(PACKAGE_VERSION).jar ..."
	@cd $(abs_top_builddir)/src/java && $(MVN) -f mesos.pom clean package

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
  $(CONTAINERIZER_PROTO)						\
  $(MESOS_PROTO)							\
  $(V1_EXECUTOR_PROTO)							\
  $(V1_MESOS_PROTO)							\
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
  java/jni/org_apache_mesos_state_ZooKeeperState.h

BUILT_SOURCES += $(nodist_libjava_la_SOURCES)

java/jni/org_apache_mesos_MesosSchedulerDriver.h: $(MESOS_JAR)
	$(JAVA_HOME)/bin/javah -d java/jni				\
	-classpath $(MESOS_JAR):@PROTOBUF_JAR@				\
	  org.apache.mesos.MesosSchedulerDriver

java/jni/org_apache_mesos_MesosExecutorDriver.h: $(MESOS_JAR)
	$(JAVA_HOME)/bin/javah -d java/jni				\
	-classpath $(MESOS_JAR):@PROTOBUF_JAR@				\
          org.apache.mesos.MesosExecutorDriver

java/jni/org_apache_mesos_Log.h: $(MESOS_JAR)
	$(JAVA_HOME)/bin/javah -d java/jni				\
	-classpath $(MESOS_JAR):@PROTOBUF_JAR@				\
	  org.apache.mesos.Log

java/jni/org_apache_mesos_state_AbstractState.h: $(MESOS_JAR)
	$(JAVA_HOME)/bin/javah -d java/jni				\
	-classpath $(MESOS_JAR):@PROTOBUF_JAR@				\
	  org.apache.mesos.state.AbstractState

java/jni/org_apache_mesos_state_LevelDBState.h: $(MESOS_JAR)
	$(JAVA_HOME)/bin/javah -d java/jni				\
	-classpath $(MESOS_JAR):@PROTOBUF_JAR@				\
	  org.apache.mesos.state.LevelDBState

java/jni/org_apache_mesos_state_LogState.h: $(MESOS_JAR)
	$(JAVA_HOME)/bin/javah -d java/jni				\
	-classpath $(MESOS_JAR):@PROTOBUF_JAR@				\
	  org.apache.mesos.state.LogState

java/jni/org_apache_mesos_state_Variable.h: $(MESOS_JAR)
	$(JAVA_HOME)/bin/javah -d java/jni				\
	-classpath $(MESOS_JAR):@PROTOBUF_JAR@				\
	  org.apache.mesos.state.Variable

java/jni/org_apache_mesos_state_ZooKeeperState.h: $(MESOS_JAR)
	$(JAVA_HOME)/bin/javah -d java/jni				\
	-classpath $(MESOS_JAR):@PROTOBUF_JAR@				\
	  org.apache.mesos.state.ZooKeeperState

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
	$(MVN) -f java/mesos.pom install

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
	  PYTHONPATH=$(abs_top_builddir)/$(DISTRIBUTE)	\
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
	PYTHONPATH=$(abs_top_builddir)/$(DISTRIBUTE):$(abs_top_builddir)/$(WHEEL)	\
	$(PYTHON) setup.py bdist_egg --dist-dir=$(abs_top_builddir)/src/python/dist	\
	bdist_wheel --dist-dir=$(abs_top_builddir)/src/python/dist

CLEANFILES += $(MESOS_EGGS) $(MESOS_WHLS) python/*/build python/*/dist $(PYTHON_SOURCE)

pippythonpath = $(DESTDIR)$(pythondir):$(abs_top_builddir)/$(DISTRIBUTE):$(abs_top_builddir)/$(PIP):$(abs_top_builddir)/$(WHEEL)

# Some python installations create their own distutils.cfg (homebrew, redhat).
# This includes `prefix=` which overrides anything set via. pip during install.
# The file is required to ensure the python packages get installed to the right
# location. It only affects files installed from the python/dist directory.
python/dist/setup.cfg:
	printf "[install]\nprefix=\n" > $@

# See the comment next to AM_DISTCHECK_CONFIGURE_FLAGS in the top level
# Makefile.am for an explanation of this variable.
pipinstallargs=

if WITHOUT_PYTHON_DEPS
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
	find python -name "build" -o -name "dist" -o -name "*.pyc"	\
	  -o -name "*.egg-info" | xargs rm -rf

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

check_PROGRAMS += load-generator-framework
load_generator_framework_SOURCES = examples/load_generator_framework.cpp
load_generator_framework_CPPFLAGS = $(MESOS_CPPFLAGS)
load_generator_framework_LDADD = libmesos.la $(LDADD)

check_PROGRAMS += persistent-volume-framework
persistent_volume_framework_SOURCES = examples/persistent_volume_framework.cpp
persistent_volume_framework_CPPFLAGS = $(MESOS_CPPFLAGS)
persistent_volume_framework_LDADD = libmesos.la $(LDADD)

if OS_LINUX
check_PROGRAMS += setns-test-helper
setns_test_helper_SOURCES =					\
  tests/containerizer/setns_test_helper_main.cpp		\
  tests/containerizer/setns_test_helper.cpp
setns_test_helper_CPPFLAGS = $(MESOS_CPPFLAGS)
setns_test_helper_LDADD = libmesos.la $(LDADD)
endif

check_PROGRAMS += memory-test-helper
memory_test_helper_SOURCES =					\
  tests/flags.cpp						\
  tests/utils.cpp						\
  tests/containerizer/memory_test_helper_main.cpp		\
  tests/containerizer/memory_test_helper.cpp
memory_test_helper_CPPFLAGS = $(mesos_tests_CPPFLAGS)
memory_test_helper_LDADD = $(mesos_tests_LDADD)

check_PROGRAMS += active-user-test-helper
active_user_test_helper_SOURCES = tests/active_user_test_helper.cpp
active_user_test_helper_CPPFLAGS = $(MESOS_CPPFLAGS)
active_user_test_helper_LDADD = libmesos.la $(LDADD)

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
  tests/anonymous_tests.cpp					\
  tests/attributes_tests.cpp					\
  tests/authentication_tests.cpp				\
  tests/authorization_tests.cpp					\
  tests/cluster.cpp						\
  tests/command_executor_tests.cpp				\
  tests/container_logger_tests.cpp				\
  tests/containerizer.cpp					\
  tests/cram_md5_authentication_tests.cpp			\
  tests/credentials_tests.cpp					\
  tests/disk_quota_tests.cpp					\
  tests/dynamic_weights_tests.cpp				\
  tests/environment.cpp						\
  tests/examples_tests.cpp					\
  tests/exception_tests.cpp					\
  tests/executor_http_api_tests.cpp				\
  tests/fault_tolerance_tests.cpp				\
  tests/fetcher_cache_tests.cpp					\
  tests/fetcher_tests.cpp					\
  tests/files_tests.cpp						\
  tests/flags.cpp						\
  tests/gc_tests.cpp						\
  tests/hdfs_tests.cpp						\
  tests/health_check_tests.cpp					\
  tests/hierarchical_allocator_tests.cpp			\
  tests/hook_tests.cpp						\
  tests/http_authentication_tests.cpp				\
  tests/http_fault_tolerance_tests.cpp				\
  tests/log_tests.cpp						\
  tests/logging_tests.cpp					\
  tests/main.cpp						\
  tests/master_allocator_tests.cpp				\
  tests/master_authorization_tests.cpp				\
  tests/master_contender_detector_tests.cpp			\
  tests/master_maintenance_tests.cpp				\
  tests/master_quota_tests.cpp					\
  tests/master_slave_reconciliation_tests.cpp			\
  tests/master_tests.cpp					\
  tests/master_validation_tests.cpp				\
  tests/mesos.cpp						\
  tests/metrics_tests.cpp					\
  tests/module.cpp						\
  tests/module_tests.cpp					\
  tests/oversubscription_tests.cpp				\
  tests/partition_tests.cpp					\
  tests/paths_tests.cpp						\
  tests/persistent_volume_endpoints_tests.cpp			\
  tests/persistent_volume_tests.cpp				\
  tests/protobuf_io_tests.cpp					\
  tests/rate_limiting_tests.cpp					\
  tests/reconciliation_tests.cpp				\
  tests/registrar_tests.cpp					\
  tests/repair_tests.cpp					\
  tests/reservation_endpoints_tests.cpp				\
  tests/reservation_tests.cpp					\
  tests/resource_offers_tests.cpp				\
  tests/resources_tests.cpp					\
  tests/role_tests.cpp						\
  tests/scheduler_driver_tests.cpp				\
  tests/scheduler_event_call_tests.cpp				\
  tests/scheduler_http_api_tests.cpp				\
  tests/scheduler_tests.cpp					\
  tests/script.cpp						\
  tests/slave_recovery_tests.cpp				\
  tests/slave_tests.cpp						\
  tests/sorter_tests.cpp					\
  tests/state_tests.cpp						\
  tests/status_update_manager_tests.cpp				\
  tests/teardown_tests.cpp					\
  tests/uri_tests.cpp						\
  tests/uri_fetcher_tests.cpp					\
  tests/utils.cpp						\
  tests/values_tests.cpp					\
  tests/zookeeper_url_tests.cpp					\
  tests/common/command_utils_tests.cpp				\
  tests/common/http_tests.cpp					\
  tests/common/recordio_tests.cpp				\
  tests/containerizer/composing_containerizer_tests.cpp		\
  tests/containerizer/docker_containerizer_tests.cpp		\
  tests/containerizer/docker_spec_tests.cpp			\
  tests/containerizer/docker_tests.cpp				\
  tests/containerizer/external_containerizer_test.cpp		\
  tests/containerizer/isolator_tests.cpp			\
  tests/containerizer/launcher.cpp				\
  tests/containerizer/memory_test_helper.cpp			\
  tests/containerizer/mesos_containerizer_tests.cpp		\
  tests/containerizer/provisioner_appc_tests.cpp		\
  tests/containerizer/provisioner_backend_tests.cpp		\
  tests/containerizer/provisioner_docker_tests.cpp

if ENABLE_XFS_DISK_ISOLATOR
mesos_tests_SOURCES +=						\
  tests/containerizer/xfs_quota_tests.cpp
endif

mesos_tests_CPPFLAGS = $(MESOS_CPPFLAGS)
mesos_tests_CPPFLAGS += -DSOURCE_DIR=\"$(abs_top_srcdir)\"
mesos_tests_CPPFLAGS += -DBUILD_DIR=\"$(abs_top_builddir)\"
mesos_tests_CPPFLAGS += -I../$(GTEST)/include
mesos_tests_CPPFLAGS += -I../$(GMOCK)/include
mesos_tests_CPPFLAGS += -DTESTLIBEXECDIR=\"$(testlibexecdir)\"
mesos_tests_CPPFLAGS += -DSBINDIR=\"$(sbindir)\"

mesos_tests_LDADD = ../$(LIBPROCESS)/3rdparty/libgmock.la libmesos.la $(LDADD)

if !OS_FREEBSD
  mesos_tests_LDADD += -ldl # FreeBSD includes dynamic lib utils in libc.
endif

mesos_tests_DEPENDENCIES = 					\
  ../$(LIBPROCESS)/3rdparty/libgmock.la 			\
  libmesos.la 							\
  $(MESOS_TEST_MODULES)

if OS_LINUX
mesos_tests_SOURCES +=						\
  tests/containerizer/cgroups_isolator_tests.cpp		\
  tests/containerizer/cgroups_tests.cpp				\
  tests/containerizer/runtime_isolator_tests.cpp		\
  tests/containerizer/filesystem_isolator_tests.cpp		\
  tests/containerizer/fs_tests.cpp				\
  tests/containerizer/launch_tests.cpp				\
  tests/containerizer/memory_pressure_tests.cpp			\
  tests/containerizer/ns_tests.cpp				\
  tests/containerizer/perf_tests.cpp				\
  tests/containerizer/sched_tests.cpp				\
  tests/containerizer/setns_test_helper.cpp
endif

if WITH_NETWORK_ISOLATOR
mesos_tests_SOURCES +=						\
  tests/containerizer/port_mapping_tests.cpp			\
  tests/containerizer/routing_tests.cpp
endif

if ENABLE_NVIDIA_GPU_SUPPORT
mesos_tests_SOURCES +=						\
  tests/containerizer/nvidia_gpu_isolator_tests.cpp
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
mesos_tests_LDFLAGS = $(AM_LDFLAGS)
mesos_tests_DEPENDENCIES += $(EXAMPLES_JAR)

EXAMPLESCRIPTSJAVA =						\
  examples/java/test-executor					\
  examples/java/test-exception-framework			\
  examples/java/test-framework					\
  examples/java/test-log					\
  examples/java/test-multiple-executors-framework

check_SCRIPTS += $(EXAMPLESCRIPTSJAVA)
mesos_tests_DEPENDENCIES += $(EXAMPLESCRIPTSJAVA)
endif

if HAS_PYTHON
mesos_tests_DEPENDENCIES += $(MESOS_EGG)

EXAMPLESCRIPTSPYTHON =						\
  examples/python/test_containerizer.py				\
  examples/python/test-containerizer				\
  examples/python/test_executor.py				\
  examples/python/test-executor					\
  examples/python/test_framework.py				\
  examples/python/test-framework

check_SCRIPTS += $(EXAMPLESCRIPTSPYTHON)
mesos_tests_DEPENDENCIES += $(EXAMPLESCRIPTSPYTHON)
endif

EXTRA_DIST +=							\
  examples/python/test_containerizer.py				\
  examples/python/test_executor.py				\
  examples/python/test_framework.py


dist_check_SCRIPTS +=						\
  tests/balloon_framework_test.sh				\
  tests/dynamic_reservation_framework_test.sh			\
  tests/test_http_framework_test.sh				\
  tests/java_exception_test.sh					\
  tests/java_framework_test.sh					\
  tests/java_log_test.sh					\
  tests/no_executor_framework_test.sh				\
  tests/persistent_volume_framework_test.sh			\
  tests/python_framework_test.sh				\
  tests/test_framework_test.sh

../$(LIBPROCESS)/3rdparty/libgmock.la:
	@cd ../$(LIBPROCESS)/3rdparty && $(MAKE) $(AM_MAKEFLAGS) libgmock.la

if INSTALL_TESTS
testlibexec_PROGRAMS = $(check_PROGRAMS)
dist_testlibexec_SCRIPTS =			\
  tests/balloon_framework_test.sh		\
  tests/test_http_framework_test.sh		\
  tests/java_exception_test.sh			\
  tests/java_framework_test.sh			\
  tests/java_log_test.sh			\
  tests/no_executor_framework_test.sh		\
  tests/persistent_volume_framework_test.sh	\
  tests/python_framework_test.sh		\
  tests/test_framework_test.sh

endif

# Copy mesos-agent-env.sh.template to mesos-slave-env.sh.template.
#
# TODO(tomxing): Remove this target once the
# slave->agent rename is complete(MESOS-3782).
copy-agent-env-template:
	cp $(DESTDIR)/$(pkgsysconfdir)/mesos-agent-env.sh.template \
	  $(DESTDIR)/$(pkgsysconfdir)/mesos-slave-env.sh.template

# Install compatibility symlinks for modules that used to be in $(LIBDIR)
# but are now in $(PKGMODULEDIR). We use install-data-hook because it
# runs late in the install process after the target directories have
# been created.
install-data-hook: copy-agent-env-template
	cd $(DESTDIR)/$(libdir) && 				\
	for name in libfixed_resource_estimator 		\
	    libload_qos_controller 				\
	    liblogrotate_container_logger; do 			\
		for lib in `cd $(DESTDIR)/$(pkgmoduledir) && ls $${name}*`; do \
		  rm -f $$lib; 					\
		  $(LN_S) $(relative_pkgmoduledir)/$$lib $$lib; \
		done;						\
	done

# TODO(tomxing): Remove this hook once the
# slave->agent rename is complete(MESOS-3782).
uninstall-hook:
	rm -f $(DESTDIR)/$(pkgsysconfdir)/mesos-slave-env.sh.template

# We use a check-local target for now to avoid the parallel test
# runner that ships with newer versions of autotools.
# See the following discussion for the workaround:
# http://lists.gnu.org/archive/html/automake/2013-01/msg00051.html
check-local: tests
	./mesos-tests

if INSTALL_TESTS
# If we enabled test installation, we can run the tests from the
# installation tree.
installcheck-local:
	$(testlibexecdir)/mesos-tests --build_dir=/nowhere --source_dir=/nowhere
endif

clean-local: clean-java clean-python

tests: all $(check_PROGRAMS) mesos-tests

.PHONY: $(PHONY_TARGETS)
