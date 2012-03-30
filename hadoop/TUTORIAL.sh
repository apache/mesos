#!/bin/bash

# The Hadoop version for this tutorial.
hadoop=hadoop-0.20.205.0

# The potentially running JobTracker, that we need to kill.
jobtracker_pid=

# Trap Ctrl-C (signal 2).
trap 'test ! -z ${jobtracker_pid} && kill ${jobtracker_pid}; echo; exit 1' 2


# Utility function for failing the tutorial with a helpful message.
function fail() {
    cat <<__EOF__

${RED}Oh no! We failed to run '${1}'. If you need help try emailing:

  mesos-dev@incubator.apache.org

(Remember to include as much debug information as possible.)${NORMAL}

__EOF__
    exit 1
}


# Make sure we start out in the right directory.
cd `dirname ${0}`

# Include wonderful colors for our tutorial!
test -f ../support/colors.sh && . ../support/colors.sh

# Make sure we have all the necessary files we need.
files="TUTORIAL.sh \
  hadoop-0.20.205.0.patch \
  hadoop-0.20.205.0.tar.gz \
  hadoop-0.20.205.0_conf_hadoop-env.sh.patch \
  hadoop-0.20.205.0_conf_mapred-site.xml.patch"

for file in `echo ${files}`; do
    if test ! -f ${file}; then
        cat <<__EOF__

${RED}We seem to be missing ${file} from the directory containing this
tutorial and we can't continue without that file. If you haven't made
any modifications to this directory, please report this to:

  mesos-dev@incubator.apache.org

(Remember to include as much debug information as possible.)${NORMAL}

__EOF__
        exit 1
    fi
done


# Start the tutorial!
cat <<__EOF__

Welcome to the tutorial on running Apache Hadoop on top of Mesos!
During this ${BRIGHT}interactive${NORMAL} guide we'll ask some yes/no
questions and you should enter your answer via 'Y' or 'y' for yes and
'N' or 'n' for no.

Let's begin!

__EOF__


# Check for JAVA_HOME.
if test -z ${JAVA_HOME}; then
    cat <<__EOF__

${RED}You probably need to set JAVA_HOME in order to run this tutorial!${NORMAL}

__EOF__
    read -e -p "${BRIGHT}Hit enter to continue.${NORMAL} "
    echo
fi


# Extract the archive.
if test ! -d ${hadoop}; then
    cat <<__EOF__

We've included the 0.20.205.0 version of Hadoop in this directory
(${hadoop}.tar.gz). Start by extracting it:

  $ tar zxvf ${hadoop}.tar.gz

__EOF__
    read -e -p "${BRIGHT}Hit enter to continue.${NORMAL} "
    echo
    tar zxvf ${hadoop}.tar.gz || fail "tar zxvf ${hadoop}.tar.gz"
else
    cat <<__EOF__

${RED}It looks like you've already extracted ${hadoop}.tar.gz, so
we'll skip that step.${NORMAL}

__EOF__
fi


# Apply the patch.
cat <<__EOF__

To run Hadoop on Mesos we need to apply a rather minor patch. The
patch makes a small number of modifications in Hadoop, and adds some
new code at src/contrib/mesos. (Note that the changes to Hadoop have
been committed in revisions r1033804 and r987589 so at some point we
won't need to apply any patch at all.) We'll apply the patch with:

  $ patch -p2 <${hadoop}.patch

__EOF__

if test -d ${hadoop}/src/contrib/mesos; then
    cat <<__EOF__

${RED}It looks like you've already applied the patch, so we'll skip
applying it now.${NORMAL}

__EOF__
else
    read -e -p "${BRIGHT}Hit enter to continue.${NORMAL} "
    echo
    patch -p2 <${hadoop}.patch || fail "patch -p2 <${hadoop}.patch"
fi

if test ! -x ${hadoop}/bin/mesos-executor; then
    cat <<__EOF__

We'll also need to make one of the new files executable via:

  $ chmod +x ${hadoop}/bin/mesos-executor

__EOF__
    read -e -p "${BRIGHT}Hit enter to continue.${NORMAL} "
    echo
    chmod +x ${hadoop}/bin/mesos-executor || \
        fail "chmod +x ${hadoop}/bin/mesos-executor"
fi


# Change into Hadoop directory.
cat <<__EOF__

Okay, now let's change into the directory in order to build Hadoop.

  $ cd ${hadoop}

__EOF__

read -e -p "${BRIGHT}Hit enter to continue.${NORMAL} "
echo

cd ${hadoop} || fail "cd ${hadoop}"


# Determine MESOS_BUILD_DIR.
cat <<__EOF__

Okay, now we're ready to build and then run Hadoop! There are a couple
important considerations. First, we need to locate the Mesos JAR and
native library (i.e., libmesos.so on Linux and libmesos.dylib on Mac
OS X). The Mesos JAR is used for both building and running, while the
native library is only used for running. In addition, we need to
locate the Protobuf JAR (if you don't already have one one your
default classpath).

This tutorial assumes you've built Mesos already. We'll use the
environment variable MESOS_BUILD_DIR to denote this directory.

__EOF__

read -e -p "${BRIGHT}Hit enter to continue.${NORMAL} "
echo

MESOS_BUILD_DIR=`cd ../../ && pwd`

while test ! -f `echo ${MESOS_BUILD_DIR}/src/mesos-*.jar`; do
    cat <<__EOF__

${RED}We couldn't automagically determine MESOS_BUILD_DIR. It doesn't
look like you used ${MESOS_BUILD_DIR} to build Mesos. Maybe you need
to go back and run 'make' in that directory before
continuing?${NORMAL}

__EOF__

    DEFAULT=${MESOS_BUILD_DIR}
    read -e -p "${BRIGHT}Where is the build directory?${NORMAL} [${DEFAULT}] "
    echo
    test -z ${REPLY} && REPLY=${DEFAULT}
    MESOS_BUILD_DIR=`cd ${REPLY} && pwd`
done


cat <<__EOF__

Using ${BRIGHT}${MESOS_BUILD_DIR}${NORMAL} as the build directory.

__EOF__


VERSION=`echo @PACKAGE_VERSION@ | ${MESOS_BUILD_DIR}/config.status --file=-:-`


# Build with ant.
cat <<__EOF__

Okay, let's try building Hadoop now! We need to let the build system
know where the Mesos JAR is located by using the MESOS_JAR environment
variable (i.e., MESOS_JAR=\${MESOS_BUILD_DIR}/src/mesos-${VERSION}.jar). We
can put it on the command line with 'ant like this:

  $ MESOS_JAR=${MESOS_BUILD_DIR}/src/mesos-${VERSION}.jar ant

__EOF__

read -e -p "${BRIGHT}Hit enter to continue.${NORMAL} "
echo

MESOS_JAR=${MESOS_BUILD_DIR}/src/mesos-${VERSION}.jar ant || \
    fail "MESOS_JAR=${MESOS_BUILD_DIR}/src/mesos-${VERSION}.jar ant"


# Apply conf/mapred-site.xml patch.
cat <<__EOF__

${GREEN}Build success!${NORMAL} Now let's run something!

First we need to configure Hadoop appropriately by modifying
conf/mapred-site.xml (as is always required when running Hadoop). In
order to run Hadoop on Mesos we need to set at least these three
properties:

  mapred.job.tracker

  mapred.jobtracker.taskScheduler

  mapred.mesos.master

The 'mapred.job.tracker' property should be set to the host:port where
you want to launch the JobTracker (e.g., localhost:54321).

The 'mapred.jobtracker.taskScheduler' property must be set to
'org.apache.hadoop.mapred.MesosScheduler'.

If you've alredy got a Mesos master running you can use that for
'mapred.mesos.master', but for this tutorial well just use 'local' in
order to bring up a Mesos "cluster" within the process. To connect to
a remote master simply use the Mesos URL used to connect the slave to
the master (e.g., mesos://master@localhost:5050).

We've got a prepared patch for conf/mapred-site.xml that makes the
changes necessary to get everything running. We can apply that patch
like so:

  $ patch -p3 <../${hadoop}_conf_mapred-site.xml.patch

__EOF__

read -e -p "${BRIGHT}Hit enter to continue.${NORMAL} "
echo

patch --dry-run --silent --force -p3 \
    <../${hadoop}_conf_mapred-site.xml.patch 1>/dev/null 2>&1

if test ${?} == "1"; then
    cat <<__EOF__

${RED}It looks like conf/mapred-site.xml has been modified. You'll
need to copy that to something else and restore the file to it's
original contents before we'll be able to apply this patch.${NORMAL}

__EOF__
    DEFAULT="N"
else
    DEFAULT="Y"
fi

read -e -p "${BRIGHT}Patch conf/mapred-site.xml?${NORMAL} [${DEFAULT}] "
echo
test -z ${REPLY} && REPLY=${DEFAULT}
if test ${REPLY} == "Y" -o ${REPLY} == "y"; then
    patch -p3 <../${hadoop}_conf_mapred-site.xml.patch || \
        fail "patch -p3 <../${hadoop}_conf_mapred-site.xml.patch"
fi

# Apply conf/hadoop-env.sh patch.
cat <<__EOF__

Now in order to actually run Hadoop we need to set up our environment
appropriately for Hadoop. We can do this in conf/hadoop-env.sh This
includes:

  (1) Setting JAVA_HOME (unnecessary if JAVA_HOME is set in your environment).
  (2) Adding the Mesos contrib class files to HADOOP_CLASSPATH.
  (3) Adding mesos-${VERSION}.jar to the HADOOP_CLASSPATH.
  (4) Adding protobuf-2.4.1.jar to the HADOOP_CLASSPATH.
  (5) Setting MESOS_NATIVE_LIBRARY to point to the native library.

We've got a prepared patch for conf/hadoop-env.sh that makes the
necessary changes. We can apply that patch like so:

  $ patch -p3 <../${hadoop}_conf_hadoop-env.sh.patch

(Note that this patch assumes MESOS_BUILD_DIR is '../..' and you'll
need to specify that on the command line when you try and run the
JobTracker if that's not the case ... don't worry, we'll remind you
again later.)

__EOF__

read -e -p "${BRIGHT}Hit enter to continue.${NORMAL} "
echo

patch --dry-run --silent --force -p3 \
    <../${hadoop}_conf_hadoop-env.sh.patch 1>/dev/null 2>&1

if test ${?} == "1"; then
    cat <<__EOF__

${RED}It looks like conf/hadoop-env.sh has been modified. You'll need
to copy that to something else and restore the file to it's original
contents before we'll be able to apply this patch.${NORMAL}

__EOF__
    DEFAULT="N"
else
    DEFAULT="Y"
fi

read -e -p "${BRIGHT}Patch conf/hadoop-env.sh?${NORMAL} [${DEFAULT}] "
echo
test -z ${REPLY} && REPLY=${DEFAULT}
if test ${REPLY} == "Y" -o ${REPLY} == "y"; then
    patch -p3 <../${hadoop}_conf_hadoop-env.sh.patch || \
        fail "patch -p3 <../${hadoop}_conf_hadoop-env.sh.patch"
fi


# Start JobTracker.
cat <<__EOF__

Let's go ahead and try and start the JobTracker via:

  $ ./bin/hadoop jobtracker

Note that if you applied our conf/hadoop-env.sh patch we assume that
MESOS_BUILD_DIR is located at '../..'. If this isn't the case (i.e.,
you specified a different build directory than the default during this
tutorial) than you'll need to set that variable either directly in
conf/hadoop-env.sh or on the command line via:

  $ MESOS_BUILD_DIR=/path/to/mesos/build ./bin/hadoop jobtracker

__EOF__

read -e -p "${BRIGHT}Hit enter to continue.${NORMAL} "
echo

MESOS_BUILD_DIR=${MESOS_BUILD_DIR} ./bin/hadoop jobtracker 1>/dev/null 2>&1 &

jobtracker_pid=${!}

cat <<__EOF__

JobTracker started at ${BRIGHT}${jobtracker_pid}${NORMAL}.

__EOF__

echo -n "Waiting 5 seconds for it to start."

for i in 1 2 3 4 5; do sleep 1 && echo -n " ."; done


# Now let's run an example.
cat <<__EOF__

Alright, now let's run the "wordcount" example via:

  $ ./bin/hadoop jar hadoop-examples-0.20.205.0.jar wordcount \
  src/contrib/mesos/src/java/org/apache/hadoop/mapred out

__EOF__

read -e -p "${BRIGHT}Hit enter to continue.${NORMAL} "
echo

rm -rf out # TODO(benh): Ask about removing this first.

./bin/hadoop jar hadoop-examples-0.20.205.0.jar wordcount \
    src/contrib/mesos/src/java/org/apache/hadoop/mapred out


if test ${?} == "0"; then
    cat <<__EOF__

${GREEN}Success!${NORMAL} We'll kill the JobTracker and exit.

We hope you found this was helpful!

__EOF__
    kill ${jobtracker_pid}
else
    cat <<__EOF__

${RED}Oh no, it failed! Try running the JobTracker and wordcount
example manually ... it might be an issue with your environment that
this tutorial didn't cover (if you find this to be the case, please
create a JIRA for us and/or send us a code review).${NORMAL}

__EOF__
    kill ${jobtracker_pid}
    exit 1
fi
