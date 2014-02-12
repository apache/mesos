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

include ../3rdparty/versions.am
include ../3rdparty/libprocess/3rdparty/versions.am

# TODO(charles): Move these into an included automakefile and have
# them include $(top_builddir) as appropriate.
DISTRIBUTE = 3rdparty/distribute-$(DISTRIBUTE_VERSION)
LEVELDB = 3rdparty/leveldb
ZOOKEEPER = 3rdparty/zookeeper-$(ZOOKEEPER_VERSION)/src/c
LIBPROCESS = 3rdparty/libprocess
STOUT = $(LIBPROCESS)/3rdparty/stout
BOOST = $(LIBPROCESS)/3rdparty/boost-$(BOOST_VERSION)
GLOG = $(LIBPROCESS)/3rdparty/glog-$(GLOG_VERSION)
GMOCK = $(LIBPROCESS)/3rdparty/gmock-$(GMOCK_VERSION)
GTEST = $(GMOCK)/gtest
PROTOBUF = $(LIBPROCESS)/3rdparty/protobuf-$(PROTOBUF_VERSION)


# Unfortunatley, 'pkglibexecdir' and 'pkglocalstatedir' are not set
# before automake 1.11, so we need to set them manually (until we in
# the future assume an automake version).
pkglibexecdir = $(libexecdir)/$(PACKAGE)
pkglocalstatedir = $(localstatedir)/$(PACKAGE)

# Protocol buffer compiler.
PROTOC = ../$(PROTOBUF)/src/protoc
PROTOCFLAGS = -I$(top_srcdir)/include/mesos -I$(srcdir)

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
nobase_pkglocalstate_DATA =
check_PROGRAMS =
dist_check_SCRIPTS =
check_SCRIPTS =
BUILT_SOURCES =
CLEANFILES =
EXTRA_DIST =
PHONY_TARGETS =

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
MESOS_CPPFLAGS += -I../$(BOOST)
MESOS_CPPFLAGS += -I../$(PROTOBUF)/src
MESOS_CPPFLAGS += -I../$(GLOG)/src

if WITH_INCLUDED_ZOOKEEPER
  MESOS_CPPFLAGS += -I../$(ZOOKEEPER)/include
  MESOS_CPPFLAGS += -I../$(ZOOKEEPER)/generated
endif


# README: we build the Mesos library out of a collection of
# convenience libraries (that is, libraries that do not get installed
# but we can use as building blocks to vary compile flags as necessary
# and then aggregate into final archives): libmesos_no_3rdparty.la
# libbuild.la, liblog.la, libjava.la.

# First, let's define necessary protocol buffer files.

MESOS_PROTO = $(top_srcdir)/include/mesos/mesos.proto

CXX_PROTOS = mesos.pb.cc mesos.pb.h
JAVA_PROTOS = java/generated/org/apache/mesos/Protos.java
PYTHON_PROTOS = python/src/mesos_pb2.py

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
%.pb.cc %.pb.h: $(top_srcdir)/include/mesos/%.proto
	$(MKDIR_P) $(@D)
	$(PROTOC) $(PROTOCFLAGS) --cpp_out=. $^

%.pb.cc %.pb.h: %.proto
	$(MKDIR_P) $(@D)
	$(PROTOC) $(PROTOCFLAGS) --cpp_out=. $^

$(JAVA_PROTOS): $(MESOS_PROTO)
	$(MKDIR_P)  $(@D)
	$(PROTOC) $(PROTOCFLAGS) --java_out=java/generated $^

$(PYTHON_PROTOS): $(MESOS_PROTO)
	$(MKDIR_P) $(@D)
	$(PROTOC) $(PROTOCFLAGS) --python_out=python/src $^

# We even use a convenience library for most of Mesos so that we can
# exclude third party libraries so setuptools/distribute can build a
# self-contained Python library and statically link in the third party
# libraries themselves.
noinst_LTLIBRARIES += libmesos_no_3rdparty.la

nodist_libmesos_no_3rdparty_la_SOURCES =				\
  $(CXX_PROTOS)								\
  $(MESSAGES_PROTOS)							\
  $(REGISTRY_PROTOS)

libmesos_no_3rdparty_la_SOURCES =					\
	sasl/authenticatee.hpp						\
	sasl/authenticator.hpp						\
	sasl/auxprop.hpp						\
	sasl/auxprop.cpp						\
	sched/sched.cpp							\
	local/local.cpp							\
	master/contender.cpp						\
	master/constants.cpp						\
	master/detector.cpp						\
	master/drf_sorter.cpp						\
	master/http.cpp							\
	master/master.cpp						\
	master/registry.hpp						\
	master/registry.proto                                           \
	master/registrar.cpp						\
	slave/constants.cpp						\
	slave/gc.cpp							\
	slave/monitor.cpp						\
	slave/state.cpp							\
	slave/slave.cpp							\
	slave/http.cpp							\
	slave/containerizer/containerizer.cpp				\
	slave/containerizer/isolator.cpp				\
	slave/containerizer/launcher.cpp				\
	slave/containerizer/mesos_containerizer.cpp			\
	slave/status_update_manager.cpp					\
	exec/exec.cpp							\
	common/lock.cpp							\
	common/date_utils.cpp						\
	common/resources.cpp						\
	common/attributes.cpp						\
	common/values.cpp						\
	files/files.cpp							\
	logging/logging.cpp						\
	zookeeper/contender.cpp						\
	zookeeper/detector.cpp						\
	zookeeper/zookeeper.cpp						\
	zookeeper/authentication.cpp					\
	zookeeper/group.cpp						\
	messages/messages.proto

pkginclude_HEADERS = $(top_srcdir)/include/mesos/executor.hpp	\
		     $(top_srcdir)/include/mesos/scheduler.hpp	\
                     $(top_srcdir)/include/mesos/resources.hpp	\
                     $(top_srcdir)/include/mesos/values.hpp	\
		     $(top_srcdir)/include/mesos/mesos.proto

nodist_pkginclude_HEADERS = ../include/mesos/mesos.hpp mesos.pb.h

if OS_LINUX
  libmesos_no_3rdparty_la_SOURCES += linux/cgroups.cpp
  libmesos_no_3rdparty_la_SOURCES += slave/containerizer/isolators/cgroups/cpushare.cpp
  libmesos_no_3rdparty_la_SOURCES += slave/containerizer/isolators/cgroups/mem.cpp
  libmesos_no_3rdparty_la_SOURCES += slave/containerizer/cgroups_launcher.cpp
  libmesos_no_3rdparty_la_SOURCES += linux/fs.cpp
else
  EXTRA_DIST += linux/cgroups.cpp
  EXTRA_DIST += linux/fs.cpp
endif

libmesos_no_3rdparty_la_SOURCES += common/attributes.hpp		\
	common/build.hpp common/date_utils.hpp common/factory.hpp	\
	common/protobuf_utils.hpp					\
	common/lock.hpp							\
	common/type_utils.hpp common/thread.hpp				\
	examples/utils.hpp files/files.hpp				\
	hdfs/hdfs.hpp							\
	linux/cgroups.hpp						\
	linux/fs.hpp local/flags.hpp local/local.hpp			\
	logging/flags.hpp logging/logging.hpp				\
	master/allocator.hpp						\
	master/contender.hpp						\
	master/constants.hpp						\
	master/detector.hpp						\
	master/drf_sorter.hpp master/flags.hpp				\
	master/hierarchical_allocator_process.hpp			\
	master/registrar.hpp						\
	master/master.hpp master/sorter.hpp				\
	messages/messages.hpp slave/constants.hpp			\
	slave/containerizer/cgroups_launcher.hpp			\
	slave/containerizer/containerizer.hpp				\
	slave/containerizer/isolator.hpp				\
	slave/containerizer/isolators/cgroups/cpushare.hpp		\
	slave/containerizer/isolators/cgroups/mem.hpp			\
	slave/containerizer/isolators/posix.hpp				\
	slave/containerizer/launcher.hpp				\
	slave/containerizer/mesos_containerizer.hpp			\
	slave/flags.hpp slave/gc.hpp slave/monitor.hpp			\
	slave/paths.hpp slave/state.hpp					\
	slave/status_update_manager.hpp					\
	slave/slave.hpp							\
	tests/environment.hpp tests/script.hpp				\
	tests/zookeeper.hpp tests/flags.hpp tests/utils.hpp		\
	tests/cluster.hpp						\
	tests/containerizer.hpp						\
	tests/mesos.hpp							\
	tests/zookeeper_test_server.hpp zookeeper/authentication.hpp	\
	zookeeper/contender.hpp						\
	zookeeper/detector.hpp						\
	zookeeper/group.hpp zookeeper/watcher.hpp			\
	zookeeper/zookeeper.hpp zookeeper/url.hpp

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
  log/tool/initialize.cpp						\
  log/tool/read.cpp
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
  log/tool/initialize.hpp						\
  log/tool/read.hpp							\
  messages/log.hpp							\
  messages/log.proto
nodist_liblog_la_SOURCES = $(LOG_PROTOS)
liblog_la_CPPFLAGS = -I../$(LEVELDB)/include $(MESOS_CPPFLAGS)

libmesos_no_3rdparty_la_LIBADD += liblog.la


# Convenience library for building "state" abstraction in order to
# include the leveldb headers.
noinst_LTLIBRARIES += libstate.la
libstate_la_SOURCES = state/leveldb.cpp state/zookeeper.cpp
libstate_la_SOURCES +=							\
  state/leveldb.hpp							\
  state/protobuf.hpp							\
  state/state.hpp							\
  state/storage.hpp							\
  state/zookeeper.hpp							\
  messages/state.hpp							\
  messages/state.proto
nodist_libstate_la_SOURCES = $(STATE_PROTOS)
libstate_la_CPPFLAGS = -I../$(LEVELDB)/include $(MESOS_CPPFLAGS)

libmesos_no_3rdparty_la_LIBADD += libstate.la

# The final result!
lib_LTLIBRARIES += libmesos.la

libmesos_la_SOURCES = $(MESOS_PROTO) # Include as part of the distribution.

libmesos_la_LDFLAGS = -release $(PACKAGE_VERSION) -shared

# Since we just include the convenience library (and no sources), we
# need to tell libtool to build this as a C++ library.
libmesos_la_LIBTOOLFLAGS = --tag=CXX

# Add the convenience library.
libmesos_la_LIBADD = libmesos_no_3rdparty.la

# For non-convenience libraries we need to link them in to make the shared
# library each time. (Currently, we don't support platforms where this is not
# possible.)
libmesos_la_LIBADD += ../$(PROTOBUF)/src/libprotobuf.la
libmesos_la_LIBADD += ../$(GLOG)/libglog.la

# We need to directly include the leveldb library in order to avoid
# the installed libmesos.la file to include leveldb in
# 'dependency_libs' (via '-L../3rdparty/leveldb -lleveldb').
libmesos_la_LIBADD += ../$(LEVELDB)/libleveldb.a

if WITH_INCLUDED_ZOOKEEPER
  libmesos_la_LIBADD += ../$(ZOOKEEPER)/libzookeeper_mt.la
endif

libmesos_la_LIBADD += ../$(LIBPROCESS)/libprocess.la


# Binaries.
sbin_PROGRAMS += mesos-master
mesos_master_SOURCES = master/main.cpp
mesos_master_CPPFLAGS = $(MESOS_CPPFLAGS)
mesos_master_LDADD = libmesos.la

sbin_PROGRAMS += mesos-slave
mesos_slave_SOURCES = slave/main.cpp
mesos_slave_CPPFLAGS = $(MESOS_CPPFLAGS)
mesos_slave_LDADD = libmesos.la

bin_PROGRAMS += mesos-local
mesos_local_SOURCES = local/main.cpp
mesos_local_CPPFLAGS = $(MESOS_CPPFLAGS)
mesos_local_LDADD = libmesos.la

pkglibexec_PROGRAMS += mesos-fetcher
mesos_fetcher_SOURCES = launcher/fetcher.cpp
mesos_fetcher_CPPFLAGS = $(MESOS_CPPFLAGS)
mesos_fetcher_LDADD = libmesos.la

pkglibexec_PROGRAMS += mesos-executor
mesos_executor_SOURCES = launcher/executor.cpp
mesos_executor_CPPFLAGS = $(MESOS_CPPFLAGS)
mesos_executor_LDADD = libmesos.la

bin_PROGRAMS += mesos-log
mesos_log_SOURCES = log/main.cpp
mesos_log_CPPFLAGS = $(MESOS_CPPFLAGS)
mesos_log_LDADD = libmesos.la

bin_PROGRAMS += mesos
mesos_SOURCES = cli/mesos.cpp
mesos_CPPFLAGS = $(MESOS_CPPFLAGS)
mesos_LDADD = libmesos.la

bin_PROGRAMS += mesos-execute
mesos_execute_SOURCES = cli/execute.cpp
mesos_execute_CPPFLAGS = $(MESOS_CPPFLAGS)
mesos_execute_LDADD = libmesos.la

bin_PROGRAMS += mesos-resolve
mesos_resolve_SOURCES = cli/resolve.cpp
mesos_resolve_CPPFLAGS = $(MESOS_CPPFLAGS)
mesos_resolve_LDADD = libmesos.la

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
# 'localstatedir' (e.g., /usr/local/var) is for modifiable "data".
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

nobase_pkglocalstate_DATA += deploy/mesos-deploy-env.sh.template	\
                             deploy/mesos-master-env.sh.template	\
                             deploy/mesos-slave-env.sh.template

# Need to explicitly add this because by default DATA files are not
# included in distributions.
EXTRA_DIST += deploy/mesos-deploy-env.sh.template			\
              deploy/mesos-master-env.sh.template			\
              deploy/mesos-slave-env.sh.template

# Java related files are listed outside if HAS_JAVA so we can add them
# to EXTRA_DIST unconditionally.

# Mesos JAR.
MESOS_JAR = mesos-$(PACKAGE_VERSION).jar
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
	$(srcdir)/examples/java/TestMultipleExecutorsFramework.java
EXTRA_DIST += $(EXAMPLES_SOURCE)


if HAS_JAVA
# Protocol buffers JAR.
PROTOBUF_JAR = ../protobuf-$(PROTOBUF_VERSION).jar

# TODO(charles): Move into 3rdparty/Makefile.am.
$(PROTOBUF_JAR): # TODO(charles): Specify dependencies for the jar.
	@echo "Building protobuf-$(PROTOBUF_VERSION).jar ..."
	$(MKDIR_P) ../$(PROTOBUF)/java/src/main/java
	$(PROTOC) --java_out=../$(PROTOBUF)/java/src/main/java		\
          -I../$(PROTOBUF)/src						\
          ../$(PROTOBUF)/src/google/protobuf/descriptor.proto
	$(JAVA_HOME)/bin/javac -source 1.6 -target 1.6			\
          -d ../$(PROTOBUF)/java/src/main/java				\
          ../$(PROTOBUF)/java/src/main/java/com/google/protobuf/*.java	\
          ../$(PROTOBUF)/java/src/main/java/com/google/protobuf/*.java
	$(JAVA_HOME)/bin/jar cf $@ -C ../$(PROTOBUF)/java/src/main/java com

CLEANFILES += $(PROTOBUF_JAR)


$(MESOS_JAR): $(MESOS_JAR_SOURCE) $(MESOS_JAR_GENERATED) $(PROTOBUF_JAR)
	@echo "Building mesos-$(PACKAGE_VERSION).jar ..."
	-rm -rf java/classes/org/apache/mesos
	$(MKDIR_P) java/classes/org/apache/mesos
	$(JAVA_HOME)/bin/javac -source 1.6 -target 1.6			\
          -cp $(PROTOBUF_JAR)						\
          -d java/classes $(MESOS_JAR_SOURCE) $(MESOS_JAR_GENERATED)
	$(JAVA_HOME)/bin/jar cf $@ -C java/classes org

CLEANFILES += $(MESOS_JAR)


MESOS_SOURCES_JAR = ../src/mesos-$(PACKAGE_VERSION)-sources.jar

$(MESOS_SOURCES_JAR): $(MESOS_JAR_SOURCE)
	-rm -f $(MESOS_SOURCES_JAR)
	$(JAVA_HOME)/bin/jar cf $@ -C $(srcdir)/java/src org
	$(JAVA_HOME)/bin/jar uf $@ -C java/generated org

CLEANFILES += $(MESOS_SOURCES_JAR)


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
	java/jni/org_apache_mesos_MesosSchedulerDriver.cpp		\
	java/jni/org_apache_mesos_MesosExecutorDriver.cpp		\
	java/jni/org_apache_mesos_state_AbstractState.cpp		\
	java/jni/org_apache_mesos_state_LevelDBState.cpp		\
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

libjava_la_DEPENDENCIES = $(MESOS_PROTO)

libjava_la_LIBADD = $(JAVA_LDFLAGS)

# We don't add libjava.la to libmesos_no_3rdparty.la so we don't
# include the JNI bindings in the Python egg (but we might want to
# reconsider this in the future). We also need to add the JAVA_LDFLAGS
# to libmesos.la so on Linux we set the rpath for libmesos.so.
libmesos_la_LIBADD += libjava.la
libmesos_la_LIBADD += $(JAVA_LDFLAGS)

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
	java/jni/org_apache_mesos_state_Variable.h			\
	java/jni/org_apache_mesos_state_ZooKeeperState.h

BUILT_SOURCES += $(nodist_libjava_la_SOURCES)

java/jni/org_apache_mesos_MesosSchedulerDriver.h: $(MESOS_JAR)
	$(JAVA_HOME)/bin/javah -d java/jni -classpath			\
          $(PROTOBUF_JAR):java/classes					\
          org.apache.mesos.MesosSchedulerDriver

java/jni/org_apache_mesos_MesosExecutorDriver.h: $(MESOS_JAR)
	$(JAVA_HOME)/bin/javah -d java/jni -classpath			\
          $(PROTOBUF_JAR):java/classes					\
          org.apache.mesos.MesosExecutorDriver

java/jni/org_apache_mesos_Log.h: $(MESOS_JAR)
	$(JAVA_HOME)/bin/javah -d java/jni -classpath			\
          $(PROTOBUF_JAR):java/classes org.apache.mesos.Log

java/jni/org_apache_mesos_state_AbstractState.h: $(MESOS_JAR)
	$(JAVA_HOME)/bin/javah -d java/jni -classpath		\
          $(PROTOBUF_JAR):java/classes org.apache.mesos.state.AbstractState

java/jni/org_apache_mesos_state_LevelDBState.h: $(MESOS_JAR)
	$(JAVA_HOME)/bin/javah -d java/jni -classpath		\
          $(PROTOBUF_JAR):java/classes org.apache.mesos.state.LevelDBState

java/jni/org_apache_mesos_state_Variable.h: $(MESOS_JAR)
	$(JAVA_HOME)/bin/javah -d java/jni -classpath			\
          $(PROTOBUF_JAR):java/classes org.apache.mesos.state.Variable

java/jni/org_apache_mesos_state_ZooKeeperState.h: $(MESOS_JAR)
	$(JAVA_HOME)/bin/javah -d java/jni -classpath			\
          $(PROTOBUF_JAR):java/classes org.apache.mesos.state.ZooKeeperState

$(EXAMPLES_JAR): $(EXAMPLES_SOURCE)
	@echo "Building examples.jar ..."
	$(MKDIR_P) examples/java
	$(JAVA_HOME)/bin/javac -source 1.6 -target 1.6			\
          -cp $(PROTOBUF_JAR):$(MESOS_JAR):$(srcdir)/examples/java	\
          -sourcepath $(srcdir)/examples/java -d examples/java	        \
          $(srcdir)/examples/java/*.java
	$(JAVA_HOME)/bin/jar cf $@ -C examples/java .

CLEANFILES += $(EXAMPLES_JAR)

maven-install: $(MESOS_JAR) $(MESOS_SOURCES_JAR) java/mesos.pom
	mvn install:install-file -Dfile=$(MESOS_JAR)			\
          -Dsources=$(MESOS_SOURCES_JAR) -DpomFile=java/mesos.pom

PHONY_TARGETS += maven-install
endif # HAS_JAVA

# We remove mesos-*.jar here to make sure any older versions of the
# jar are removed.
clean-java:
	-rm -rf java/classes
	-rm -f examples/java/*.class
	-rm -f java/jni/org_apache_mesos*.h
	-rm -f mesos-*.jar

PHONY_TARGETS += clean-java


# Python files listed outside HAS_PYTHON so they are included with the
# distribution unconditionally.
EXTRA_DIST += python/src/mesos.py					\
	      python/native/mesos_executor_driver_impl.cpp		\
	      python/native/mesos_executor_driver_impl.hpp		\
	      python/native/mesos_scheduler_driver_impl.cpp		\
	      python/native/mesos_scheduler_driver_impl.hpp		\
	      python/native/module.cpp python/native/module.hpp		\
	      python/native/proxy_executor.cpp				\
	      python/native/proxy_executor.hpp				\
	      python/native/proxy_scheduler.cpp				\
	      python/native/proxy_scheduler.hpp

if HAS_PYTHON
# Used for building Python eggs.
DISTRIBUTE_EGG_SUFFIX = $(DISTRIBUTE_VERSION)$(PYTHON_EGG_PUREPY_POSTFIX).egg
DISTRIBUTE_EGG = \
  $(abs_top_builddir)/$(DISTRIBUTE)/dist/distribute-$(DISTRIBUTE_EGG_SUFFIX)

# Build a protobuf Python egg.
PROTOBUF_EGG_SUFFIX = $(PROTOBUF_VERSION)$(PYTHON_EGG_PUREPY_POSTFIX).egg
PROTOBUF_EGG = ../$(PROTOBUF)/python/dist/protobuf-$(PROTOBUF_EGG_SUFFIX)

$(PROTOBUF_EGG):
	@echo "Building protobuf Python egg ..."
	cd ../$(PROTOBUF)/python && \
          CC="$(CC)" CXX="$(CXX)" CFLAGS="$(CFLAGS)" CXXFLAGS="$(CXXFLAGS)" \
	  PYTHONPATH=$(DISTRIBUTE_EGG) $(PYTHON) setup.py build bdist_egg

CLEANFILES += $(PROTOBUF_EGG)

# This builds a Python egg against libmesos_no_3rdparty.a that is
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

MESOS_EGG = python/dist/mesos-$(PACKAGE_VERSION)$(PYTHON_EGG_POSTFIX).egg

$(MESOS_EGG): python/setup.py $(srcdir)/python/src/mesos.py		\
              $(PYTHON_PROTOS) libmesos_no_3rdparty.la			\
              $(PROTOBUF_EGG)
	@echo "Building Mesos Python egg ..."
	@if test "$(top_srcdir)" != "$(top_builddir)"; then		\
	  $(MKDIR_P) python/src;					\
	  cp -pf $(srcdir)/python/src/mesos.py python/src;		\
	fi
	@LIBS="$(LIBS)" CC="$(CC)" CXX="$(CXX)"				\
	  PYTHONPATH=$(DISTRIBUTE_EGG) $(PYTHON) python/setup.py bdist_egg

CLEANFILES += $(MESOS_EGG) python/build/temp.*/native/*.o python/build/lib.*/*

# Make sure the egg gets built via 'make all'.
all-local: $(MESOS_EGG)
endif # HAS_PYTHON

# Note that to clean up the copied Python files we have to 'chmod u+w
# python/native' because creating a distribution can eliminate write
# permissions for the owner which are conserved even after we 'cp -p'
# so we won't be able to delete the files until we update permissions.
#
# We remove mesos-*.egg here to make sure any older versions of the
# egg or versions for different architectures are removed.
clean-python:
	test "$(top_srcdir)" = "$(top_builddir)" ||			\
	  (chmod -R u+w python/native;					\
	   rm -rf python/src/mesos.py python/native)
	-rm -rf python/src/mesos.egg-info python/build
	-rm -f python/dist/mesos-*.egg

PHONY_TARGETS += clean-python


# Test (make check) binaries.
check_PROGRAMS += test-framework
test_framework_SOURCES = examples/test_framework.cpp
test_framework_CPPFLAGS = $(MESOS_CPPFLAGS)
test_framework_LDADD = libmesos.la

check_PROGRAMS += test-executor
test_executor_SOURCES = examples/test_executor.cpp
test_executor_CPPFLAGS = $(MESOS_CPPFLAGS)
test_executor_LDADD = libmesos.la

check_PROGRAMS += long-lived-framework
long_lived_framework_SOURCES = examples/long_lived_framework.cpp
long_lived_framework_CPPFLAGS = $(MESOS_CPPFLAGS)
long_lived_framework_LDADD = libmesos.la

check_PROGRAMS += long-lived-executor
long_lived_executor_SOURCES = examples/long_lived_executor.cpp
long_lived_executor_CPPFLAGS = $(MESOS_CPPFLAGS)
long_lived_executor_LDADD = libmesos.la

check_PROGRAMS += no-executor-framework
no_executor_framework_SOURCES = examples/no_executor_framework.cpp
no_executor_framework_CPPFLAGS = $(MESOS_CPPFLAGS)
no_executor_framework_LDADD = libmesos.la

check_PROGRAMS += balloon-framework
balloon_framework_SOURCES = examples/balloon_framework.cpp
balloon_framework_CPPFLAGS = $(MESOS_CPPFLAGS)
balloon_framework_LDADD = libmesos.la

check_PROGRAMS += balloon-executor
balloon_executor_SOURCES = examples/balloon_executor.cpp
balloon_executor_CPPFLAGS = $(MESOS_CPPFLAGS)
balloon_executor_LDADD = libmesos.la

check_PROGRAMS += mesos-tests

mesos_tests_SOURCES =				\
  tests/allocator_tests.cpp			\
  tests/attributes_tests.cpp			\
  tests/authentication_tests.cpp		\
  tests/containerizer.cpp			\
  tests/environment.cpp				\
  tests/examples_tests.cpp			\
  tests/exception_tests.cpp			\
  tests/fault_tolerance_tests.cpp		\
  tests/files_tests.cpp				\
  tests/flags.cpp				\
  tests/gc_tests.cpp				\
  tests/isolator_tests.cpp			\
  tests/log_tests.cpp				\
  tests/logging_tests.cpp			\
  tests/main.cpp				\
  tests/master_contender_detector_tests.cpp	\
  tests/master_tests.cpp			\
  tests/mesos.cpp				\
  tests/monitor_tests.cpp			\
  tests/paths_tests.cpp				\
  tests/protobuf_io_tests.cpp			\
  tests/registrar_tests.cpp			\
  tests/resource_offers_tests.cpp		\
  tests/resources_tests.cpp			\
  tests/sasl_tests.cpp				\
  tests/script.cpp				\
  tests/slave_recovery_tests.cpp		\
  tests/sorter_tests.cpp			\
  tests/state_tests.cpp				\
  tests/status_update_manager_tests.cpp		\
  tests/utils.cpp				\
  tests/zookeeper_url_tests.cpp

mesos_tests_CPPFLAGS = $(MESOS_CPPFLAGS)
mesos_tests_CPPFLAGS += -DSOURCE_DIR=\"$(abs_top_srcdir)\"
mesos_tests_CPPFLAGS += -DBUILD_DIR=\"$(abs_top_builddir)\"
mesos_tests_CPPFLAGS += -I../$(GTEST)/include
mesos_tests_CPPFLAGS += -I../$(GMOCK)/include

mesos_tests_LDADD = ../$(LIBPROCESS)/3rdparty/libgmock.la libmesos.la

mesos_tests_DEPENDENCIES = # Initialized to allow += below.

if OS_LINUX
  mesos_tests_SOURCES += tests/cgroups_isolator_tests.cpp
  mesos_tests_SOURCES += tests/cgroups_tests.cpp
  mesos_tests_SOURCES += tests/fs_tests.cpp
endif

if HAS_JAVA
  mesos_tests_SOURCES += tests/zookeeper.cpp				\
                         tests/zookeeper_test_server.cpp		\
                         tests/zookeeper_tests.cpp			\
                         tests/group_tests.cpp				\
                         tests/allocator_zookeeper_tests.cpp
  mesos_tests_CPPFLAGS += $(JAVA_CPPFLAGS)
  mesos_tests_CPPFLAGS += -DZOOKEEPER_VERSION=\"$(ZOOKEEPER_VERSION)\"
  mesos_tests_LDFLAGS = $(JAVA_LDFLAGS) $(AM_LDFLAGS)
  mesos_tests_DEPENDENCIES += $(EXAMPLES_JAR)

  EXAMPLESSCRIPTSJAVA = examples/java/test-framework			\
			examples/java/test-executor			\
			examples/java/test-exception-framework		\
			examples/java/test-multiple-executors-framework

  check_SCRIPTS += $(EXAMPLESCRIPTSJAVA)
  mesos_tests_DEPENDENCIES += $(EXAMPLESCRIPTSJAVA)
endif

if HAS_PYTHON
  mesos_tests_DEPENDENCIES += $(MESOS_EGG)

  EXAMPLESCRIPTSPYTHON = examples/python/test_framework.py		\
			 examples/python/test-framework			\
			 examples/python/test_executor.py		\
			 examples/python/test-executor

  check_SCRIPTS += $(EXAMPLESCRIPTSPYTHON)
  mesos_tests_DEPENDENCIES += $(EXAMPLESCRIPTSPYTHON)
endif

EXTRA_DIST += examples/python/test_framework.py				\
	      examples/python/test_executor.py


dist_check_SCRIPTS +=							\
  tests/balloon_framework_test.sh					\
  tests/test_framework_test.sh						\
  tests/no_executor_framework_test.sh					\
  tests/java_exception_test.sh						\
  tests/java_framework_test.sh						\
  tests/python_framework_test.sh

# We use a check-local target for now to avoid the parallel test
# runner that ships with newer versions of autotools.
# See the following discussion for the workaround:
# http://lists.gnu.org/archive/html/automake/2013-01/msg00051.html
check-local: mesos-tests
	./mesos-tests

clean-local: clean-java clean-python

.PHONY: $(PHONY_TARGETS)
