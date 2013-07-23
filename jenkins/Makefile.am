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

EXTRA_DIST = pom.xml										\
	src/main/java/org/jenkinsci/plugins/mesos/JenkinsScheduler.java				\
	src/main/java/org/jenkinsci/plugins/mesos/Mesos.java					\
	src/main/java/org/jenkinsci/plugins/mesos/MesosCloud.java				\
	src/main/java/org/jenkinsci/plugins/mesos/MesosComputer.java				\
	src/main/java/org/jenkinsci/plugins/mesos/MesosComputerLauncher.java			\
	src/main/java/org/jenkinsci/plugins/mesos/MesosRetentionStrategy.java			\
	src/main/java/org/jenkinsci/plugins/mesos/MesosSlave.java				\
	src/main/resources/index.jelly								\
	src/main/resources/org/jenkinsci/plugins/mesos/MesosCloud/config.jelly			\
	src/main/resources/org/jenkinsci/plugins/mesos/MesosCloud/help-master.html		\
	src/main/resources/org/jenkinsci/plugins/mesos/MesosSlave/configure-entries.jelly


if OS_LINUX
  MESOS_LIBRARY = libmesos.so
else
  MESOS_LIBRARY = libmesos.dylib
endif
TARGET_LIBS_DIR = src/main/webapp


$(MESOS_LIBRARY):
	@cd $(abs_top_builddir)/src && $(MAKE) $(AM_MAKEFLAGS);


MESOS_JAR = mesos-$(PACKAGE_VERSION).jar


$(MESOS_JAR):
	@cd $(abs_top_builddir)/src && $(MAKE) $(AM_MAKEFLAGS) maven-install;


# Defines a target to build the Jenkins plugin for Mesos.
# This will create ./target/mesos.hpi that you can deploy to Jenkins.
jenkins: $(MESOS_LIBRARY) $(MESOS_JAR)
	if test "$(top_srcdir)" != "$(top_builddir)"; then			\
          cp -rp $(srcdir)/src .;						\
        fi
	mkdir -p $(TARGET_LIBS_DIR) &&						\
	cp $(abs_top_builddir)/src/.libs/$(MESOS_LIBRARY) $(TARGET_LIBS_DIR) && \
	mvn package


clean-local:
	if test "$(top_srcdir)" != "$(top_builddir)"; then		\
          rm -rf src;							\
        fi
	rm -rf target work;


.PHONY: jenkins
