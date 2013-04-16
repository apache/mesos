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

EXTRA_DIST = TUTORIAL.sh hadoop-gridmix.patch				\
  hadoop-0.20.2-cdh3u3_hadoop-env.sh.patch				\
  hadoop-0.20.2-cdh3u3_mesos.patch					\
  hadoop-2.0.0-mr1-cdh4.1.2_hadoop-env.sh.patch				\
  hadoop-2.0.0-mr1-cdh4.1.2_mesos.patch					\
  hadoop-2.0.0-mr1-cdh4.2.0_hadoop-env.sh.patch				\
  hadoop-2.0.0-mr1-cdh4.2.0_mesos.patch					\
  hadoop-7698-1.patch							\
  hadoop-0.20.205.0_hadoop-env.sh.patch hadoop-0.20.205.0_mesos.patch	\
  HadoopPipes.cc.patch                                                  \
  mapred-site.xml.patch mesos-executor mesos/build.xml			\
  mesos/ivy/libraries.properties mesos/ivy.xml				\
  mesos/src/java/org/apache/hadoop/mapred/MesosExecutor.java		\
  mesos/src/java/org/apache/hadoop/mapred/MesosScheduler.java

# Defines some targets to run the Hadoop tutorial using a specified
# distribution. At some point we might want to do this automagically
# (i.e., as part of 'make check'). Note that we set the environment
# variable TMOUT to 1 so that each prompt in the tutorial will return
# after 1 second so no interaction from the user is required.
hadoop-0.20.205.0:
	if test "$(top_srcdir)" != "$(top_builddir)"; then \
          cp -p $(srcdir)/TUTORIAL.sh .; \
          cp -p $(srcdir)/hadoop-gridmix.patch .; \
          cp -p $(srcdir)/hadoop-7698-1.patch .; \
          cp -p $(srcdir)/hadoop-0.20.205.0_hadoop-env.sh.patch .; \
          cp -p $(srcdir)/hadoop-0.20.205.0_mesos.patch .; \
          cp -p $(srcdir)/mapred-site.xml.patch .; \
          cp -rp $(srcdir)/mesos .; \
          cp -p $(srcdir)/mesos-executor .; \
        fi
	rm -rf hadoop-0.20.205.0
	@TMOUT=1 JAVA_HOME=$(JAVA_HOME) ./TUTORIAL.sh

hadoop-0.20.2-cdh3u3:
	if test "$(top_srcdir)" != "$(top_builddir)"; then \
          cp -p $(srcdir)/TUTORIAL.sh .; \
          cp -p $(srcdir)/hadoop-gridmix.patch .; \
          cp -p $(srcdir)/hadoop-0.20.2-cdh3u3_hadoop-env.sh.patch .; \
          cp -p $(srcdir)/hadoop-0.20.2-cdh3u3_mesos.patch .; \
          cp -p $(srcdir)/mapred-site.xml.patch .; \
          cp -rp $(srcdir)/mesos .; \
          cp -p $(srcdir)/mesos-executor .; \
        fi
	rm -rf hadoop-0.20.2-cdh3u3
	@TMOUT=1 JAVA_HOME=$(JAVA_HOME) ./TUTORIAL.sh 0.20.2-cdh3u3

hadoop-2.0.0-mr1-cdh4.1.2:
	if test "$(top_srcdir)" != "$(top_builddir)"; then \
          cp -p $(srcdir)/TUTORIAL.sh .; \
          cp -p $(srcdir)/hadoop-gridmix.patch .; \
          cp -p $(srcdir)/hadoop-2.0.0-mr1-cdh4.1.2_hadoop-env.sh.patch .; \
          cp -p $(srcdir)/hadoop-2.0.0-mr1-cdh4.1.2_mesos.patch .; \
          cp -p $(srcdir)/mapred-site.xml.patch .; \
          cp -p $(srcdir)/HadoopPipes.cc.patch .; \
          cp -rp $(srcdir)/mesos .; \
          cp -p $(srcdir)/mesos-executor .; \
        fi
	rm -rf hadoop-2.0.0-mr1-cdh4.1.2
	@TMOUT=1 JAVA_HOME=$(JAVA_HOME) ./TUTORIAL.sh 2.0.0-mr1-cdh4.1.2

hadoop-2.0.0-mr1-cdh4.2.0:
	if test "$(top_srcdir)" != "$(top_builddir)"; then \
          cp -p $(srcdir)/TUTORIAL.sh .; \
          cp -p $(srcdir)/hadoop-gridmix.patch .; \
          cp -p $(srcdir)/hadoop-2.0.0-mr1-cdh4.2.0_hadoop-env.sh.patch .; \
          cp -p $(srcdir)/hadoop-2.0.0-mr1-cdh4.2.0_mesos.patch .; \
          cp -p $(srcdir)/mapred-site.xml.patch .; \
          cp -p $(srcdir)/HadoopPipes.cc.patch .; \
          cp -rp $(srcdir)/mesos .; \
          cp -p $(srcdir)/mesos-executor .; \
        fi
	rm -rf hadoop-2.0.0-mr1-cdh4.2.0
	@TMOUT=1 JAVA_HOME=$(JAVA_HOME) ./TUTORIAL.sh 2.0.0-mr1-cdh4.2.0


clean-local:
	if test "$(top_srcdir)" != "$(top_builddir)"; then \
          rm -f TUTORIAL.sh; \
          rm -f hadoop-gridmix.patch; \
          rm -f hadoop-7698-1.patch; \
          rm -f hadoop-0.20.2-cdh3u3_hadoop-env.sh.patch; \
          rm -f hadoop-0.20.2-cdh3u3_mesos.patch; \
          rm -f hadoop-2.0.0-mr1-cdh4.1.2_hadoop-env.sh.patch; \
          rm -f hadoop-2.0.0-mr1-cdh4.1.2_mesos.patch; \
          rm -f hadoop-2.0.0-mr1-cdh4.2.0_hadoop-env.sh.patch; \
          rm -f hadoop-2.0.0-mr1-cdh4.2.0_mesos.patch; \
          rm -f hadoop-0.20.205.0_hadoop-env.sh.patch; \
          rm -f hadoop-0.20.205.0_mesos.patch; \
          rm -f mapred-site.xml.patch; \
          rm -f mesos-executor; \
          rm -rf mesos; \
        fi
	rm -rf hadoop-0.20.2-cdh3u3
	rm -f hadoop-0.20.2-cdh3u3.tar.gz
	rm -rf hadoop-2.0.0-mr1-cdh4.1.2
	rm -f mr1-2.0.0-mr1-cdh4.1.2.tar.gz
	rm -rf hadoop-2.0.0-mr1-cdh4.2.0
	rm -f mr1-2.0.0-mr1-cdh4.2.0.tar.gz
	rm -rf hadoop-0.20.205.0
	rm -f hadoop-0.20.205.0.tar.gz


.PHONY: hadoop-0.20.205.0 hadoop-0.20.2-cdh3u3 hadoop-2.0.0-mr1-cdh4.1.2 hadoop-2.0.0-mr1-cdh4.2.0
