# Overview of Mesos

Mesos is a cluster management platform that provides resource sharing and isolation across distributed applications. For example, [Hadoop MapReduce](http://hadoop.apache.org), [MPI](http://www.mcs.anl.gov/research/projects/mpich2/), [Hypertable](http://hypertable.org), and [Spark](http://github.com/mesos/spark/wiki) (a new MapReduce-like framework from the Mesos team that supports low-latency interactive and iterative jobs) can run on Mesos.

#### Mesos Use-cases
* Run Hadoop, MPI, Spark and other cluster applications on a dynamically shared pool of machines
* Run **multiple instances of Hadoop** on the same cluster, e.g. separate instances for production and experimental jobs, or even multiple versions of Hadoop
* Build new cluster applications without having to reinvent low-level facilities for running tasks on different nodes, monitoring them, etc., and have your applications coexist with existing ones

#### Features of Mesos
* Master fault tolerance using ZooKeeper
* Isolation between tasks using Linux Containers
* Memory and CPU aware allocation
* Efficient fine-grained resource sharing abstraction (<i>resource offers</i>) that allows applications to achieve app-specific scheduling goals (e.g. Hadoop optimizes for data locality)
<br/>
Visit [mesosproject.org](http://mesosproject.org) for more details about Mesos.

_**Please note that Mesos is still in beta. Though the current version is in use in production at Twitter, it may have some stability issues in certain environments.**_

#### What you'll find on this page
1. Quick-start Guides
1. System Requirements
1. Downloading Mesos
1. Building Mesos
1. Testing the Build
1. Deploying to a Cluster
1. Where to Go From Here

# System Requirements

Mesos runs on Linux and Mac OS X, and has previously also been tested on OpenSolaris. You will need the following packages to run it:

* g++ 4.1 or higher.
* Python 2.6 (for the Mesos web UI). On Mac OS X 10.6 or earlier, get Python from [MacPorts](http://www.macports.org/) via `port install python26`, because OS X's version of Python is not 64-bit.
* Python 2.6 developer packages (on red-hat - sudo yum install python26-devel python-devel)
* cppunit (for building zookeeper) (on red-hat - sudo yum install cppunit-devel)
* Java JDK 1.6 or higher. Mac OS X 10.6 users will need to install the JDK from http://connect.apple.com/ and set `JAVA_HEADERS=/System/Library/Frameworks/JavaVM.framework/Versions/A/Headers`.

# Downloading and Building Mesos

Mesos uses the standard GNU build tools. See the section farther below for instructions for checking out and building the source code via source control.

To get running with Mesos version 0.9.0-incubating, our most recently release:

1. [Download Mesos 0.9.0-incubating](http://www.apache.org/dyn/closer.cgi/incubator/mesos/mesos-0.9.0-incubating/)
1. Run configure (there are a few different helper scripts that wrap the `configure` script called configure.<type-of-os>)
    1. In OS X: run `./configure.macosx`. If you're running Mountain Lion, you may need to follow the instructions [here](https://issues.apache.org/jira/browse/MESOS-261?focusedCommentId=13447058&page=com.atlassian.jira.plugin.system.issuetabpanels:comment-tabpanel#comment-13447058) and [here](https://issues.apache.org/jira/browse/MESOS-285).
    1. In Linux, you can probably just run `./configure --with-webui --with-included-zookeeper`. These flags are what we recommend; advanced users may want to exclude these flags or use others, see [[Mesos Configure Command Flag Options]].
1. run `make`

### NOTES:
* In the SVN trunk branch since Jan 19 2012 (when Mesos switched fully to the GNU Autotools build system), the build process attempts to guess where your Java include directory is, but if you have set the $JAVA_HOME environment variable, it will use $JAVA_HOME/include, which may not be correct (or exist) on your machine (in which case you will see an error such as: "configure: error: failed to build against JDK (using libtool)"). If this is the case, we suggest you unset the JAVA_HOME environment variable.
* `configure` may print a warning at the end about "unrecognized options: --with-java-home, ...". This comes from one of the nested `configure` scripts that we call, so it doesn't mean that your options were ignored.
* (NOT SURE IF THIS IS STILL RELEVANT) On 32-bit platforms, you should set `CXXFLAGS="-march=i486"` when running `configure` to ensure certain atomic instructions can be used.

# Checking Mesos Source Code out of Git or SVN

Currently, you can obtain the current Mesos development HEAD by checking it out from either the Apache SVN or Apache Git repository (the git repo is a mirror of the SVN repo)
* For SVN, use: `svn co https://svn.apache.org/repos/asf/incubator/mesos/trunk mesos-trunk`
* For git, use: `git clone git://git.apache.org/mesos.git`

# Running Example Frameworks and Testing the Build

Currently, in order to run the example frameworks (in src/examples), you must first build the test suite, as instructed below.

After you build Mesos by running the `make` command, you can build and run its example frameworks and unit tests (which use the example frameworks) by issuing the `make check` command from the directory where you ran the `make` command.

After you have done this, you can also set up a small Mesos cluster and run a job on it as follows:

1. Go into the directory where you built Mesos.
1. Type `bin/mesos-master.sh` to launch the master.
1. Take note of the IP and port that the master is running on, which will look something like <code>192.168.0.1:5050</code>. <i>Note: In this example the IP address of master is 192.168.0.1, and the port is 5050. We will continue to use the URL shown here for the rest of this example; however when you run the following commands replace all instances of it with the URL of your master.</i>
1. URL of master: <code>192.168.0.1:5050</code>
1. View the master's web UI at `http://[hostname of master]:5050`.
1. Launch a slave by typing <code>bin/mesos-slave.sh --master=192.168.0.1:5050</code>. The slave will show up on the master's web UI if you refresh it.
1. Run the C++ test framework (a sample that just runs five tasks on the cluster) using <code>src/test-framework 192.168.0.1:5050</code>. It should successfully exit after running five tasks.
1. You can also try the example python or Java frameworks, with commands like the following:
  2. `src/examples/java/test-framework 192.168.0.1:5050`
  2. `src/examples/python/test-framework 192.168.0.1:5050`

# Deploying to a Cluster

To run Mesos on more than one machine, you have multiple options:

* To launch a cluster on a private cluster that you own, use Mesos's [[deploy scripts]]
* To launch a cluster on Amazon EC2, you can use the Mesos [[EC2 scripts]]

# Where to Go From Here

* [[Mesos architecture]] -- an overview of Mesos concepts.
* [[Mesos Developers guide]] -- resources for developers contributing to Mesos. Style guides, tips for working with SVN/git, and more!
* [[App/Framework development guide]] -- learn how to build applications on top of Mesos.
* [[Configuring Mesos|Configuration]] -- a guide to the various settings that can be passed to Mesos daemons.
* Mesos system feature guides:
    * [[Deploy scripts]] for launching a Mesos cluster on a set of machines.
    * [[EC2 scripts]] for launching a Mesos cluster on Amazon EC2.
    * [[Logging and Debugging]] -- viewing Mesos and framework logs.
    * [[Using ZooKeeper]] for master fault-tolerance.
    * [[Using Linux Containers]] for resource isolation on slaves.
* Guide to running existing frameworks:
    * [[Running Spark on Mesos|https://github.com/mesos/spark/wiki]]
    * [[Using the Mesos Submit tool]]
    * [[Using Mesos with Hypertable on EC2|http://code.google.com/p/hypertable/wiki/Mesos]] (external link) - Hypertable is a distributed database platform.
    * [[Running Hadoop on Mesos]]
    * [[Running a web application farm on Mesos]]
    * [[Running Torque or MPI on Mesos]]
* [[Powered-by|Powered-by Mesos]] -- Projects that are using Mesos!
* [[Mesos code internals overview|Mesos Code Internals]] -- an overview of the codebase and internal organization.
* [[Mesos development roadmap|Mesos Roadmap]]
* [[The official Mesos website|http://mesosproject.org]]