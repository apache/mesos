NOTE: The documentation below is only for convenience of reference. <i>As with any documentation, the following may become stale</i>. If you discover it is wrong, please: (1) run the command `./configure --help` and treat the printed output as the true source of the configure flag options, and (2) either edit this wiki page or send mail to mesos-dev@incubator.apache.org to point out the error.

The configure script itself accepts the following arguments to enable various options:

* `--with-python-headers=DIR`: Find Python header files in `DIR` (to turn on Python support). Recommended.
* `--with-webui`: Enable the Mesos web UI (which requires Python 2.6). Recommended.
* `--with-java-home=DIR`: Enable Java application/framework support with a given installation of Java. Required for Hadoop and Spark.
* `--with-java-headers=DIR`: Find Java header files (necessary for newer versions of OS X Snow Leopard).
* `--with-included-zookeeper` or `--with-zookeeper=DIR`: Enable master fault-tolerance using an existing ZooKeeper installation or the version of ZooKeeper bundled with Mesos. For details, see [[using ZooKeeper]].
