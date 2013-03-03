We have ported version 0.20.205.0 of Hadoop to run on Mesos. Most of the Mesos port is implemented by a pluggable Hadoop scheduler, which communicates with Mesos to receive nodes to launch tasks on. However, a few small additions to Hadoop's internal APIs are also required.

You can build the ported version of Hadoop using `make hadoop`. It gets placed in the `hadoop/hadoop-0.20.205.0` directory. However, if you want to patch your own version of Hadoop to add Mesos support, you can also use .patch files located in `<Mesos directory>/hadoop`. These patches are likely to work on other Hadoop versions derived from 0.20. For example, for Cloudera's Distribution, GitHub user patelh has already created a [Mesos-compatible version of CDH3u3](https://github.com/patelh/cdh3u3-with-mesos).

To run Hadoop on Mesos, follow these steps:
<ol>
<li> Run `make hadoop` to build Hadoop 0.20.205.0 with Mesos support, or `TUTORIAL.sh` to patch and build your own Hadoop version.</li>
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

Note that when you run on a cluster, Hadoop (and Mesos) should be located on the same path on all nodes.

If you wish to run multiple JobTrackers, the easiest way is to give each one a different port by using a different Hadoop `conf` directory for each one and passing the `--conf` flag to `bin/hadoop` to specify which config directory to use. You can copy Hadoop's existing `conf` directory to a new location and modify it to achieve this.

## Hadoop Versions with Mesos Support Available

* 0.20.205.0: Included in Mesos (as described above).
* CDH3u3: [https://github.com/patelh/cdh3u3-with-mesos](https://github.com/patelh/cdh3u3-with-mesos)