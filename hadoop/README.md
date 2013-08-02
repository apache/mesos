Hadoop on Mesos
---------------

#### Overview ####

To run _Hadoop on Mesos_ you need to add the `hadoop-mesos-0.0.1.jar`
library to your Hadoop distribution (any distribution that supports
`hadoop-core-1.2.0` should work) and set some new configuration
properties. Read on for details.

#### Build ####

You can build `hadoop-mesos-0.0.1.jar` using Maven:

```
$ mvn package
```

If successful, the JAR will be at `target/hadoop-mesos-0.0.1.jar`.

> NOTE: If you want to build against a different version of Mesos than
> the default you'll need to update `mesos-version` in `pom.xml`.

We plan to provide already built JARs at http://repository.apache.org
in the near future!

#### Package ####

You'll need to download an existing Hadoop distribution. For this
guide, we'll use [CDH4.2.1][CDH4.2.1]. First grab the tar archive and
extract it.

```
$ wget http://archive.cloudera.com/cdh4/cdh/4/mr1-2.0.0-mr1-cdh4.2.1.tar.gz
...
$ tar zxf mr1-2.0.0-mr1-cdh4.2.1.tar.gz
```

> **Take note**, the extracted directory is `hadoop-2.0.0-mr1-cdh4.2.1`.

Now copy `hadoop-mesos-0.0.1.jar` into the `lib` folder.

```
$ cp /path/to/hadoop-mesos-0.0.1.jar hadoop-2.0.0-mr1-cdh4.2.1/lib/
```

_That's it!_ You now have a _Hadoop on Mesos_ distribution!

[CDH4.2.1]: http://www.cloudera.com/content/support/en/documentation/cdh4-documentation/cdh4-documentation-v4-2-1.html

#### Upload ####

You'll want to upload your _Hadoop on Mesos_ distribution somewhere
that Mesos can access in order to launch each `TaskTracker`. For
example, if you're already running HDFS:

```
$ tar czf hadoop-2.0.0-mr1-cdh4.2.1.tar.gz hadoop-2.0.0-mr1-cdh4.2.1
$ hadoop fs -put hadoop-2.0.0-mr1-cdh4.2.1.tar.gz /hadoop-2.0.0-mr1-cdh4.2.1.tar.gz
```

> **Consider** any permissions issues with your uploaded location
> (i.e., on HDFS you'll probably want to make the file world
> readable).

Now you'll need to configure your `JobTracker` to launch each
`TaskTracker` on Mesos!

#### Configure ####

Along with the normal configuration properties you might want to set
to launch a `JobTracker`, you'll need to set some Mesos specific ones
too.

Here are the mandatory configuration properties for
`conf/mapred-site.xml` (initialized to values representative of
running in [pseudo distributed
operation](http://hadoop.apache.org/docs/stable/single_node_setup.html#PseudoDistributed):

```
<property>
  <name>mapred.job.tracker</name>
  <value>localhost:9001</value>
</property>
<property>
  <name>mapred.jobtracker.taskScheduler</name>
  <value>org.apache.hadoop.mapred.MesosScheduler</value>
</property>
<property>
  <name>mapred.mesos.taskScheduler</name>
  <value>org.apache.hadoop.mapred.JobQueueTaskScheduler</value>
</property>
<property>
  <name>mapred.mesos.master</name>
  <value>localhost:5050</value>
</property>
<property>
  <name>mapred.mesos.executor</name>
  <value>hdfs://localhost:9000/hadoop-2.0.0-mr1-cdh4.2.1.tar.gz</value>
</property>
```

#### Start ####

Now you can start the `JobTracker` but you'll need to include the path
to the Mesos native library.

On Linux:

```
$ MESOS_NATIVE_LIBRARY=/path/to/libmesos.so hadoop jobtracker
```

And on OS X:

```
$ MESOS_NATIVE_LIBRARY=/path/to/libmesos.dylib hadoop jobtracker
```

> **NOTE: You do not need to worry about distributing your Hadoop
> configuration! All of the configuration properties read by the**
> `JobTracker` **along with any necessary** `TaskTracker` **specific
> _overrides_ will get serialized and passed to each** `TaskTracker`
> **on startup.**

_Please email user@mesos.apache.org with questions!_

----------
