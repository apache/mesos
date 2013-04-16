#!/bin/bash

# Determine the Hadoop distribution to use.
if test -z "${1}"; then
    distribution="0.20.205.0"
    url="http://archive.apache.org/dist/hadoop/core/hadoop-0.20.205.0"
    bundle="hadoop-0.20.205.0.tar.gz"
elif test "${1}" = "0.20.2-cdh3u3"; then
    distribution="0.20.2-cdh3u3"
    url="http://archive.cloudera.com/cdh/3"
    bundle="hadoop-0.20.2-cdh3u3.tar.gz"
elif test "${1}" = "2.0.0-mr1-cdh4.1.2"; then
    distribution="2.0.0-mr1-cdh4.1.2"
    url="http://archive.cloudera.com/cdh4/cdh/4"
    bundle="mr1-2.0.0-mr1-cdh4.1.2.tar.gz"
elif test "${1}" = "2.0.0-mr1-cdh4.2.0"; then
    distribution="2.0.0-mr1-cdh4.2.0"
    url="http://archive.cloudera.com/cdh4/cdh/4"
    bundle="mr1-2.0.0-mr1-cdh4.2.0.tar.gz"
fi

hadoop="hadoop-${distribution}"

# The potentially running JobTracker, that we need to kill.
jobtracker_pid=

# Trap Ctrl-C (signal 2).
trap 'test ! -z ${jobtracker_pid} && kill ${jobtracker_pid}; echo; exit 1' 2


# A helper function to run one or more commands.
# If any command fails, the tutorial exits with a helpful message.
function run() {
  for command in "${@}"; do
      eval ${command}

      if test "$?" != 0; then
          cat <<__EOF__

${RED}Oh no! We failed to run '${command}'. If you need help try emailing:

  mesos-dev@incubator.apache.org

(Remember to include as much debug information as possible.)${NORMAL}

__EOF__
          exit 1
      fi
  done
}


# A helper function to execute a step of the tutorial (i.e., one or
# more commands). Prints the command(s) out before they are run and
# waits for the user to confirm. In addition, the commands are
# appended to the summary.
summary=""
function execute() {
  echo
  for command in "${@}"; do
      echo "  $ ${command}"
  done
  echo

  read -e -p "${BRIGHT}Hit enter to continue.${NORMAL} "
  echo

  for command in "${@}"; do
      run "${command}"

      # Append to the summary.
      summary="${summary}
$ ${command}"
  done
}


# Make sure we start out in the right directory.
cd `dirname ${0}`

# Include wonderful colors for our tutorial!
test -f ../support/colors.sh && . ../support/colors.sh

# Make sure we have all the necessary files/directories we need.
resources="TUTORIAL.sh \
  hadoop-gridmix.patch \
  ${hadoop}_hadoop-env.sh.patch \
  ${hadoop}_mesos.patch \
  mapred-site.xml.patch \
  mesos \
  mesos-executor"

if test ${distribution} = "0.20.205.0"; then
    resources="${resources} hadoop-7698-1.patch"
fi

if test ${distribution} = "2.0.0-mr1-cdh4.1.2" -o ${distribution} = "2.0.0-mr1-cdh4.2.0"; then
    resources="${resources} HadoopPipes.cc.patch"
fi

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

# Make sure we have all the build tools we need.
programs="mvn \
  ant"

for program in `echo ${programs}`; do
    which ${program} > /dev/null
    if test "$?" != 0; then
        cat <<__EOF__

${RED}We seem to be missing ${program} from PATH.  Please install
${program} and re-run this tutorial.  If you still have troubles, please report
this to:

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
if test ! -e ${bundle}; then
    cat <<__EOF__

We'll try and grab ${hadoop} from ${url}/${bundle} for you now.

__EOF__
    execute "wget ${url}/${bundle}"
else
    cat <<__EOF__

${RED}It looks like you've already downloaded ${bundle}, so
we'll skip that step.${NORMAL}

__EOF__
fi


# Extract the archive.
if test ! -d ${hadoop}; then
    cat <<__EOF__

Let's start by extracting ${bundle}.

__EOF__
    execute "tar zxf ${bundle}"
else
    cat <<__EOF__

${RED}It looks like you've already extracted ${bundle}, so
we'll skip that step.${NORMAL}

__EOF__
fi


# Change into Hadoop directory.
cat <<__EOF__

Okay, now let's change into the ${hadoop} directory in order to apply
some patches, copy in the Mesos specific code, and build everything.

__EOF__

execute "cd ${hadoop}"


# Apply the GridMix patch.
cat <<__EOF__

To run Hadoop on Mesos under Java 7 we need to apply a rather minor patch
(hadoop-gridmix.patch) to GridMix, a contribution in Hadoop. See 'NOTES'
file for more info.

__EOF__

# Check and see if the patch has already been applied.
grep 'private String getEnumValues' \
  src/contrib/gridmix/src/java/org/apache/hadoop/mapred/gridmix/Gridmix.java \
  >/dev/null

if test ${?} == "0"; then
    cat <<__EOF__

${RED}It looks like you've already applied the patch, so we'll skip
applying it now.${NORMAL}

__EOF__
else
    execute "patch -p1 <../hadoop-gridmix.patch"
fi

# Apply the 'jsvc' patch for hadoop-0.20.205.0.
if test ${distribution} = "0.20.205.0"; then
  cat <<__EOF__

To build Mesos executor bundle, we need to apply a patch for
'jsvc' target (hadoop-7698-1.patch) that is broken in build.xml.

__EOF__

  # Check and see if the patch has already been applied.
  grep 'os-name' build.xml >/dev/null

  if test ${?} == "0"; then
      cat <<__EOF__

  ${RED}It looks like you've already applied the patch, so we'll skip
  applying it now.${NORMAL}

__EOF__
  else
      execute "patch -p1 <../hadoop-7698-1.patch"
  fi
fi

# Copy over the Mesos contrib components (and mesos-executor).
cat <<__EOF__

Now, we'll copy over the Mesos contrib components.

__EOF__

execute "cp -r ../mesos src/contrib" \
  "cp -p ../mesos-executor bin"


# Apply the patch to build the contrib.
cat <<__EOF__

In addition, we will need to edit ivy/libraries.properties and
src/contrib/build.xml to hook the Mesos contrib component into the
build. We've included a patch (${hadoop}_mesos.patch) to do that for
you.

__EOF__


# Check and see if the patch has already been applied.
grep mesos src/contrib/build.xml >/dev/null

if test ${?} == "0"; then
    cat <<__EOF__

${RED}It looks like you've already applied the patch, so we'll skip
applying it now.${NORMAL}

__EOF__
else
    execute "patch -p1 <../${hadoop}_mesos.patch"
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

run "${JAVA_HOME}/bin/javac PlatformName.java"

PLATFORM=`${JAVA_HOME}/bin/java -Xmx32m PlatformName | sed -e "s/ /_/g"`

run "rm PlatformName.*"


# Copy over libraries.
MESOS_JAR=`echo ${MESOS_BUILD_DIR}/src/mesos-*.jar`
PROTOBUF_JAR=`echo ${MESOS_BUILD_DIR}/protobuf-*.jar`

cat <<__EOF__

Now we'll copy over the necessary libraries we need from the build
directory.

__EOF__

execute "cp ${PROTOBUF_JAR} lib" \
  "cp ${MESOS_JAR} lib" \
  "mkdir -p lib/native/${PLATFORM}" \
  "cp ${LIBRARY} lib/native/${PLATFORM}"

if test ${distribution} = "0.20.205.0"; then
    cat <<__EOF__

The Apache Hadoop distribution requires that we also copy some
libraries to multiple places. :/

__EOF__

    execute "cp ${PROTOBUF_JAR} share/hadoop/lib" \
      "cp ${MESOS_JAR} share/hadoop/lib" \
      "cp ${LIBRARY} lib"
fi


# Apply conf/mapred-site.xml patch.
cat <<__EOF__

First we need to configure Hadoop appropriately by modifying
conf/mapred-site.xml (as is always required when running Hadoop).
In order to run Hadoop on Mesos we need to set at least these four
properties:

  mapred.job.tracker

  mapred.jobtracker.taskScheduler

  mapred.mesos.master

  mapred.mesos.executor

The 'mapred.job.tracker' property should be set to the host:port where
you want to launch the JobTracker (e.g., localhost:54321).

The 'mapred.jobtracker.taskScheduler' property must be set to
'org.apache.hadoop.mapred.MesosScheduler'.

If you've alredy got a Mesos master running you can use that for
'mapred.mesos.master', but for this tutorial well just use 'local' in
order to bring up a Mesos "cluster" within the process. To connect to
a remote master simply use the URL used to connect the slave to the
master (e.g., localhost:5050).

The 'mapred.mesos.executor' property must be set to the location
of Mesos executor bundle so that Mesos slaves can download
and run the executor.
NOTE: You need to MANUALLY upload the Mesos executor bundle to
the above location.


We've got a prepared patch (mapred-site.xml.patch) for
conf/mapred-site.xml that makes the changes necessary
to get everything running with a local Mesos cluster.

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
    execute "patch -p1 <../mapred-site.xml.patch"
fi



# Apply conf/hadoop-env.sh patch.
cat <<__EOF__

Most users will need to set JAVA_HOME in conf/hadoop-env.sh, but we'll
also need to set MESOS_NATIVE_LIBRARY and update the HADOOP_CLASSPATH
to include the Mesos contrib classfiles. We've prepared a patch
(${hadoop}_hadoop-env.sh.patch) for conf/hadoop-env.sh that makes the
necessary changes.

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
    execute "patch -p1 <../${hadoop}_hadoop-env.sh.patch"
fi

if test ${distribution} = "2.0.0-mr1-cdh4.1.2" -o ${distribution} = "2.0.0-mr1-cdh4.2.0"; then
    # Apply HadoopPipes.cc patch.
    cat <<__EOF__

    This version of Hadoop needs to be patched to build on GCC 4.7 and newer compilers.

__EOF__

    read -e -p "${BRIGHT}Hit enter to continue.${NORMAL} "
    echo

    patch --dry-run --silent --force -p1 \
	<../HadoopPipes.cc.patch 1>/dev/null 2>&1

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

    read -e -p "${BRIGHT}Patch src/c++/pipes/impl/HadoopPipes.cc?${NORMAL} [${DEFAULT}] "
    echo
    test -z ${REPLY} && REPLY=${DEFAULT}
    if test ${REPLY} == "Y" -o ${REPLY} == "y"; then
	execute "patch -p1 <../HadoopPipes.cc.patch"
    fi
fi

# Build Hadoop and Mesos executor package that Mesos slaves can download
# and execute.
# TODO(vinod): Create a new ant target in build.xml that builds the executor.
# NOTE: We specifically set the version when calling ant, to ensure we know
# the resulting directory name.
cat <<__EOF__

Okay, let's try building Hadoop.

__EOF__

read -e -p "${BRIGHT}Hit enter to continue.${NORMAL} "
echo

if test ${distribution} = "2.0.0-mr1-cdh4.1.2" -o ${distribution} = "2.0.0-mr1-cdh4.2.0"; then
  cat <<__EOF__

  We need to chmod +x install-sh scripts to compile
  C++ components needed by the 'bin-package' target in CDH4.
  We also need to specifically set the 'reactor.repo' property.

__EOF__
  execute "find . -name "*install-sh*" | xargs chmod +x" \
    "ant -Dreactor.repo=file://$HOME/.m2/repository \
-Dversion=${distribution} -Dcompile.c++=true compile bin-package"
else
  execute "ant -Dversion=${distribution} compile bin-package"
fi

cat <<__EOF__

To build the Mesos executor package, we first copy the
necessary Mesos libraries.

__EOF__

# Copy the Mesos native library.
execute "cd build/${hadoop}" \
  "mkdir -p lib/native/${PLATFORM}" \
  "cp ${LIBRARY} lib/native/${PLATFORM}"

if test ${distribution} != "0.20.205.0"; then
  cat <<__EOF__

  We will remove Cloudera patches from the Mesos executor package
  to save space (~62MB).

__EOF__
  execute "rm -rf cloudera"
fi

cat <<__EOF__

  Finally, we will build the Mesos executor package as follows:

__EOF__

# We re-name the directory to 'hadoop' so that the Mesos executor
# can be agnostic to the Hadoop version.
execute "cd .." \
  "mv ${hadoop} ${hadoop}-mesos" \
  "tar czf ${hadoop}-mesos.tar.gz ${hadoop}-mesos/"

# Start JobTracker.
cat <<__EOF__

${GREEN}Build success!${NORMAL}

The Mesos distribution is now built in '${hadoop}-mesos'

Now let's run something!

We'll try and start the JobTracker from the Mesos distribution path via:
  $ cd ${hadoop}-mesos
  $ ./bin/hadoop jobtracker

__EOF__

read -e -p "${BRIGHT}Hit enter to continue.${NORMAL} "
echo


# Fake the resources for this local slave, because the default resources
# (esp. memory on MacOSX) offered by the slave might not be enough to
# launch TaskTrackers.
# TODO(vinod): Pipe these commands through 'execute()' so that they
# can be appended to the summary.
cd ${hadoop}-mesos
export MESOS_RESOURCES="cpus:16;mem:16384;disk:307200;ports:[31000-32000]"
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
  ${MESOS_BUILD_DIR}/src/mesos out

__EOF__

read -e -p "${BRIGHT}Hit enter to continue.${NORMAL} "
echo

rm -rf out # TODO(benh): Ask about removing this first.

./bin/hadoop jar hadoop-examples-${distribution}.jar wordcount \
    ${MESOS_BUILD_DIR}/src/mesos out


if test ${?} == "0"; then
    cat <<__EOF__

${GREEN}Success!${NORMAL} We'll kill the JobTracker and exit.

Summary:
${summary}

Remember you'll need to make some changes to
${hadoop}/conf/mapred-site.xml to run Hadoop on a
real Mesos cluster:

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
