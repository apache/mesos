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

SUBDIRS = . third_party src

EXTRA_DIST = bootstrap LICENSE NOTICE README support/colors.sh

PHONY_TARGETS = # Initialized to empty.


# Since we generate several files in src/ with config.status, make
# sure they're regenerated before we recurse into the src directory.
all-recursive: src/python/setup.py src/java/mesos.pom


# We include support for Hadoop on Mesos in the distribution.
EXTRA_DIST += hadoop/TUTORIAL.sh hadoop/hadoop-0.20.205.0.patch	\
	hadoop/hadoop-0.20.205.0.tar.gz				\
	hadoop/hadoop-0.20.205.0_conf_hadoop-env.sh.patch	\
	hadoop/hadoop-0.20.205.0_conf_mapred-site.xml.patch

# Defines a target which runs the Hadoop tutorial to make sure
# everything works. At some point we might want to do this
# automagically (i.e., as part of 'make check'). Note that we set the
# environment variable TMOUT to 1 so that each prompt in the tutorial
# will return after 1 second so no interaction from the user is
# required.
hadoop: all
	@if test "$(top_srcdir)" != "$(top_builddir)"; then \
	  rm -rf hadoop; \
	  cp -rpf $(srcdir)/hadoop hadoop; \
	fi
	@TMOUT=1 JAVA_HOME=$(JAVA_HOME) ./hadoop/TUTORIAL.sh

# TODO(benh): Cleanup (i.e., via 'clean-local') for hadoop target.

PHONY_TARGETS += hadoop


if HAS_JAVA
maven-install:
	@cd src && $(MAKE) $(AM_MAKEFLAGS) maven-install

PHONY_TARGETS += maven-install
endif


.PHONY: $(PHONY_TARGETS)
