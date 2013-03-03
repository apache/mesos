## Running Mesos On Ubuntu Linux (Single Node Cluster)
This is step-by-step guide on setting up Mesos on a single node, and (optionally) running hadoop on top of Mesos. Here we are assuming Ubuntu 10.04 LTS - Long-term support 64-bit (Lucid Lynx).  Plus, we are using username "hadoop" and password "hadoop" for this guide.  

## Prerequisites:
* Java
    For Ubuntu 10.04 LTS (Lucid Lynx):  
    - Go to Applications > Accessories > Terminal.
    - Create/Edit ~/.bashrc file.  
    `` ~$  vi ~/.bashrc ``  
    add the following:  
    ``export JAVA_HOME=/usr/lib/jvm/java-6-sun``
    - `` ~$  echo $JAVA_HOME ``  
    You should see this:  
    ``/usr/lib/jvm/java-6-sun``
    - Add the Canonical Partner Repository to your apt repositories:    
    `~$ sudo add-apt-repository "deb http://archive.canonical.com/ lucid partner"`    
    Or edit `~$  vi  /etc/apt/sources.list`

    - Update the source list   
    `sudo apt-get update`  
    - Install Java (we'll use Sun Java 1.6 for this tutorial, but you can OpenJDK if you want to)

    `~$ sudo apt-get install build-essential sun-java6-jdk sun-java6-plugin`    
    `~$ sudo update-java-alternatives -s java-6-sun`  

* git  
    - `~$ sudo apt-get -y install git-core gitosis`  
    - As of June 2011 download [Git release is v1.7.5.4]

* Python and ssh

    - run `` ~$  sudo apt-get install python-dev ``
    - run `` ~$  sudo apt-get install openssh-server openssh-client ``

* OPTIONAL: If you want to run Hadoop, see [Hadoop setup](http://www.michael-noll.com/tutorials/running-hadoop-on-ubuntu-linux-single-node-cluster/)

## Mesos setup:
* Follow the general setup instructions at [Home](Home)

**Congratulation! You have mesos running on your Ubuntu Linux!**

## Simulating a Mesos cluster on one machine
1. Start a Mesos master: ` ~/mesos$ bin/mesos-master `
<pre>
 ~/mesos/bin$ ./mesos-master
I0604 15:47:56.499007 1885306016 logging.cpp:40] Logging to /Users/billz/mesos/logs
I0604 15:47:56.522259 1885306016 main.cpp:75] Build: 2011-06-04 14:44:57 by billz
I0604 15:47:56.522300 1885306016 main.cpp:76] Starting Mesos master
I0604 15:47:56.522532 1885306016 webui.cpp:64] Starting master web UI on port 8080
I0604 15:47:56.522539 7163904 master.cpp:389] Master started at mesos://master@10.1.1.1:5050
I0604 15:47:56.522676 7163904 master.cpp:404] Master ID: 201106041547-0
I0604 15:47:56.522743 19939328 webui.cpp:32] Web UI thread started
... trimmed ...
</pre>

2. Take note of the master URL `mesos://master@10.1.1.1:5050`

3. Start a Mesos slave: ` ~/mesos$ bin/mesos-slave --master=mesos://master@10.1.1.1:5050`

4. View the master's web UI at `http://10.1.1.1:8080` (here assuming this computer has IP address = 10.1.1.1).

5. Run the test framework: `~/mesos$ bin/examples/cpp-test-framework mesos://master@10.1.1.1:5050`

<pre>
Registered!
.Starting task 0 on ubuntu.eecs.berkeley.edu
Task 0 is in state 1
Task 0 is in state 2
.Starting task 1 on ubuntu.eecs.berkeley.edu
Task 1 is in state 1
Task 1 is in state 2
.Starting task 2 on ubuntu.eecs.berkeley.edu
Task 2 is in state 1
Task 2 is in state 2
.Starting task 3 on ubuntu.eecs.berkeley.edu
Task 3 is in state 1
Task 3 is in state 2
.Starting task 4 on ubuntu.eecs.berkeley.edu
Task 4 is in state 1
Task 4 is in state 2
</pre>

## Running Hadoop on Mesos [old link](https://github.com/mesos/mesos/wiki/Running-Hadoop-on-Mesos)  

We have ported version 0.20.2 of Hadoop to run on Mesos. Most of the Mesos port is implemented by a pluggable Hadoop scheduler, which communicates with Mesos to receive nodes to launch tasks on. However, a few small additions to Hadoop's internal APIs are also required.

The ported version of Hadoop is included in the Mesos project under `frameworks/hadoop-0.20.2`. However, if you want to patch your own version of Hadoop to add Mesos support, you can also use the patch located at `frameworks/hadoop-0.20.2/hadoop-mesos.patch`. This patch should apply on any 0.20.* version of Hadoop, and is also likely to work on Hadoop distributions derived from 0.20, such as Cloudera's or Yahoo!'s.

Most of the Hadoop setup is derived from [Michael G. Noll' guide](http://www.michael-noll.com/tutorials/running-hadoop-on-ubuntu-linux-single-node-cluster/)

To run Hadoop on Mesos, follow these steps:

1. Setting up the environment:  
   * Create/Edit ~/.bashrc file.  
   `` ~$  vi ~/.bashrc ``  
   add the following:  

```
    # Set Hadoop-related environment variables  
    export HADOOP_HOME=/usr/local/hadoop  

    # Add Hadoop bin/ directory to PATH  
    export PATH=$PATH:$HADOOP_HOME/bin  

    # Set where you installed the mesos. For me is /home/hadoop/mesos. hadoop is my username.  
    export MESOS_HOME=/home/hadoop/mesos
```
   * Go to hadoop directory that come with mesos's directory:  
   `cd ~/mesos/frameworks/hadoop-0.20.2/conf`  
   * Edit **hadoop-env.sh** file.  
   add the following:  

```
# The java implementation to use.  Required.
export JAVA_HOME=/usr/lib/jvm/java-6-sun

# The mesos use this.
export MESOS_HOME=/home/hadoop/mesos

# Extra Java runtime options.  Empty by default. This disable IPv6.
export HADOOP_OPTS=-Djava.net.preferIPv4Stack=true
```

2. Hadoop configuration:  
   * In **core-site.xml** file add the following:  

```
<!-- In: conf/core-site.xml -->
<property>
  <name>hadoop.tmp.dir</name>
  <value>/app/hadoop/tmp</value>
  <description>A base for other temporary directories.</description>
</property>

<property>
  <name>fs.default.name</name>
  <value>hdfs://localhost:54310</value>
  <description>The name of the default file system.  A URI whose
  scheme and authority determine the FileSystem implementation.  The
  uri's scheme determines the config property (fs.SCHEME.impl) naming
  the FileSystem implementation class.  The uri's authority is used to
  determine the host, port, etc. for a filesystem.</description>
</property>
```
   * In **hdfs-site.xml** file add the following:  

```
<!-- In: conf/hdfs-site.xml -->
<property>
  <name>dfs.replication</name>
  <value>1</value>
  <description>Default block replication.
  The actual number of replications can be specified when the file is created.
  The default is used if replication is not specified in create time.
  </description>
</property>
```
   * In **mapred-site.xml** file add the following:  

```
<!-- In: conf/mapred-site.xml -->
<property>
  <name>mapred.job.tracker</name>
  <value>localhost:9001</value>
  <description>The host and port that the MapReduce job tracker runs
  at.  If "local", then jobs are run in-process as a single map
  and reduce task.
  </description>
</property>

<property>
  <name>mapred.jobtracker.taskScheduler</name>
  <value>org.apache.hadoop.mapred.MesosScheduler</value>
</property>
<property>
  <name>mapred.mesos.master</name>
  <value>mesos://master@10.1.1.1:5050</value> <!-- Here we are assuming your host IP address is 10.1.1.1 -->
</property>

```
  
3. Build the Hadoop-0.20.2 that come with Mesos
    - Start a new bash shell or reboot the host:  
    ` ~$ sudo shutdown -r now`  
    - Login as "hadoop" or any user that you start with this guide
    - Go to hadoop-0.20.2 directory:  
    ` ~$ cd ~/mesos/frameworks/hadoop-0.20.2`  
    - Build Hadoop:  
    ` ~$ ant `  
    - Build the Hadoop's Jar files:  
    ` ~$ ant compile-core jar`   
    ` ~$ ant examples jar`  

4. Setup Hadoop’s Distributed File System **HDFS**:  
    - create the directory and set the required ownerships and permissions: 

```
$ sudo mkdir /app/hadoop/tmp
$ sudo chown hadoop:hadoop /app/hadoop/tmp
# ...and if you want to tighten up security, chmod from 755 to 750...
$ sudo chmod 750 /app/hadoop/tmp
```
    - formatting the Hadoop filesystem:  
    `~/mesos/frameworks/hadoop-0.20.2$  bin/hadoop namenode -format`
    

5. Copy local example data to HDFS  
    - Download some plain text document from Project Gutenberg  
    [The Notebooks of Leonardo Da Vinci](http://www.gutenberg.org/ebooks/5000)  
    [Ulysses by James Joyce](http://www.gutenberg.org/ebooks/4300)  
    [The Complete Works of William Shakespeare](http://www.gutenberg.org/ebooks/100)  
    save these to /tmp/gutenberg/ directory.  
    - Copy files from our local file system to Hadoop’s HDFS  
    `~/mesos/frameworks/hadoop-0.20.2$ bin/hadoop dfs -copyFromLocal /tmp/gutenberg /user/hadoop/gutenberg`  
    - Check the file(s) in Hadoop's HDFS  
    `~/mesos/frameworks/hadoop-0.20.2$ bin/hadoop dfs -ls /user/hadoop/gutenberg`       
    - You should see something like the following:

```
 ~/mesos/frameworks/hadoop-0.20.2$ bin/hadoop dfs -ls /user/hadoop/gutenberg
Found 6 items
-rw-r--r--   1 hadoop supergroup    5582656 2011-07-14 16:38 /user/hadoop/gutenberg/pg100.txt
-rw-r--r--   1 hadoop supergroup    3322643 2011-07-14 16:38 /user/hadoop/gutenberg/pg135.txt
-rw-r--r--   1 hadoop supergroup    1423801 2011-07-14 16:38 /user/hadoop/gutenberg/pg5000.txt 
```

6. Start all your frameworks!
    - Start Mesos's Master:      
    ` ~/mesos$ bin/mesos-master &`  
    - Start Mesos's Slave:       
    ` ~/mesos$ bin/mesos-slave --url=mesos://master@localhost:5050 &`  
    - Start Hadoop's namenode:  
    ` ~/mesos/frameworks/hadoop-0.20.2$ bin/hadoop-daemon.sh namenode`  
    - Start Hadoop's datanode:  
    ` ~/mesos/frameworks/hadoop-0.20.2$ bin/hadoop-daemon.sh datanode`  
    - Start Hadoop's jobtracker:  
    ` ~/mesos/frameworks/hadoop-0.20.2$ bin/hadoop-daemon.sh jobtracker`  

    - Note: There may be some intermediate issue with dfs directory. Try deleting the /app/hadoop/tmp and /tmp/hadoop*.  Then do a hadoop namenode -format.

7. Run the MapReduce job:  
   We will now run your first Hadoop MapReduce job. We will use the [WordCount](http://wiki.apache.org/hadoop/WordCount) example job which reads text files and counts how often words occur. The input is text files and the output is text files, each line of which contains a word and the count of how often it occurred, separated by a tab.  

    - Run the "wordcount" example MapReduce job:  
    ` ~/mesos/frameworks/hadoop-0.20.2$ bin/hadoop jar build/hadoop-0.20.3-dev-examples.jar wordcount /user/hadoop/gutenberg /user/hadoop/output`  
    - You will see something like the following:  

```
11/07/19 15:34:29 INFO input.FileInputFormat: Total input paths to process : 6
11/07/19 15:34:29 INFO mapred.JobClient: Running job: job_201107191533_0001
11/07/19 15:34:30 INFO mapred.JobClient:  map 0% reduce 0%
11/07/19 15:34:43 INFO mapred.JobClient:  map 16% reduce 0%

[ ... trimmed ... ]
```

8. Web UI for Hadoop and Mesos:   
    - [http://localhost:50030](http://localhost:50030) - web UI for MapReduce job tracker(s)  
    - [http://localhost:50060](http://localhost:50060) - web UI for task tracker(s)  
    - [http://localhost:50070](http://localhost:50070) - web UI for HDFS name node(s)  
    - [http://localhost:8080](http://localhost:8080) - web UI for Mesos master  

9. Retrieve the job result from HDFS:
   - list the HDFS directory:

```
~/mesos/frameworks/hadoop-0.20.2$ bin/hadoop dfs -ls /user/billz/gutenberg
Found 2 items
drwxr-xr-x   - hadoop supergroup          0 2011-07-14 16:38 /user/hadoop/gutenberg
drwxr-xr-x   - hadoop supergroup          0 2011-07-19 15:35 /user/hadoop/output
```
   - View the output file:  
    `~/mesos/frameworks/hadoop-0.20.2$ bin/hadoop dfs -cat /user/hadoop/output/part-r-00000`

### Congratulation! You have Hadoop running on mesos, and mesos running on your Ubuntu Linux!


## Need more help?   
* [Use our mailing lists](http://incubator.apache.org/projects/mesos.html)
* mesos-dev@incubator.apache.org

## Wants to contribute?
* [Contributing to Open Source Projects HOWTO](http://www.kegel.com/academy/opensource.html)
* [Submit a Bug](https://issues.apache.org/jira/browse/MESOS)