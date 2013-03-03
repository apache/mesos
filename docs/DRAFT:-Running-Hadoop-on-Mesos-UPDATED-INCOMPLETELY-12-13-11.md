We have ported version 0.20.2 of Hadoop to run on Mesos. Most of the Mesos port is implemented by a pluggable Hadoop scheduler, which communicates with Mesos to receive nodes to launch tasks on. However, a few small additions to Hadoop's internal APIs are also required.

The ported version of Hadoop is included in the Mesos project under `frameworks/hadoop-0.20.2`. However, if you want to patch your own version of Hadoop to add Mesos support, you can also use the patch located at `frameworks/hadoop-0.20.2/hadoop-mesos.patch`. This patch should apply on any 0.20.* version of Hadoop, and is also likely to work on Hadoop distributions derived from 0.20, such as Cloudera's or Yahoo!'s.

To run Hadoop on Mesos, follow these steps:
<ol>
<li> Build Hadoop using <code>ant</code>.</li>
<li> Set up [[Hadoop's configuration|http://www.michael-noll.com/tutorials/running-hadoop-on-ubuntu-linux-single-node-cluster/]] as you would usually do with a new install of Hadoop, following the [[instructions on the Hadoop website|http://hadoop.apache.org/common/docs/r0.20.2/index.html]] (at the very least, you need to set <code>JAVA_HOME</code> in Hadoop's <code>conf/hadoop-env.sh</code> and set <code>mapred.job.tracker</code> in <code>conf/mapred-site.xml</code>).</li>
</li>
<li> Add the following parameters to Hadoop's <code>conf/mapred-site.xml</code>:
<pre>
&lt;property&gt;
  &lt;name&gt;mapred.jobtracker.taskScheduler&lt;/name&gt;
  &lt;value&gt;org.apache.hadoop.mapred.MesosScheduler&lt;/value&gt;
&lt;/property&gt;
&lt;property&gt;
  &lt;name&gt;mapred.mesos.master&lt;/name&gt;
  &lt;value&gt;[URL of Mesos master]&lt;/value&gt;
&lt;/property&gt;
</pre>
</li>
<li> Launch a JobTracker with <code>bin/hadoop jobtracker</code> (<i>do not</i> use <code>bin/start-mapred.sh</code>). The JobTracker will then launch TaskTrackers on Mesos when jobs are submitted.</li>
<li> Submit jobs to your JobTracker as usual.</li>
</ol>

Note that when you run on a cluster, Hadoop should be installed at the same path on all nodes, and so should Mesos.

## Running a version of Hadoop other than the one included with Mesos
If you run your own version of Hadoop instead of the one included in Mesos, you will first need to patch it, and copy the Hadoop Scheduler specific to Mesos over. You will then need to take an additional step before building and running Hadoop: you must set the `MESOS_HOME` environment variable to the location where Mesos is found. You need to do this both in your shell environment when you run `ant`, and in Hadoop's `hadoop-env.sh`.

## Running Multiple Hadoops
If you wish to run multiple JobTrackers (e.g. different versions of Hadoop), the easiest way is to give each one a different port by using a different Hadoop `conf` directory for each one and passing the `--conf` flag to `bin/hadoop` to specify which config directory to use. You can copy Hadoop's existing `conf` directory to a new location and modify it to achieve this.

------------
1. Setting up the environment:  
    - Create/Edit ~/.bashrc file.  
    `` ~$  vi ~/.bashrc ``  
    add the following:  
```
    # Set Hadoop-related environment variables. Here the username is billz
    export HADOOP_HOME=/Users/billz/mesos/frameworks/hadoop-0.20.2/  

    # Add Hadoop bin/ directory to PATH  
    export PATH=$PATH:$HADOOP_HOME/bin  

    # Set where you installed the mesos. For me is /Users/billz/mesos. billz is my username.  
    export MESOS_HOME=/Users/billz/mesos/
```
    - Go to hadoop directory that come with mesos's directory:  
    `cd ~/mesos/frameworks/hadoop-0.20.2/conf`  
    - Edit **hadoop-env.sh** file.  
    add the following:  
```
# The java implementation to use.  Required.
export JAVA_HOME=/System/Library/Frameworks/JavaVM.framework/Home

# Mesos uses this.
export MESOS_HOME=/Users/username/mesos/

# Extra Java runtime options.  Empty by default. This disables IPv6.
export HADOOP_OPTS=-Djava.net.preferIPv4Stack=true
```

2. Hadoop configuration:  
    - In **core-site.xml** file add the following:  
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
    - In **hdfs-site.xml** file add the following:  
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
    - In **mapred-site.xml** file add the following:  
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
$ sudo chown billz:billz /app/hadoop/tmp
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
    `~/mesos/frameworks/hadoop-0.20.2$ bin/hadoop dfs -copyFromLocal /tmp/gutenberg ~/gutenberg`  
    - Check the file(s) in Hadoop's HDFS  
    `~/mesos/frameworks/hadoop-0.20.2$ bin/hadoop dfs -ls ~/gutenberg`       
    - You should see something like the following:
```
 ~/mesos/frameworks/hadoop-0.20.2$ bin/hadoop dfs -ls ~/gutenberg
Found 6 items
-rw-r--r--   1 billz supergroup    5582656 2011-07-14 16:38 /user/billz/gutenberg/pg100.txt
-rw-r--r--   1 billz supergroup    3322643 2011-07-14 16:38 /user/billz/gutenberg/pg135.txt
-rw-r--r--   1 billz supergroup    1884720 2011-07-14 16:38 /user/billz/gutenberg/pg14833.txt
-rw-r--r--   1 billz supergroup    2130906 2011-07-14 16:38 /user/billz/gutenberg/pg18997.txt
-rw-r--r--   1 billz supergroup    3288707 2011-07-14 16:38 /user/billz/gutenberg/pg2600.txt
-rw-r--r--   1 billz supergroup    1423801 2011-07-14 16:38 /user/billz/gutenberg/pg5000.txt

```

6. Start all your frameworks!
    - Start Mesos's Master:      
    ` ~/mesos$ bin/mesos-master &`  
    - Start Mesos's Slave:       
    ` ~/mesos$ bin/mesos-slave --master=mesos://master@localhost:5050 &`  
    - Start Hadoop's namenode:  
    ` ~/mesos/frameworks/hadoop-0.20.2$ bin/hadoop namenode &`  
    - Start Hadoop's datanode:  
    ` ~/mesos/frameworks/hadoop-0.20.2$ bin/hadoop datanode &`  
    - Start Hadoop's jobtracker:  
    ` ~/mesos/frameworks/hadoop-0.20.2$ bin/hadoop jobtracker &`  

7. Run the MapReduce job:  
   We will now run your first Hadoop MapReduce job. We will use the [WordCount](http://wiki.apache.org/hadoop/WordCount) example job which reads text files and counts how often words occur. The input is text files and the output is text files, each line of which contains a word and the count of how often it occurred, separated by a tab.  

    - Run the "wordcount" example MapReduce job:  
    ` ~/mesos/frameworks/hadoop-0.20.2$ bin/hadoop jar build/hadoop-0.20.3-dev-examples.jar wordcount ~/gutenberg ~/output`  
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
~/mesos/frameworks/hadoop-0.20.2$ bin/hadoop dfs -ls ~/gutenberg
Found 2 items
drwxr-xr-x   - billz supergroup          0 2011-07-14 16:38 /user/billz/gutenberg
drwxr-xr-x   - billz supergroup          0 2011-07-19 15:35 /user/billz/output
```
   - View the output file:  
    `~/mesos/frameworks/hadoop-0.20.2$ bin/hadoop dfs -cat ~/output/part-r-00000`

### Congratulation! You have Hadoop running on Mesos!
