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
	src/main/java/org/jenkinsci/plugins/mesos/TaskTemplate.java				\
	src/main/resources/index.jelly								\
	src/main/resources/org/jenkinsci/plugins/mesos/MesosCloud/computerSet.jelly		\
	src/main/resources/org/jenkinsci/plugins/mesos/MesosCloud/config.jelly			\
	src/main/resources/org/jenkinsci/plugins/mesos/MesosCloud/help-master.html		\
	src/main/resources/org/jenkinsci/plugins/mesos/MesosSlave/configure-entries.jelly	\
	src/main/resources/org/jenkinsci/plugins/mesos/TaskTemplate/config.jelly


# Defines a target to build the Jenkins plugin.
# This will create ./target/mesos.hpi that you can deploy to Jenkins.
jenkins:
	if test "$(top_srcdir)" != "$(top_builddir)"; then	\
          cp -p $(srcdir)/pom.xml .;				\
          cp -rp $(srcdir)/src .;				\
        fi
	mvn install


clean-local:
	if test "$(top_srcdir)" != "$(top_builddir)"; then	\
          rm -f pom.xml;					\
          rm -rf src target work;				\
        fi


.PHONY: jenkins
