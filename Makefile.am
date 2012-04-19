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

EXTRA_DIST =

PHONY_TARGETS = # Initialized to empty.


# Since we generate several files in src/ with config.status, make
# sure they're regenerated before we recurse into the src directory.
all-recursive: src/python/setup.py src/java/mesos.pom


# Standard stuff.
EXTRA_DIST += bootstrap DISCLAIMER LICENSE NOTICE README support/colors.sh


# Extra configure scripts.
EXTRA_DIST += configure.amazon-linux-64 configure.centos-5.4-64		\
  configure.macosx configure.ubuntu-lucid-64 configure.ubuntu-natty-64


# We include support for Hadoop on Mesos in the distribution.
EXTRA_DIST += hadoop/TUTORIAL.sh hadoop/hadoop-0.20.205.0.patch	\
  hadoop/hadoop-0.20.205.0.tar.gz				\
  hadoop/hadoop-0.20.205.0_conf_hadoop-env.sh.patch		\
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


# EC2 support.
EXTRA_DIST += ec2/mesos-ec2 ec2/mesos_ec2.py

EXTRA_DIST +=									\
  ec2/deploy.amazon64/root/ephemeral-hdfs/conf/core-site.xml			\
  ec2/deploy.amazon64/root/ephemeral-hdfs/conf/hadoop-env.sh			\
  ec2/deploy.amazon64/root/ephemeral-hdfs/conf/hdfs-site.xml			\
  ec2/deploy.amazon64/root/ephemeral-hdfs/conf/mapred-site.xml			\
  ec2/deploy.amazon64/root/ephemeral-hdfs/conf/masters				\
  ec2/deploy.amazon64/root/ephemeral-hdfs/conf/slaves				\
  ec2/deploy.amazon64/root/mesos-ec2/cluster-url				\
  ec2/deploy.amazon64/root/mesos-ec2/copy-dir					\
  ec2/deploy.amazon64/root/mesos-ec2/create-swap				\
  ec2/deploy.amazon64/root/mesos-ec2/hadoop-framework-conf/core-site.xml	\
  ec2/deploy.amazon64/root/mesos-ec2/hadoop-framework-conf/hadoop-env.sh	\
  ec2/deploy.amazon64/root/mesos-ec2/hadoop-framework-conf/mapred-site.xml	\
  ec2/deploy.amazon64/root/mesos-ec2/haproxy+apache/haproxy.config.template	\
  ec2/deploy.amazon64/root/mesos-ec2/hypertable/Capfile				\
  ec2/deploy.amazon64/root/mesos-ec2/hypertable/hypertable.cfg			\
  ec2/deploy.amazon64/root/mesos-ec2/masters					\
  ec2/deploy.amazon64/root/mesos-ec2/mesos-daemon				\
  ec2/deploy.amazon64/root/mesos-ec2/redeploy-mesos				\
  ec2/deploy.amazon64/root/mesos-ec2/setup					\
  ec2/deploy.amazon64/root/mesos-ec2/setup-slave				\
  ec2/deploy.amazon64/root/mesos-ec2/setup-torque				\
  ec2/deploy.amazon64/root/mesos-ec2/slaves					\
  ec2/deploy.amazon64/root/mesos-ec2/ssh-no-keychecking				\
  ec2/deploy.amazon64/root/mesos-ec2/start-hypertable				\
  ec2/deploy.amazon64/root/mesos-ec2/start-mesos				\
  ec2/deploy.amazon64/root/mesos-ec2/stop-hypertable				\
  ec2/deploy.amazon64/root/mesos-ec2/stop-mesos					\
  ec2/deploy.amazon64/root/mesos-ec2/zoo					\
  ec2/deploy.amazon64/root/persistent-hdfs/conf/core-site.xml			\
  ec2/deploy.amazon64/root/persistent-hdfs/conf/hadoop-env.sh			\
  ec2/deploy.amazon64/root/persistent-hdfs/conf/hdfs-site.xml			\
  ec2/deploy.amazon64/root/persistent-hdfs/conf/mapred-site.xml			\
  ec2/deploy.amazon64/root/persistent-hdfs/conf/masters				\
  ec2/deploy.amazon64/root/persistent-hdfs/conf/slaves				\
  ec2/deploy.amazon64/root/spark/conf/spark-env.sh

EXTRA_DIST += ec2/deploy.generic/root/mesos-ec2/ec2-variables.sh


.PHONY: $(PHONY_TARGETS)
