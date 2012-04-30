#!/bin/bash

# Determine the Hadoop distribution to use.
if test -z "${1}"; then
    distribution="0.20.205.0"
    url="http://apache.cs.utah.edu/hadoop/common/hadoop-0.20.205.0"
elif test "${1}" = "0.20.2-cdh3u3"; then
    distribution="0.20.2-cdh3u3"
    url="http://archive.cloudera.com/cdh/3"
fi

hadoop="hadoop-${distribution}"

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

# Make sure we have all the necessary files/directories we need.
resources="TUTORIAL.sh \
  ${hadoop}.patch \
  ${hadoop}_hadoop-env.sh.patch \
  ${hadoop}_mesos.patch \
  mapred-site.xml.patch \
  mesos \
  mesos-executor"

for resource in `echo ${resources}`; do
    if test ! -e ${resource}; then
        cat <<__EOF__

${RED}We seem to be missing ${resource} from the directory containing
this tutorial and we can't continue without it. If you haven't
made any modifications to this directory, please report this to:

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


# Download Hadoop.
if test ! -e ${hadoop}.tar.gz; then
    cat <<__EOF__

We'll try and grab ${hadoop} for you now via:

  $ wget ${url}/${hadoop}.tar.gz

__EOF__
    read -e -p "${BRIGHT}Hit enter to continue.${NORMAL} "
    echo
    wget ${url}/${hadoop}.tar.gz || fail "wget ${url}/${hadoop}.tar.gz"
else
    cat <<__EOF__

${RED}It looks like you've already downloaded ${hadoop}.tar.gz, so
we'll skip that step.${NORMAL}

__EOF__
fi


# Extract the archive.
if test ! -d ${hadoop}; then
    cat <<__EOF__

Let's start by extracting ${hadoop}.tar.gz:

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


# Change into Hadoop directory.
cat <<__EOF__

Okay, now let's change into the ${hadoop} directory in order to apply
some patches, copy in the Mesos specific code, and build everything.

  $ cd ${hadoop}

__EOF__

read -e -p "${BRIGHT}Hit enter to continue.${NORMAL} "
echo

cd ${hadoop} || fail "cd ${hadoop}"


# Apply the Hadoop patch.
cat <<__EOF__

To run Hadoop on Mesos we need to apply a rather minor patch. The
patch makes a small number of modifications in Hadoop. (Note that the
changes to Hadoop have been committed in revisions r1033804 and
r987589 so at some point we won't need to apply any patch at all.)
We'll apply the patch with:

  $ patch -p1 <../${hadoop}.patch

__EOF__

# Check and see if the patch has already been applied.
grep extraData src/mapred/org/apache/hadoop/mapred/Task.java >/dev/null

if test ${?} == "0"; then
    cat <<__EOF__

${RED}It looks like you've already applied the patch, so we'll skip
applying it now.${NORMAL}

__EOF__
else
    read -e -p "${BRIGHT}Hit enter to continue.${NORMAL} "
    echo
    patch -p1 <../${hadoop}.patch || fail "patch -p1 <../${hadoop}.patch"
fi


# Copy over the Mesos contrib component (and mesos-executor) and apply
# the patch to build the contrib.
cat <<__EOF__

Now we'll copy over the Mesos contrib components. In addition, we'll
need to edit ivy/libraries.properties and src/contrib/build.xml to
hook the Mesos contrib componenet into the build. We've included a
patch to do that for you:

  $ cp -r ../mesos src/contrib
  $ cp -p ../mesos-executor bin
  $ patch -p1 <../${hadoop}_mesos.patch

__EOF__

cp -r ../mesos src/contrib || fail "cp -r ../mesos src/contrib"
cp -p ../mesos-executor bin || fail "cp -p ../mesos-executor bin"

# Check and see if the patch has already been applied.
grep mesos src/contrib/build.xml >/dev/null

if test ${?} == "0"; then
    cat <<__EOF__

${RED}It looks like you've already applied the patch, so we'll skip
applying it now.${NORMAL}

__EOF__
else
    read -e -p "${BRIGHT}Hit enter to continue.${NORMAL} "
    echo
    patch -p1 <../${hadoop}_mesos.patch || \
        fail "patch -p1 <../${hadoop}_mesos.patch"
fi


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

LIBRARY=${MESOS_BUILD_DIR}/src/.libs/libmesos.so

if test ! -f ${LIBRARY}; then
    LIBRARY=${MESOS_BUILD_DIR}/src/.libs/libmesos.dylib
fi

if test ! -f ${LIBRARY}; then
    cat <<__EOF__

${RED}We seem to be having trouble locating the native library (it's
not at ${MESOS_BUILD_DIR}/src/.libs/libmesos.so or
${MESOS_BUILD_DIR}/src/.libs/libmesos.dylib).

Have you already built Mesos? If you have, please report this to:

  mesos-dev@incubator.apache.org

(Remember to include as much debug information as possible.)${NORMAL}

__EOF__
    exit 1
fi

# Determine the "platform name" to copy the native library.
cat <<__EOF__ >PlatformName.java
public class PlatformName {
  public static void main(String[] args) {
    System.out.println(System.getProperty("os.name") + "-" +
      System.getProperty("os.arch") + "-" +
      System.getProperty("sun.arch.data.model"));
    System.exit(0);
  }
}
__EOF__

${JAVA_HOME}/bin/javac PlatformName.java || \
  fail "${JAVA_HOME}/bin/javac PlatformName.java"

PLATFORM=`${JAVA_HOME}/bin/java -Xmx32m PlatformName | sed -e "s/ /_/g"`

rm PlatformName.*


# Copy over libraries.
MESOS_JAR=`echo ${MESOS_BUILD_DIR}/src/mesos-*.jar`
PROTOBUF_JAR=`echo ${MESOS_BUILD_DIR}/protobuf-*.jar`

cat <<__EOF__

Now we'll copy over the necessary libraries we need from the build
directory.

  $ cp ${PROTOBUF_JAR} lib
  $ cp ${MESOS_JAR} lib
  $ mkdir -p lib/native/${PLATFORM}
  $ cp ${LIBRARY} lib/native/${PLATFORM}

__EOF__

cp ${PROTOBUF_JAR} lib || fail "cp ${PROTOBUF_JAR} lib"
cp ${MESOS_JAR} lib || fail "cp ${MESOS_JAR} lib"
mkdir -p lib/native/${PLATFORM} || fail "mkdir -p lib/native/${PLATFORM}"
cp ${LIBRARY} lib/native/${PLATFORM} || \
    fail "cp ${LIBRARY} lib/native/${PLATFORM}"

if test ${distribution} = "0.20.205.0"; then
    cat <<__EOF__

The Apache distribution requires that we also copy some libraries to
multiple places. :/

  $ cp ${PROTOBUF_JAR} share/hadoop/lib
  $ cp ${MESOS_JAR} share/hadoop/lib
  $ cp ${LIBRARY} lib

__EOF__

    cp ${PROTOBUF_JAR} share/hadoop/lib || \
        fail "cp ${PROTOBUF_JAR} share/hadoop/lib"
    cp ${MESOS_JAR} share/hadoop/lib || \
        fail "cp ${MESOS_JAR} share/hadoop/lib"
    cp ${LIBRARY} lib || fail "cp ${LIBRARY} lib"
fi


# Build with ant.
cat <<__EOF__

Okay, let's try building Hadoop and the Mesos contrib classes:

  $ ant

__EOF__

read -e -p "${BRIGHT}Hit enter to continue.${NORMAL} "
echo

ant || fail "ant"


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
a remote master simply use the URL used to connect the slave to the
master (e.g., localhost:5050).

We've got a prepared patch for conf/mapred-site.xml that makes the
changes necessary to get everything running. We can apply that patch
like so:

  $ patch -p1 <../mapred-site.xml.patch

__EOF__

read -e -p "${BRIGHT}Hit enter to continue.${NORMAL} "
echo

patch --dry-run --silent --force -p1 \
    <../mapred-site.xml.patch 1>/dev/null 2>&1

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
    patch -p1 <../mapred-site.xml.patch || \
        fail "patch -p1 <../mapred-site.xml.patch"
fi



# Apply conf/hadoop-env.sh patch.
cat <<__EOF__

Most users will need to set JAVA_HOME in conf/hadoop-env.sh, but we'll
also need to set MESOS_NATIVE_LIBRARY and update the HADOOP_CLASSPATH
to include the Mesos contrib classfiles. We've prepared a patch for
conf/hadoop-env.sh that makes the necessary changes. We can apply that
patch like so:

  $ patch -p1 <../${hadoop}_hadoop-env.sh.patch

__EOF__

read -e -p "${BRIGHT}Hit enter to continue.${NORMAL} "
echo

patch --dry-run --silent --force -p1 \
    <../${hadoop}_hadoop-env.sh.patch 1>/dev/null 2>&1

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
    patch -p1 <../${hadoop}_hadoop-env.sh.patch || \
        fail "patch -p1 <../${hadoop}_hadoop-env.sh.patch"
fi


# Start JobTracker.
cat <<__EOF__

Let's go ahead and try and start the JobTracker via:

  $ ./bin/hadoop jobtracker

__EOF__

read -e -p "${BRIGHT}Hit enter to continue.${NORMAL} "
echo

./bin/hadoop jobtracker 1>/dev/null 2>&1 &

jobtracker_pid=${!}

cat <<__EOF__

JobTracker started at ${BRIGHT}${jobtracker_pid}${NORMAL}.

__EOF__

echo -n "Waiting 5 seconds for it to start."

for i in 1 2 3 4 5; do sleep 1 && echo -n " ."; done


# Now let's run an example.
cat <<__EOF__

Alright, now let's run the "wordcount" example via:

  $ ./bin/hadoop jar hadoop-examples-${distribution}.jar wordcount \
  src/contrib/mesos/src/java/org/apache/hadoop/mapred out

__EOF__

read -e -p "${BRIGHT}Hit enter to continue.${NORMAL} "
echo

rm -rf out # TODO(benh): Ask about removing this first.

./bin/hadoop jar hadoop-examples-${distribution}.jar wordcount \
    src/contrib/mesos/src/java/org/apache/hadoop/mapred out


if test ${?} == "0"; then
    cat <<__EOF__

${GREEN}Success!${NORMAL} We'll kill the JobTracker and exit.

Summary:

  $ wget ${url}/${hadoop}.tar.gz
  $ tar zxvf ${hadoop}.tar.gz
  $ cd ${hadoop}
  $ patch -p1 <../${hadoop}.patch
  $ cp -r ../mesos src/contrib
  $ cp -p ../mesos-executor bin
  $ patch -p1 <../${hadoop}_mesos.patch
  $ cp ${PROTOBUF_JAR} lib
  $ cp ${MESOS_JAR} lib
  $ mkdir -p lib/native/${PLATFORM}
  $ cp ${LIBRARY} lib/native/${PLATFORM}
__EOF__

if test ${distribution} = "0.20.205.0"; then
    cat <<__EOF__
  $ cp ${PROTOBUF_JAR} share/hadoop/lib
  $ cp ${MESOS_JAR} share/hadoop/lib
  $ cp ${LIBRARY} lib
__EOF__
fi

cat <<__EOF__
  $ ant
  $ patch -p1 <../mapred-site.xml.patch
  $ patch -p1 <../${hadoop}_hadoop-env.sh.patch

Remember you'll need to change ${hadoop}/conf/mapred-site.xml to
connect to a Mesos cluster (the patch just uses 'local').

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
