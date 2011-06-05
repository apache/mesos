bin=`dirname "$0"`
bin=`cd "$bin"; pwd`

echo bin is $bin

# Set PATH to include Scala
#export PATH=$PATH:/root/scala-2.7.7.final/bin

#files that list master(s) and slaves
MASTER=`cat $bin/master`
SLAVES=`cat $bin/slaves`

#The dir where Mesos deployment scripts live
MESOS_ROOT=`cd $bin/..;pwd`
echo "MESOS_ROOT is $MESOS_ROOT"

MESOS_HOME=`cd $bin/../src;pwd`

MESOS_LOGS=/scratch/mesos-logs

#the dir where Hadoop is installed
HADOOP_HOME=/root/hadoop-0.20.2

#which java to use
JAVA_HOME=/usr/lib/jvm/java-6-sun

#options for ssh'ing
SSH_OPTS="-o stricthostkeychecking=no -o connecttimeout=2"

#LIBPROCESS_IP="hostname -i" #works on older versions of hostname, not on osx
#FULL_IP="hostname --all-ip-addresses" # newer versions of hostname only
#export LIBPROCESS_IP=`echo $FULL_IP|sed 's/\([^ ]*\) .*/\1/'`
