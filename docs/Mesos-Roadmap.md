The Mesos development roadmap can roughly be separated into 3 main areas:

1. Building a community and promoting adoption
1. Core Mesos development
1. Mesos Application development

## 1. Growing a development community and promoting adoption
1. Migrate into Apache Incubator (see the [[incubator proposal|http://wiki.apache.org/incubator/MesosProposal]])
    1. Migrate issues from github issues into JIRA
1. Documentation management and organization

## 2. Core Mesos (e.g. scheduling, resource management, etc.)
1. More advanced allocation modules that implement the following functionality
    1. Resource revocation
    1. Resource inheritance, hierarchical scheduling
    1. A Mesos Service Level Objective
    1. Scheduling based on history
1. Migrate to ProtocolBuffers as the serialization format (under development at Twitter)
1. User authentication
1. Mesos application debugging support
    1. More advanced User Interface and [[Event History]] (i.e. logging) - See [[issue #143|https://github.com/mesos/mesos/issuesearch?state=open&q=event#issue/143]] for more details.
1. Testing infrastructure, and more tests!

## 3. Mesos applications

This category of future work is probably the most important! Technically speaking, a mesos application is defined as a mesos scheduler plus a mesos executor. Practically speaking, applications can serve many purposes. These can be broken down into a few categories.

### 3.1 Applications providing cluster OS functionality (e.g. storage, synchronization, naming...)

The core of Mesos has been designed using the same philosophy behind traditional [[Microkernel operating systems|http://en.wikipedia.org/wiki/Microkernel]]. This means that the core of Mesos (the kernel, if you will) provides a minimal set of low-level abstractions for cluster resources (see [[Mesos Architecture]] for an introduction to the resource offer abstraction). This, in turn means that higher level abstractions (analogous to the filesystem, memory sharing, etc. in traditional operating systems, as well as some abstractions that have no analog in traditional single node operating systems such as DNS).

those which are primarily intended to be used by other applications (as opposed to being used by human users). They can be seen as operating system modules that implement functionality in the form of services.

1. Meta Framework Development (under development at Berkeley, and also at Twitter)
    1. Allow users to submit a job (specifying their resource constraints) and have the job wait in a queue until the resources are acquired (the Application Scheduler translates those constraints into accepting or rejecting resource offers)
1. Slave data storage/caching services (a generalization of MapReduce's map output server)
1. Distributed file systems, like HDFS
1. Further Hypertable integration
1. Mesos package management application (i.e. the "apt-get" of the cluster... `apt-get install hadoop-0.20.0`)
1. A machine metadata database

### 3.2 Applications providing user facing services (e.g. web apps, PL abstractions...)

This category of applications is intended to interface with users. Due to nature of distributed applications (i.e. vs. what can be solved by simply using single computer applications) these apps tend to either (a) serving thousands to millions of users at a time (e.g. web applications), (b) large parallel computations (like MPI style jobs), (c) data intensive (e.g. enabling data analytics at large scale), or some combination of the above.

1. Spark
1. A graph processing framework? 
1. A streaming database framework?
1. Web applications (that server users)
1. High performance computing, MPI style jobs...

## Research Roadmap

In addition to a popular, (increasingly) stable, and (soon) production system, Mesos is also a research project that is pushing the borders of distributed system research! Check out <a href="http://mesosproject.org/about.html">the "About Mesos" page</a> for links to Mesos research papers, experiments, and other information about Mesos research.
