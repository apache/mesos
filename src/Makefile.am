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
LEVELDB = 3rdparty/leveldb
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

if WITH_BUNDLED_PROTOBUF
  # Protocol buffer compiler.
  PROTOC = ../$(PROTOBUF)/src/protoc
else
  PROTOC = @PROTOCOMPILER@
endif

PROTOCFLAGS = -I$(top_srcdir)/include -I$(srcdir)

# Initialize variables here so we can use += operator everywhere else.
lib_LTLIBRARIES =
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
MESOS_CPPFLAGS += -I$(top_srcdir)/include
MESOS_CPPFLAGS += -I$(top_srcdir)/$(LIBPROCESS)/include
MESOS_CPPFLAGS += -I$(top_srcdir)/$(STOUT)/include
MESOS_CPPFLAGS += -I../include

# Protobuf headers that depend on mesos.pb.h need this.
MESOS_CPPFLAGS += -I../include/mesos

# Header only dependencies will be ignored for --disable-bundled.
MESOS_CPPFLAGS += -I../$(BOOST)
MESOS_CPPFLAGS += -I../$(PICOJSON)

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

if WITH_NETWORK_ISOLATOR
  MESOS_CPPFLAGS += $(LIBNL_CFLAGS)
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

MESOS_PROTO = $(top_srcdir)/include/mesos/mesos.proto

ALLOCATOR_PROTO =							\
  $(top_srcdir)/include/mesos/master/allocator.proto

AUTHENTICATION_PROTO =							\
  $(top_srcdir)/include/mesos/authentication/authentication.proto

CONTAINERIZER_PROTO =							\
  $(top_srcdir)/include/mesos/containerizer/containerizer.proto

FETCHER_PROTO =								\
  $(top_srcdir)/include/mesos/fetcher/fetcher.proto

MODULE_PROTO =								\
  $(top_srcdir)/include/mesos/module/module.proto

SCHEDULER_PROTO =							\
  $(top_srcdir)/include/mesos/scheduler/scheduler.proto

CXX_PROTOS =								\
  mesos.pb.cc								\
  ../include/mesos/mesos.pb.h						\
  authentication/authentication.pb.cc					\
  ../include/mesos/authentication/authentication.pb.h			\
  containerizer/containerizer.pb.cc					\
  ../include/mesos/containerizer/containerizer.pb.h			\
  fetcher/fetcher.pb.cc							\
  ../include/mesos/fetcher/fetcher.pb.h					\
  master/allocator.pb.cc						\
  ../include/mesos/master/allocator.pb.h				\
  module/module.pb.cc							\
  ../include/mesos/module/module.pb.h					\
  scheduler/scheduler.pb.cc						\
  ../include/mesos/scheduler/scheduler.pb.h

JAVA_PROTOS =								\
  java/generated/org/apache/mesos/Protos.java				\
  java/generated/org/apache/mesos/containerizer/Protos.java

PYTHON_PROTOS =								\
  python/interface/src/mesos/interface/mesos_pb2.py			\
  python/interface/src/mesos/interface/containerizer_pb2.py

BUILT_SOURCES += $(CXX_PROTOS) $(JAVA_PROTOS) $(PYTHON_PROTOS)
CLEANFILES += $(CXX_PROTOS) $(JAVA_PROTOS) $(PYTHON_PROTOS)

MESSAGES_PROTOS = messages/messages.pb.cc messages/messages.pb.h

BUILT_SOURCES += $(MESSAGES_PROTOS)
CLEANFILES += $(MESSAGES_PROTOS)

LOG_PROTOS = messages/log.pb.cc messages/log.pb.h

BUILT_SOURCES += $(LOG_PROTOS)
CLEANFILES += $(LOG_PROTOS)

STATE_PROTOS = messages/state.pb.cc messages/state.pb.h

BUILT_SOURCES += $(STATE_PROTOS)
CLEANFILES += $(STATE_PROTOS)

REGISTRY_PROTOS = master/registry.pb.cc master/registry.pb.h

BUILT_SOURCES += $(REGISTRY_PROTOS)
CLEANFILES += $(REGISTRY_PROTOS)

# Targets for generating protocol buffer code.
# For the include headers, place the header files in the include
# directory and leave the cc files in src.
%.pb.cc ../include/mesos/%.pb.h: $(top_srcdir)/include/mesos/%.proto
	$(MKDIR_P) $(@D)
	$(PROTOC) $(PROTOCFLAGS) --cpp_out=../include $^
	mv ../include/mesos/*.pb.cc .

authentication/%.pb.cc ../include/mesos/authentication/%.pb.h: $(AUTHENTICATION_PROTO)
	$(MKDIR_P) $(@D)
	$(MKDIR_P) ../include/mesos/authentication
	$(PROTOC) $(PROTOCFLAGS) --cpp_out=../include $^
	mv ../include/mesos/authentication/*.pb.cc $(@D)

containerizer/%.pb.cc ../include/mesos/containerizer/%.pb.h: $(CONTAINERIZER_PROTO)
	$(MKDIR_P) $(@D)
	$(MKDIR_P) ../include/mesos/containerizer
	$(PROTOC) $(PROTOCFLAGS) --cpp_out=../include $^
	mv ../include/mesos/containerizer/*.pb.cc $(@D)

fetcher/%.pb.cc ../include/mesos/fetcher/%.pb.h: $(FETCHER_PROTO)
	$(MKDIR_P) $(@D)
	$(MKDIR_P) ../include/mesos/fetcher
	$(PROTOC) $(PROTOCFLAGS) --cpp_out=../include $^
	mv ../include/mesos/fetcher/*.pb.cc $(@D)

master/%.pb.cc ../include/mesos/master/%.pb.h: $(ALLOCATOR_PROTO)
	$(MKDIR_P) $(@D)
	$(MKDIR_P) ../include/mesos/master
	$(PROTOC) $(PROTOCFLAGS) --cpp_out=../include $^
	mv ../include/mesos/master/*.pb.cc $(@D)

module/%.pb.cc ../include/mesos/module/%.pb.h: $(MODULE_PROTO)
	$(MKDIR_P) $(@D)
	$(MKDIR_P) ../include/mesos/module
	$(PROTOC) $(PROTOCFLAGS) --cpp_out=../include $^
	mv ../include/mesos/module/*.pb.cc $(@D)

scheduler/%.pb.cc ../include/mesos/scheduler/%.pb.h: $(SCHEDULER_PROTO)
	$(MKDIR_P) $(@D)
	$(MKDIR_P) ../include/mesos/scheduler
	$(PROTOC) $(PROTOCFLAGS) --cpp_out=../include $^
	mv ../include/mesos/scheduler/*.pb.cc $(@D)

%.pb.cc %.pb.h: %.proto
	$(MKDIR_P) $(@D)
	$(PROTOC) $(PROTOCFLAGS) --cpp_out=. $^

java/generated/org/apache/mesos/Protos.java: $(MESOS_PROTO)
	$(MKDIR_P)  $(@D)
	$(PROTOC) $(PROTOCFLAGS) --java_out=java/generated $^

java/generated/org/apache/mesos/containerizer/Protos.java:		\
		$(CONTAINERIZER_PROTO)
	$(MKDIR_P)  $(@D)
	$(PROTOC) $(PROTOCFLAGS) --java_out=java/generated $^

java/generated/org/apache/mesos/fetcher/Protos.java: $(FETCHER_PROTO)
	$(MKDIR_P)  $(@D)
	$(PROTOC) $(PROTOCFLAGS) --java_out=java/generated $^

java/generated/org/apache/mesos/scheduler/Protos.java: $(SCHEDULER_PROTO)
	$(MKDIR_P)  $(@D)
	$(PROTOC) $(PROTOCFLAGS) --java_out=java/generated $^

python/interface/src/mesos/interface/mesos_pb2.py: $(MESOS_PROTO)
	$(MKDIR_P) $(@D)
	$(PROTOC) -I$(top_srcdir)/include/mesos				\
		$(PROTOCFLAGS) --python_out=python/interface/src/mesos/interface $^

# All python protocol buffer code that imports from mesos.proto has
# to get patched using sed, removing the leading 'mesos.' namespace
# qualifier due to the flat hierachy in the resulting mesos.interface
# egg. See MESOS-1750 for more.
python/interface/src/mesos/interface/containerizer_pb2.py:		\
		$(CONTAINERIZER_PROTO)
	$(MKDIR_P) $(@D)
	$(PROTOC) -I$(top_srcdir)/include/mesos/containerizer		\
		$(PROTOCFLAGS)						\
		--python_out=python/interface/src/mesos/interface $^
	sed -e 's/mesos\.mesos_pb2/mesos_pb2/' <$@ >$@

python/interface/src/mesos/interface/scheduler_pb2.py: $(SCHEDULER_PROTO)
	$(MKDIR_P) $(@D)
	$(PROTOC) -I$(top_srcdir)/include/mesos/scheduler		\
		$(PROTOCFLAGS)						\
		--python_out=python/interface/src/mesos/interface $^
	sed -e 's/mesos\.mesos_pb2/mesos_pb2/' <$@ >$@

# We even use a convenience library for most of Mesos so that we can
# exclude third party libraries so setuptools/distribute can build a
# self-contained Python library and statically link in the third party
# libraries themselves.
noinst_LTLIBRARIES += libmesos_no_3rdparty.la

nodist_libmesos_no_3rdparty_la_SOURCES =				\
  $(CXX_PROTOS)								\
  $(MESSAGES_PROTOS)							\
  $(REGISTRY_PROTOS)

# TODO(tillt): Remove authentication/cram_md5/* which will enable us to
# lose the immediate cyrus-sasl2 dependency.
libmesos_no_3rdparty_la_SOURCES =					\
	authentication/cram_md5/authenticator.cpp			\
	authentication/cram_md5/auxprop.cpp				\
	authorizer/authorizer.cpp					\
	common/attributes.cpp						\
	common/date_utils.cpp						\
	common/http.cpp							\
	common/lock.cpp							\
	common/protobuf_utils.cpp					\
	common/resources.cpp						\
	common/resources_utils.cpp					\
	common/thread.cpp						\
	common/type_utils.cpp						\
	common/values.cpp						\
	docker/docker.hpp						\
	docker/docker.cpp						\
	exec/exec.cpp							\
	files/files.cpp							\
	hook/manager.cpp						\
	local/local.cpp							\
	logging/flags.cpp						\
	logging/logging.cpp						\
	master/contender.cpp						\
	master/constants.cpp						\
	master/detector.cpp						\
	master/flags.cpp						\
	master/http.cpp							\
	master/master.cpp						\
	master/metrics.cpp						\
	master/registry.hpp						\
	master/registry.proto						\
	master/registrar.cpp						\
	master/repairer.cpp						\
	master/validation.cpp						\
	master/allocator/sorter/drf/sorter.cpp				\
	module/manager.cpp						\
	sched/constants.cpp						\
	sched/sched.cpp							\
	scheduler/scheduler.cpp						\
	slave/constants.cpp						\
	slave/gc.cpp							\
	slave/flags.cpp							\
	slave/http.cpp							\
	slave/metrics.cpp						\
	slave/monitor.cpp						\
	slave/paths.cpp							\
	slave/state.cpp							\
	slave/slave.cpp							\
	slave/containerizer/containerizer.cpp				\
	slave/containerizer/composing.cpp				\
	slave/containerizer/composing.hpp				\
	slave/containerizer/docker.cpp					\
	slave/containerizer/docker.hpp					\
	slave/containerizer/external_containerizer.cpp			\
	slave/containerizer/fetcher.cpp					\
	slave/containerizer/isolator.cpp				\
	slave/containerizer/isolators/posix/disk.cpp			\
	slave/containerizer/launcher.cpp				\
	slave/containerizer/mesos/containerizer.cpp			\
	slave/containerizer/mesos/launch.cpp				\
	slave/status_update_manager.cpp					\
	usage/usage.cpp							\
	watcher/whitelist_watcher.cpp					\
	zookeeper/contender.cpp						\
	zookeeper/detector.cpp						\
	zookeeper/zookeeper.cpp						\
	zookeeper/authentication.cpp					\
	zookeeper/group.cpp						\
	messages/messages.proto

pkginclude_HEADERS =							\
  $(top_srcdir)/include/mesos/executor.hpp				\
  $(top_srcdir)/include/mesos/hook.hpp					\
  $(top_srcdir)/include/mesos/mesos.hpp					\
  $(top_srcdir)/include/mesos/module.hpp				\
  $(top_srcdir)/include/mesos/resources.hpp				\
  $(top_srcdir)/include/mesos/scheduler.hpp				\
  $(top_srcdir)/include/mesos/type_utils.hpp				\
  $(top_srcdir)/include/mesos/values.hpp				\
  $(top_srcdir)/include/mesos/mesos.proto

nodist_pkginclude_HEADERS =						\
  ../include/mesos/version.hpp						\
  ../include/mesos/mesos.pb.h

authenticationdir = $(pkgincludedir)/authentication

authentication_HEADERS =						\
  $(top_srcdir)/include/mesos/authentication/authenticatee.hpp		\
  $(top_srcdir)/include/mesos/authentication/authentication.hpp		\
  $(top_srcdir)/include/mesos/authentication/authentication.proto	\
  $(top_srcdir)/include/mesos/authentication/authenticator.hpp

nodist_authentication_HEADERS = ../include/mesos/authentication/authentication.pb.h

containerizerdir = $(pkgincludedir)/containerizer

containerizer_HEADERS =							\
  $(top_srcdir)/include/mesos/containerizer/containerizer.hpp		\
  $(top_srcdir)/include/mesos/containerizer/containerizer.proto

nodist_containerizer_HEADERS = ../include/mesos/containerizer/containerizer.pb.h

fetcherdir = $(pkgincludedir)/fetcher

fetcher_HEADERS =							\
  $(top_srcdir)/include/mesos/fetcher/fetcher.hpp			\
  $(top_srcdir)/include/mesos/fetcher/fetcher.proto

nodist_fetcher_HEADERS = ../include/mesos/fetcher/fetcher.pb.h

masterdir = $(pkgincludedir)/master

master_HEADERS =							\
  $(top_srcdir)/include/mesos/master/allocator.hpp			\
  $(top_srcdir)/include/mesos/master/allocator.proto

nodist_master_HEADERS = ../include/mesos/master/allocator.pb.h

moduledir = $(pkgincludedir)/module

module_HEADERS =							\
  $(top_srcdir)/include/mesos/module/allocator.hpp			\
  $(top_srcdir)/include/mesos/module/anonymous.hpp			\
  $(top_srcdir)/include/mesos/module/authenticatee.hpp			\
  $(top_srcdir)/include/mesos/module/authenticator.hpp			\
  $(top_srcdir)/include/mesos/module/hook.hpp				\
  $(top_srcdir)/include/mesos/module/isolator.hpp			\
  $(top_srcdir)/include/mesos/module/module.hpp				\
  $(top_srcdir)/include/mesos/module/module.proto

nodist_module_HEADERS = ../include/mesos/module/module.pb.h

schedulerdir = $(pkgincludedir)/scheduler

scheduler_HEADERS =							\
  $(top_srcdir)/include/mesos/scheduler/scheduler.hpp			\
  $(top_srcdir)/include/mesos/scheduler/scheduler.proto

nodist_scheduler_HEADERS = ../include/mesos/scheduler/scheduler.pb.h

slavedir = $(pkgincludedir)/slave

slave_HEADERS =								\
  $(top_srcdir)/include/mesos/slave/isolator.hpp

if OS_LINUX
  libmesos_no_3rdparty_la_SOURCES += linux/cgroups.cpp
  libmesos_no_3rdparty_la_SOURCES += linux/fs.cpp
  libmesos_no_3rdparty_la_SOURCES += linux/perf.cpp
  libmesos_no_3rdparty_la_SOURCES += slave/containerizer/isolators/cgroups/cpushare.cpp
  libmesos_no_3rdparty_la_SOURCES += slave/containerizer/isolators/cgroups/mem.cpp
  libmesos_no_3rdparty_la_SOURCES += slave/containerizer/isolators/cgroups/perf_event.cpp
  libmesos_no_3rdparty_la_SOURCES += slave/containerizer/isolators/namespaces/pid.cpp
  libmesos_no_3rdparty_la_SOURCES += slave/containerizer/isolators/filesystem/shared.cpp
  libmesos_no_3rdparty_la_SOURCES += slave/containerizer/linux_launcher.cpp
else
  EXTRA_DIST += linux/cgroups.cpp
  EXTRA_DIST += linux/fs.cpp
endif

if WITH_NETWORK_ISOLATOR
  libmesos_no_3rdparty_la_SOURCES +=					\
	linux/routing/route.cpp						\
	linux/routing/utils.cpp						\
	linux/routing/diagnosis/diagnosis.cpp				\
	linux/routing/filter/basic.cpp					\
	linux/routing/filter/icmp.cpp					\
	linux/routing/filter/ip.cpp					\
	linux/routing/link/link.cpp					\
	linux/routing/queueing/fq_codel.cpp				\
	linux/routing/queueing/handle.cpp				\
	linux/routing/queueing/ingress.cpp

  libmesos_no_3rdparty_la_SOURCES +=					\
	linux/routing/internal.hpp					\
	linux/routing/route.hpp						\
	linux/routing/utils.hpp						\
	linux/routing/diagnosis/diagnosis.hpp				\
	linux/routing/filter/action.hpp					\
	linux/routing/filter/basic.hpp					\
	linux/routing/filter/filter.hpp					\
	linux/routing/filter/handle.hpp					\
	linux/routing/filter/icmp.hpp					\
	linux/routing/filter/internal.hpp				\
	linux/routing/filter/ip.hpp					\
	linux/routing/filter/priority.hpp				\
	linux/routing/link/internal.hpp					\
	linux/routing/link/link.hpp					\
	linux/routing/queueing/fq_codel.hpp				\
	linux/routing/queueing/handle.hpp				\
	linux/routing/queueing/ingress.hpp				\
	linux/routing/queueing/internal.hpp

  libmesos_no_3rdparty_la_SOURCES +=					\
	slave/containerizer/isolators/network/port_mapping.cpp		\
	slave/containerizer/isolators/network/port_mapping.hpp
endif

libmesos_no_3rdparty_la_SOURCES +=					\
	authentication/cram_md5/authenticatee.hpp			\
	authentication/cram_md5/authenticator.hpp			\
	authentication/cram_md5/auxprop.hpp				\
	authorizer/authorizer.hpp					\
	common/attributes.hpp						\
	common/build.hpp						\
	common/date_utils.hpp						\
	common/factory.hpp						\
	common/http.hpp							\
	common/lock.hpp							\
	common/parse.hpp						\
	common/protobuf_utils.hpp					\
	common/resources_utils.hpp					\
	common/status_utils.hpp						\
	common/thread.hpp						\
	credentials/credentials.hpp					\
	examples/test_anonymous_module.hpp				\
	examples/test_module.hpp					\
	examples/utils.hpp						\
	files/files.hpp							\
	hdfs/hdfs.hpp							\
	hook/manager.hpp						\
	linux/cgroups.hpp						\
	linux/fs.hpp							\
	linux/ns.hpp							\
	linux/perf.hpp							\
	local/flags.hpp							\
	local/local.hpp							\
	logging/flags.hpp						\
	logging/logging.hpp						\
	master/contender.hpp						\
	master/constants.hpp						\
	master/detector.hpp						\
	master/flags.hpp						\
	master/master.hpp						\
	master/metrics.hpp						\
	master/repairer.hpp						\
	master/registrar.hpp						\
	master/validation.hpp						\
	master/allocator/mesos/allocator.hpp				\
	master/allocator/mesos/hierarchical.hpp				\
	master/allocator/sorter/drf/sorter.hpp				\
	master/allocator/sorter/sorter.hpp				\
	messages/messages.hpp						\
	module/manager.hpp						\
	sched/constants.hpp						\
	sched/flags.hpp							\
	slave/constants.hpp						\
	slave/flags.hpp							\
	slave/gc.hpp							\
	slave/metrics.hpp						\
	slave/monitor.hpp						\
	slave/paths.hpp							\
	slave/slave.hpp							\
	slave/state.hpp							\
	slave/status_update_manager.hpp					\
	slave/containerizer/containerizer.hpp				\
	slave/containerizer/fetcher.hpp					\
	slave/containerizer/external_containerizer.hpp			\
	slave/containerizer/launcher.hpp				\
	slave/containerizer/linux_launcher.hpp				\
	slave/containerizer/mesos/containerizer.hpp			\
	slave/containerizer/mesos/launch.hpp				\
	slave/containerizer/isolators/posix.hpp				\
	slave/containerizer/isolators/posix/disk.hpp			\
	slave/containerizer/isolators/cgroups/constants.hpp		\
	slave/containerizer/isolators/cgroups/cpushare.hpp		\
	slave/containerizer/isolators/cgroups/mem.hpp			\
	slave/containerizer/isolators/cgroups/perf_event.hpp		\
	slave/containerizer/isolators/namespaces/pid.hpp		\
	slave/containerizer/isolators/filesystem/shared.hpp		\
	tests/cluster.hpp						\
	tests/containerizer.hpp						\
	tests/environment.hpp						\
	tests/flags.hpp							\
	tests/isolator.hpp						\
	tests/launcher.hpp						\
	tests/limiter.hpp						\
	tests/memory_test_helper.hpp					\
	tests/mesos.hpp							\
	tests/module.hpp						\
	tests/script.hpp						\
	tests/setns_test_helper.hpp					\
	tests/utils.hpp							\
	tests/zookeeper.hpp						\
	tests/zookeeper_test_server.hpp					\
	usage/usage.hpp							\
	watcher/whitelist_watcher.hpp					\
	zookeeper/authentication.hpp					\
	zookeeper/contender.hpp						\
	zookeeper/detector.hpp						\
	zookeeper/group.hpp						\
	zookeeper/url.hpp						\
	zookeeper/watcher.hpp						\
	zookeeper/zookeeper.hpp

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
BUILD_FLAGS = $(echo $(MESOS_CPPFLAGS) $(CPPFLAGS) | sed 's/\"/\\\"/g')	\
              $(echo $(AM_CFLAGS) $(CFLAGS) | sed 's/\"/\\\"/g')	\
              $(echo $(AM_CXXFLAGS) $(CXXFLAGS) | sed 's/\"/\\\"/g')

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
nodist_liblog_la_SOURCES = $(LOG_PROTOS)
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
  state/in_memory.hpp							\
  state/leveldb.hpp							\
  state/log.hpp								\
  state/protobuf.hpp							\
  state/state.hpp							\
  state/storage.hpp							\
  state/zookeeper.hpp							\
  messages/state.hpp							\
  messages/state.proto
nodist_libstate_la_SOURCES = $(STATE_PROTOS)
libstate_la_CPPFLAGS = $(MESOS_CPPFLAGS)

libmesos_no_3rdparty_la_LIBADD += libstate.la

# The final result!
lib_LTLIBRARIES += libmesos.la

# Include as part of the distribution.
libmesos_la_SOURCES =							\
  $(ALLOCATOR_PROTO)							\
  $(CONTAINERIZER_PROTO)						\
  $(FETCHER_PROTO)							\
  $(MESOS_PROTO)							\
  $(MODULE_PROTO)							\
  $(SCHEDULER_PROTO)

libmesos_la_LDFLAGS = -release $(PACKAGE_VERSION)

# Since we just include the convenience library (and no sources), we
# need to tell libtool to build this as a C++ library.
libmesos_la_LIBTOOLFLAGS = --tag=CXX

# Add the convenience library.
libmesos_la_LIBADD = 							\
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


# Binaries.
sbin_PROGRAMS += mesos-master
mesos_master_SOURCES = master/main.cpp
mesos_master_CPPFLAGS = $(MESOS_CPPFLAGS)
mesos_master_LDADD = libmesos.la $(LDADD)

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

pkglibexec_PROGRAMS += mesos-containerizer
mesos_containerizer_SOURCES = slave/containerizer/mesos/main.cpp
mesos_containerizer_CPPFLAGS = $(MESOS_CPPFLAGS)
mesos_containerizer_LDADD = libmesos.la $(LDADD)

if WITH_NETWORK_ISOLATOR
pkglibexec_PROGRAMS += mesos-network-helper
mesos_network_helper_SOURCES = slave/containerizer/isolators/network/helper.cpp
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

# Also install the supporting scripts for the Python based CLI tools.
# TODO(benh): Use PIP and python/setup.py to do this correctly.
mesospythonpkglibexecdir = $(pkglibexecdir)/python/mesos

dist_mesospythonpkglibexec_SCRIPTS =					\
  cli/python/mesos/__init__.py						\
  cli/python/mesos/cli.py						\
  cli/python/mesos/futures.py						\
  cli/python/mesos/http.py

# Need to distribute/install webui javascript. We use 'pkgdatadir'
# instead of 'datadir' as the install directory so we get the the
# package name (i.e., 'mesos') as part of the path (i.e.,
# /path/to/mesos/webui versus something like /path/to/webui). Note
# that 'datadir' (e.g., /usr/local/share) is for read-only "data" and
# 'sysconfdir' (e.g., /usr/local/var) is for modifiable "data".
nobase_dist_pkgdata_DATA +=						\
  webui/master/static/js/controllers.js					\
  webui/master/static/js/app.js						\
  webui/master/static/js/services.js					\
  webui/master/static/js/jquery.pailer.js

# Need to distribute/install webui CSS.
nobase_dist_pkgdata_DATA +=						\
  webui/master/static/css/bootstrap-3.0.3.min.css			\
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
  webui/master/static/directives/timestamp.html				\
  webui/master/static/directives/pagination.html			\
  webui/master/static/directives/tableHeader.html


# Need to distribute/install webui images.
nobase_dist_pkgdata_DATA +=						\
  webui/master/static/img/loading.gif					\
  webui/master/static/ico/favicon.ico

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
  webui/master/static/js/ui-bootstrap-tpls-0.9.0.js			\
  webui/master/static/js/ui-bootstrap-tpls-0.9.0.min.js			\
  webui/master/static/js/underscore-1.4.3.js				\
  webui/master/static/js/underscore-1.4.3.min.js			\
  webui/master/static/js/jquery-1.7.1.js				\
  webui/master/static/js/jquery-1.7.1.min.js				\
  webui/master/static/js/relative-date.js				\
  webui/master/static/js/zeroclipboard-1.1.7.js				\
  webui/master/static/js/zeroclipboard-1.1.7.min.js			\
  webui/master/static/obj/zeroclipboard-1.1.7.swf

# And the deploy related stuff.
nodist_sbin_SCRIPTS += deploy/mesos-daemon.sh				\
  deploy/mesos-start-cluster.sh deploy/mesos-start-masters.sh		\
  deploy/mesos-start-slaves.sh deploy/mesos-stop-cluster.sh		\
  deploy/mesos-stop-masters.sh deploy/mesos-stop-slaves.sh

pkgsysconf_DATA += deploy/mesos-deploy-env.sh.template			\
                   deploy/mesos-master-env.sh.template			\
                   deploy/mesos-slave-env.sh.template

# Need to explicitly add this because by default DATA files are not
# included in distributions.
EXTRA_DIST += deploy/mesos-deploy-env.sh.template			\
              deploy/mesos-master-env.sh.template			\
              deploy/mesos-slave-env.sh.template

# Java related files are listed outside if HAS_JAVA so we can add them
# to EXTRA_DIST unconditionally.

# Mesos JAR.
MESOS_JAR = java/target/mesos-$(PACKAGE_VERSION).jar
MESOS_JAR_SOURCE =							\
	$(srcdir)/java/src/org/apache/mesos/ExecutorDriver.java		\
	$(srcdir)/java/src/org/apache/mesos/Executor.java		\
	$(srcdir)/java/src/org/apache/mesos/Log.java			\
	$(srcdir)/java/src/org/apache/mesos/MesosExecutorDriver.java	\
	$(srcdir)/java/src/org/apache/mesos/MesosSchedulerDriver.java	\
	$(srcdir)/java/src/org/apache/mesos/SchedulerDriver.java	\
	$(srcdir)/java/src/org/apache/mesos/Scheduler.java		\
	$(srcdir)/java/src/org/apache/mesos/state/AbstractState.java	\
	$(srcdir)/java/src/org/apache/mesos/state/InMemoryState.java	\
	$(srcdir)/java/src/org/apache/mesos/state/LevelDBState.java	\
	$(srcdir)/java/src/org/apache/mesos/state/LogState.java		\
	$(srcdir)/java/src/org/apache/mesos/state/State.java		\
	$(srcdir)/java/src/org/apache/mesos/state/Variable.java		\
	$(srcdir)/java/src/org/apache/mesos/state/ZooKeeperState.java
MESOS_JAR_GENERATED = $(JAVA_PROTOS)					\
	java/generated/org/apache/mesos/MesosNativeLibrary.java
EXTRA_DIST += $(MESOS_JAR_SOURCE)					\
	$(srcdir)/java/generated/org/apache/mesos/MesosNativeLibrary.java.in

# We build all the Java examples into their own JAR.
EXAMPLES_JAR = examples.jar
EXAMPLES_SOURCE =							\
	$(srcdir)/examples/java/TestExceptionFramework.java		\
	$(srcdir)/examples/java/TestExecutor.java			\
	$(srcdir)/examples/java/TestFramework.java			\
	$(srcdir)/examples/java/TestMultipleExecutorsFramework.java	\
	$(srcdir)/examples/java/TestLog.java
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
	java/jni/convert.cpp						\
	java/jni/convert.hpp						\
	java/jni/construct.cpp						\
	java/jni/construct.hpp						\
	java/jni/org_apache_mesos_Log.cpp				\
	java/jni/org_apache_mesos_MesosNativeLibrary.cpp		\
	java/jni/org_apache_mesos_MesosSchedulerDriver.cpp		\
	java/jni/org_apache_mesos_MesosExecutorDriver.cpp		\
	java/jni/org_apache_mesos_state_AbstractState.cpp		\
	java/jni/org_apache_mesos_state_LevelDBState.cpp		\
	java/jni/org_apache_mesos_state_LogState.cpp			\
	java/jni/org_apache_mesos_state_Variable.cpp			\
	java/jni/org_apache_mesos_state_ZooKeeperState.cpp		\
	jvm/jvm.cpp							\
	jvm/jvm.hpp							\
	jvm/java/io.hpp							\
	jvm/java/lang.hpp						\
	jvm/java/net.hpp						\
	jvm/org/apache/log4j.cpp					\
	jvm/org/apache/log4j.hpp					\
	jvm/org/apache/zookeeper.cpp					\
	jvm/org/apache/zookeeper.hpp

libjava_la_CPPFLAGS = $(MESOS_CPPFLAGS)
libjava_la_CPPFLAGS += $(JAVA_CPPFLAGS)
libjava_la_CPPFLAGS += -I$(srcdir)/java/jni -Ijava/jni

libjava_la_DEPENDENCIES = $(MESOS_PROTO) $(CONTAINERIZER_PROTO)

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
	java/jni/org_apache_mesos_MesosSchedulerDriver.h		\
	java/jni/org_apache_mesos_MesosExecutorDriver.h			\
	java/jni/org_apache_mesos_Log.h					\
	java/jni/org_apache_mesos_state_AbstractState.h			\
	java/jni/org_apache_mesos_state_LevelDBState.h			\
	java/jni/org_apache_mesos_state_LogState.h			\
	java/jni/org_apache_mesos_state_Variable.h			\
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
	python/src/mesos/__init__.py					\
	python/interface/src/mesos/__init__.py				\
	python/interface/src/mesos/interface/__init__.py		\
	python/native/src/mesos/__init__.py				\
	python/native/src/mesos/native/__init__.py			\
	python/native/src/mesos/native/mesos_executor_driver_impl.cpp	\
	python/native/src/mesos/native/mesos_executor_driver_impl.hpp	\
	python/native/src/mesos/native/mesos_scheduler_driver_impl.cpp	\
	python/native/src/mesos/native/mesos_scheduler_driver_impl.hpp	\
	python/native/src/mesos/native/module.cpp			\
	python/native/src/mesos/native/module.hpp			\
	python/native/src/mesos/native/proxy_executor.cpp		\
	python/native/src/mesos/native/proxy_executor.hpp		\
	python/native/src/mesos/native/proxy_scheduler.cpp		\
	python/native/src/mesos/native/proxy_scheduler.hpp

EXTRA_DIST += $(PYTHON_SOURCE)

if HAS_PYTHON

if WITH_BUNDLED_PROTOBUF
  # Build a protobuf Python egg.
  PROTOBUF_EGG_SUFFIX = $(PROTOBUF_VERSION)$(PYTHON_EGG_PUREPY_POSTFIX).egg
  PROTOBUF_EGG = ../$(PROTOBUF)/python/dist/protobuf-$(PROTOBUF_EGG_SUFFIX)

  $(PROTOBUF_EGG):
	@echo "Building protobuf Python egg ..."
	cd ../$(PROTOBUF)/python && \
          CC="$(CC)" CXX="$(CXX)" CFLAGS="$(CFLAGS)" CXXFLAGS="$(CXXFLAGS)" \
          PYTHONPATH=$(abs_top_builddir)/$(DISTRIBUTE) $(PYTHON) setup.py build bdist_egg

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
	python/dist/mesos.interface-$(PACKAGE_VERSION)$(PYTHON_EGG_PUREPY_POSTFIX).egg	\
	python/dist/mesos.native-$(PACKAGE_VERSION)$(PYTHON_EGG_POSTFIX).egg

MESOS_WHLS =										\
	python/dist/mesos.interface-$(PACKAGE_VERSION)$(PYTHON_WHL_PUREPY_POSTFIX).whl	\
	python/dist/mesos.native-$(PACKAGE_VERSION)$(PYTHON_WHL_POSTFIX).whl

# The python source is in directories of the form: python/interface. The make
# target is of the form: python/dist/mesos.interface-0.20.0-py2.7.egg. To build
# this, we must first cd into it. The first line of this takes the make target
# and converts it into the correct directory path.
$(MESOS_EGGS) $(MESOS_WHLS):								\
		$(PYTHON_PROTOS)							\
		$(PYTHON_SOURCE)							\
		libmesos_no_3rdparty.la							\
		$(PROTOBUF_EGG)
	@cd python/`echo $@ | awk -F"mesos[.]?" '{print $$2}' | cut -d- -f1` &&		\
	LIBS="$(LIBS)" CC="$(CC)" CXX="$(CXX)"						\
	CFLAGS="$(PYTHON_CFLAGS)" CPPFLAGS="$(PYTHON_CPPFLAGS)"				\
	LDFLAGS="$(PYTHON_LDFLAGS)"							\
	PYTHONPATH=$(abs_top_builddir)/$(DISTRIBUTE):$(abs_top_builddir)/$(WHEEL)	\
	$(PYTHON) setup.py bdist_egg --dist-dir=../dist					\
	bdist_wheel --dist-dir=../dist

CLEANFILES += $(MESOS_EGGS) $(MESOS_WHLS) python/*/build python/*/dist $(PYTHON_SOURCE)

pippythonpath=$(DESTDIR)$(pythondir):$(abs_top_builddir)/$(DISTRIBUTE):$(abs_top_builddir)/$(PIP):$(abs_top_builddir)/$(WHEEL)

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
	cd python/dist &&							\
	for whl in $(MESOS_WHLS); do						\
		PYTHONPATH=$(pippythonpath)					\
		PYTHONUSERBASE=$(DESTDIR)$(prefix)				\
		$(PYTHON) -c "import pip; pip.main()" install			\
		--user $(pipinstallargs)					\
		--find-links=file://$(abs_top_builddir)/src/python/dist		\
		$(abs_top_builddir)/src/$$whl;					\
	done

uninstall-local:
	for whl in $(MESOS_WHLS); do						\
		PYTHONPATH=$(DESTDIR)$(pythondir):$(pippythonpath)		\
		$(PYTHON) -c "import pip; pip.main()" uninstall			\
		--yes $$(echo $$whl | cut -d/ -f3 | cut -d- -f1);		\
	done

# Make sure the egg gets built via 'make all'.
all-local: $(MESOS_EGGS)
endif # HAS_PYTHON

clean-python:
	find python -name "build" -o -name "dist" -o -name "*.pyc"		\
		-o -name "*.egg-info" | xargs rm -rf

PHONY_TARGETS += clean-python

# Test (make check) binaries.
check_PROGRAMS += low-level-scheduler-libprocess
low_level_scheduler_libprocess_SOURCES = examples/low_level_scheduler_libprocess.cpp
low_level_scheduler_libprocess_CPPFLAGS = $(MESOS_CPPFLAGS)
low_level_scheduler_libprocess_LDADD = libmesos.la $(LDADD)

check_PROGRAMS += low-level-scheduler-pthread
low_level_scheduler_pthread_SOURCES = examples/low_level_scheduler_pthread.cpp
low_level_scheduler_pthread_CPPFLAGS = $(MESOS_CPPFLAGS)
low_level_scheduler_pthread_LDADD = libmesos.la $(LDADD)

check_PROGRAMS += test-framework
test_framework_SOURCES = examples/test_framework.cpp
test_framework_CPPFLAGS = $(MESOS_CPPFLAGS)
test_framework_LDADD = libmesos.la $(LDADD)

check_PROGRAMS += test-executor
test_executor_SOURCES = examples/test_executor.cpp
test_executor_CPPFLAGS = $(MESOS_CPPFLAGS)
test_executor_LDADD = libmesos.la $(LDADD)

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
    tests/setns_test_helper_main.cpp				\
    tests/setns_test_helper.cpp
  setns_test_helper_CPPFLAGS = $(MESOS_CPPFLAGS)
  setns_test_helper_LDADD = libmesos.la $(LDADD)

endif

check_PROGRAMS += memory-test-helper
memory_test_helper_SOURCES =					\
  tests/flags.cpp						\
  tests/memory_test_helper_main.cpp				\
  tests/memory_test_helper.cpp
memory_test_helper_CPPFLAGS = $(mesos_tests_CPPFLAGS)
memory_test_helper_LDADD = libmesos.la $(LDADD)

check_PROGRAMS += active-user-test-helper
active_user_test_helper_SOURCES = tests/active_user_test_helper.cpp
active_user_test_helper_CPPFLAGS = $(MESOS_CPPFLAGS)
active_user_test_helper_LDADD = libmesos.la $(LDADD)

check_PROGRAMS += mesos-tests

# LDFLAGS to be used for the module libraries.
# We need to build the module libraries for running the test suite but don't
# need to install them.  The 'noinst_' prefix ensures that these libraries will
# not be installed.  However, it also skips building the shared libraries. The
# workaround is to use '-rpath /nowhere' to force libtool to build the shared
# library.
MESOS_MODULE_LDFLAGS = -release $(PACKAGE_VERSION) -shared -rpath /nowhere

# Library containing an example module.
noinst_LTLIBRARIES += libexamplemodule.la
libexamplemodule_la_SOURCES = examples/example_module_impl.cpp
libexamplemodule_la_CPPFLAGS = $(MESOS_CPPFLAGS)
libexamplemodule_la_LDFLAGS = $(MESOS_MODULE_LDFLAGS)

# Library containing the test anonymous modules.
noinst_LTLIBRARIES += libtestanonymous.la
libtestanonymous_la_SOURCES = examples/test_anonymous_module.cpp
libtestanonymous_la_CPPFLAGS = $(MESOS_CPPFLAGS)
libtestanonymous_la_LDFLAGS = $(MESOS_MODULE_LDFLAGS)

# Library containing the test CRAM-MD5 authentication modules.
# TODO(tillt): Add cyrus-sasl2 dependency while removing it from libmesos.
# TODO(tillt): Enable optional building of this module library.
# TODO(tillt): Make this module library installable. See MESOS-1940.
noinst_LTLIBRARIES += libtestauthentication.la
libtestauthentication_la_SOURCES = examples/test_authentication_modules.cpp
libtestauthentication_la_CPPFLAGS = $(MESOS_CPPFLAGS)
libtestauthentication_la_LDFLAGS = $(MESOS_MODULE_LDFLAGS)

# Library containing the test DRF allocator module.
noinst_LTLIBRARIES += libtestallocator.la
libtestallocator_la_SOURCES = examples/test_allocator_module.cpp
libtestallocator_la_CPPFLAGS = $(MESOS_CPPFLAGS)
libtestallocator_la_LDFLAGS = $(MESOS_MODULE_LDFLAGS)

# Library containing test Hook module.
noinst_LTLIBRARIES += libtesthook.la
libtesthook_la_SOURCES = examples/test_hook_module.cpp
libtesthook_la_CPPFLAGS = $(MESOS_CPPFLAGS)
libtesthook_la_LDFLAGS = $(MESOS_MODULE_LDFLAGS)

# Library containing test CPU and memory isolator modules.
noinst_LTLIBRARIES += libtestisolator.la
libtestisolator_la_SOURCES = examples/test_isolator_module.cpp
libtestisolator_la_CPPFLAGS = $(MESOS_CPPFLAGS)
libtestisolator_la_LDFLAGS = $(MESOS_MODULE_LDFLAGS)

mesos_tests_SOURCES =				\
  tests/anonymous_tests.cpp			\
  tests/attributes_tests.cpp			\
  tests/authentication_tests.cpp		\
  tests/authorization_tests.cpp		        \
  tests/common/http_tests.cpp			\
  tests/composing_containerizer_tests.cpp       \
  tests/containerizer.cpp			\
  tests/containerizer_tests.cpp			\
  tests/cram_md5_authentication_tests.cpp	\
  tests/credentials_tests.cpp			\
  tests/disk_quota_tests.cpp			\
  tests/docker_containerizer_tests.cpp          \
  tests/docker_tests.cpp			\
  tests/environment.cpp				\
  tests/examples_tests.cpp			\
  tests/exception_tests.cpp			\
  tests/external_containerizer_test.cpp		\
  tests/health_check_tests.cpp                  \
  tests/fault_tolerance_tests.cpp		\
  tests/fetcher_tests.cpp                       \
  tests/files_tests.cpp				\
  tests/flags.cpp				\
  tests/gc_tests.cpp				\
  tests/hierarchical_allocator_tests.cpp	\
  tests/hook_tests.cpp				\
  tests/isolator_tests.cpp			\
  tests/log_tests.cpp				\
  tests/logging_tests.cpp			\
  tests/main.cpp				\
  tests/master_allocator_tests.cpp		\
  tests/master_authorization_tests.cpp		\
  tests/master_contender_detector_tests.cpp	\
  tests/master_slave_reconciliation_tests.cpp	\
  tests/master_tests.cpp			\
  tests/master_validation_tests.cpp		\
  tests/memory_test_helper.cpp			\
  tests/mesos.cpp				\
  tests/metrics_tests.cpp			\
  tests/module.cpp				\
  tests/module_tests.cpp			\
  tests/monitor_tests.cpp			\
  tests/partition_tests.cpp			\
  tests/paths_tests.cpp				\
  tests/persistent_volume_tests.cpp		\
  tests/protobuf_io_tests.cpp			\
  tests/rate_limiting_tests.cpp			\
  tests/reconciliation_tests.cpp		\
  tests/registrar_tests.cpp			\
  tests/repair_tests.cpp			\
  tests/resource_offers_tests.cpp		\
  tests/resources_tests.cpp			\
  tests/scheduler_tests.cpp			\
  tests/script.cpp				\
  tests/slave_recovery_tests.cpp		\
  tests/slave_tests.cpp				\
  tests/sorter_tests.cpp			\
  tests/state_tests.cpp				\
  tests/status_update_manager_tests.cpp		\
  tests/teardown_tests.cpp			\
  tests/utils.cpp				\
  tests/values_tests.cpp			\
  tests/zookeeper_url_tests.cpp

mesos_tests_CPPFLAGS = $(MESOS_CPPFLAGS)
mesos_tests_CPPFLAGS += -DSOURCE_DIR=\"$(abs_top_srcdir)\"
mesos_tests_CPPFLAGS += -DBUILD_DIR=\"$(abs_top_builddir)\"
mesos_tests_CPPFLAGS += -I../$(GTEST)/include
mesos_tests_CPPFLAGS += -I../$(GMOCK)/include

mesos_tests_LDADD = ../$(LIBPROCESS)/3rdparty/libgmock.la libmesos.la -ldl $(LDADD)

mesos_tests_DEPENDENCIES = # Initialized to allow += below.

if OS_LINUX
  mesos_tests_SOURCES += tests/cgroups_isolator_tests.cpp
  mesos_tests_SOURCES += tests/cgroups_tests.cpp
  mesos_tests_SOURCES += tests/fs_tests.cpp
  mesos_tests_SOURCES += tests/memory_pressure_tests.cpp
  mesos_tests_SOURCES += tests/ns_tests.cpp
  mesos_tests_SOURCES += tests/perf_tests.cpp
  mesos_tests_SOURCES += tests/setns_test_helper.cpp
endif

if WITH_NETWORK_ISOLATOR
  mesos_tests_SOURCES += tests/routing_tests.cpp
  mesos_tests_SOURCES += tests/port_mapping_tests.cpp
endif

if HAS_JAVA
  mesos_tests_SOURCES += tests/group_tests.cpp				\
			 tests/registrar_zookeeper_tests.cpp		\
			 tests/zookeeper.cpp				\
                         tests/zookeeper_test_server.cpp		\
                         tests/zookeeper_tests.cpp

  mesos_tests_CPPFLAGS += $(JAVA_CPPFLAGS)
  mesos_tests_CPPFLAGS += -DZOOKEEPER_VERSION=\"$(ZOOKEEPER_VERSION)\"
  mesos_tests_LDFLAGS = $(AM_LDFLAGS)
  mesos_tests_DEPENDENCIES += $(EXAMPLES_JAR)

  EXAMPLESCRIPTSJAVA = examples/java/test-framework			\
                       examples/java/test-executor			\
                       examples/java/test-exception-framework		\
                       examples/java/test-multiple-executors-framework	\
                       examples/java/test-log

  check_SCRIPTS += $(EXAMPLESCRIPTSJAVA)
  mesos_tests_DEPENDENCIES += $(EXAMPLESCRIPTSJAVA)
endif

if HAS_PYTHON
  mesos_tests_DEPENDENCIES += $(MESOS_EGG)

  EXAMPLESCRIPTSPYTHON = examples/python/test_containerizer.py		\
			 examples/python/test-containerizer		\
			 examples/python/test_framework.py		\
			 examples/python/test-framework			\
			 examples/python/test_executor.py		\
			 examples/python/test-executor

  check_SCRIPTS += $(EXAMPLESCRIPTSPYTHON)
  mesos_tests_DEPENDENCIES += $(EXAMPLESCRIPTSPYTHON)
endif

EXTRA_DIST += examples/python/test_containerizer.py			\
	      examples/python/test_executor.py				\
	      examples/python/test_framework.py


dist_check_SCRIPTS +=							\
  tests/balloon_framework_test.sh					\
  tests/low_level_scheduler_libprocess_test.sh				\
  tests/low_level_scheduler_pthread_test.sh				\
  tests/java_exception_test.sh						\
  tests/java_framework_test.sh						\
  tests/java_log_test.sh						\
  tests/no_executor_framework_test.sh					\
  tests/persistent_volume_framework_test.sh				\
  tests/python_framework_test.sh					\
  tests/test_framework_test.sh

# We use a check-local target for now to avoid the parallel test
# runner that ships with newer versions of autotools.
# See the following discussion for the workaround:
# http://lists.gnu.org/archive/html/automake/2013-01/msg00051.html
check-local: mesos-tests
	./mesos-tests

clean-local: clean-java clean-python

.PHONY: $(PHONY_TARGETS)
