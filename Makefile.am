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

SUBDIRS = . 3rdparty src ec2

EXTRA_DIST =

PHONY_TARGETS =


# Since we generate several files in src/ with config.status, make
# sure they're regenerated before we recurse into the src directory.
all-recursive: src/python/setup.py src/java/mesos.pom


# Standard stuff.
EXTRA_DIST += bootstrap LICENSE NOTICE README.md	\
  support/atexit.sh support/colors.sh


# MPI framework.
EXTRA_DIST += mpi/README mpi/mpiexec-mesos.in mpi/mpiexec-mesos.py


if HAS_JAVA
maven-install:
	@cd src && $(MAKE) $(AM_MAKEFLAGS) maven-install

PHONY_TARGETS += maven-install
endif


# Using LT_OUTPUT in configure.ac creates config.lt that doesn't get
# cleaned up by distclean-libtool. See this bug patch (which doesn't
# appear to be in all versions of libtool.m4):
# http://lists.gnu.org/archive/html/automake-commit/2008-11/msg00015.html.
distclean-local:
	-rm -f config.lt


.PHONY: $(PHONY_TARGETS)
