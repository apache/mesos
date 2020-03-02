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

ACLOCAL_AMFLAGS = -I m4

AUTOMAKE_OPTIONS = foreign

SUBDIRS = . 3rdparty src

EXTRA_DIST =

PHONY_TARGETS =

pkgconfigdir = $(libdir)/pkgconfig
pkgconfig_DATA = mesos.pc

# Directories to optionally install test binaries and libraries.
testlibexecdir = $(pkglibexecdir)/tests

# Since we generate several files in src/ with config.status, make
# sure they're regenerated before we recurse into the src directory.
all-recursive: src/python/setup.py			\
	src/python/cli/setup.py				\
	src/python/interface/setup.py			\
	src/python/native/setup.py			\
	src/java/mesos.pom

# Standard stuff.
EXTRA_DIST += bootstrap LICENSE NOTICE README.md	\
	support/atexit.sh support/colors.sh		\
	support/mesos-gtest-runner.py

# On install, pip fetches and installs all missing dependencies. When running
# distcheck, this behavior ends up installing files that we are not able to
# uninstall (protobuf for example). Using this flag during the distcheck run
# forces us to verify that we can install/uninstall the mesos specific
# libraries and only those libraries.
AM_DISTCHECK_CONFIGURE_FLAGS = --disable-python-dependency-install

# In some distributions, rpmbuild(8) will implicitly disable dependency
# tracking. Make sure we disable it in during distcheck to ensure we do not
# accidentally depend on the side-effects of automake dependency tracking.
AM_DISTCHECK_CONFIGURE_FLAGS += --disable-dependency-tracking


if HAS_JAVA
maven-install:
	@cd src && $(MAKE) $(AM_MAKEFLAGS) maven-install

PHONY_TARGETS += maven-install
endif


# Run benchmark tests.
# Currently benchmark tests exist only under src/ so the top level
# 'bench' rule calls the Makefile under src/ directly to run these
# tests, after it builds the entire program (i.e. 'all') and the test
# prerequisites (libgmock.la).
# TODO(xujyan): The use of variable 'MESOS_BENCHMARKS' on the target
# 'check' is likely going to be replaced by a long-term solution for
# benchmark testing, for which we should consider separating
# benchmark tests from the 'mesos-tests' binary (i.e. the
# 'correctness' tests).
# TODO(xujyan): Consider using `AM_EXTRA_RECURSIVE_TARGETS` macro
# (requires Automake >= 1.13) to define 'bench' as a recursive target
# when we have benchmark tests in multiple subdirs.
bench: all
	@cd 3rdparty && $(MAKE) $(AM_MAKEFLAGS) libgmock.la
	@cd src && MESOS_BENCHMARK=1 $(MAKE) $(AM_MAKEFLAGS) check

PHONY_TARGETS += bench

tests: all
	@cd 3rdparty && $(MAKE) $(AM_MAKEFLAGS) tests
	@cd src && $(MAKE) $(AM_MAKEFLAGS) tests

# Using LT_OUTPUT in configure.ac creates config.lt that doesn't get
# cleaned up by distclean-libtool. See this bug patch (which doesn't
# appear to be in all versions of libtool.m4):
# http://lists.gnu.org/archive/html/automake-commit/2008-11/msg00015.html.
distclean-local:
	-rm -f config.lt
	-rm -f ./3rdparty/report.xml
	-rm -f ./3rdparty/libprocess/report.xml
	-rm -f ./3rdparty/stout/report.xml
	-rm -f ./src/report.xml

if INSTALL_TESTS
dist_testlibexec_SCRIPTS = 				\
  support/atexit.sh 					\
  support/colors.sh
endif

.PHONY: $(PHONY_TARGETS)
