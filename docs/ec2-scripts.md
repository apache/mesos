---
layout: documentation
---

# EC2 Scripts

The `mesos-ec2` script located in the Mesos's `ec2` directory allows you to launch, manage and shut down Mesos clusters on Amazon EC2. You don't need to build Mesos to use this script -- you just need Python 2.6+ installed.

`mesos-ec2` is designed to manage multiple named clusters. You can launch a new cluster (telling the script its size and giving it a name), shutdown an existing cluster, or log into a cluster. Each cluster is identified by placing its machines into EC2 security groups whose names are derived from the name of the cluster. For example, a cluster named `test` will contain a master node in a security group called `test-master`, and a number of slave nodes in a security group called `test-slaves`. The `mesos-ec2` script will create these security groups for you based on the cluster name you request. You can also use them to identify machines belonging to each cluster in the EC2 Console or ElasticFox.

This guide describes how to get set up to run clusters, how to launch clusters, how to run jobs on them, and how to shut them down.

## Before You Start

* Create an Amazon EC2 key pair for yourself. This can be done by logging into your Amazon Web Services account through the [AWS console](http://aws.amazon.com/console/), clicking Key Pairs on the left sidebar, and creating and downloading a key. Make sure that you set the permissions for the private key file to `600` (i.e. only you can read and write it) so that `ssh` will work.
* Whenever you want to use the `mesos-ec2` script, set the environment variables `AWS_ACCESS_KEY_ID` and `AWS_SECRET_ACCESS_KEY` to your Amazon EC2 access key ID and secret access key. These can be obtained from the [AWS homepage](http://aws.amazon.com/) by clicking Account > Security Credentials > Access Credentials.

## Launching a Cluster

* Go into the `ec2` directory in the release of Mesos you downloaded.
* Run `./mesos-ec2 -k <keypair> -i <key-file> -s <num-slaves> launch <cluster-name>`, where `<keypair>` is the name of your EC2 key pair (that you gave it when you created it), `<key-file>` is the private key file for your key pair, `<num-slaves>` is the number of slave nodes to launch (try 1 at first), and `<cluster-name>` is the name to give to your cluster.
* After everything launches, check that Mesos is up and sees all the slaves by going to the Mesos Web UI link printed at the end of the script (`http://<master-hostname>:8080`).

You can also run `./mesos-ec2 --help` to see more usage options. The following options are worth pointing out:

* `--instance-type=<INSTANCE_TYPE>` can be used to specify an EC2 instance type to use. For now, the script only supports 64-bit instance types, and the default type is `m1.large` (which has 2 cores and 7.5 GB RAM). Refer to the Amazon pages about [EC2 instance types](http://aws.amazon.com/ec2/instance-types) and [EC2 pricing](http://aws.amazon.com/ec2/#pricing) for information about other instance types. 
* `--zone=<EC2_ZONE>` can be used to specify an EC2 availability zone to launch instances in. Sometimes, you will get an error because there is not enough capacity in one zone, and you should try to launch in another. This happens mostly with the `m1.large` instance types; extra-large (both `m1.xlarge` and `c1.xlarge`) instances tend to be more available.
* `--download=git` will tell the instances to download the latest release of Mesos from the github git repository.
* `--ft=<NUM_MASTERS>` can be used to run Mesos in fault tolerant (FT) mode by specifying `NUM_MASTERS` > 1.
* If one of your launches fails due to e.g. not having the right permissions on your private key file, you can run `launch` with the `--resume` option to restart the setup process on an existing cluster.

## Running Jobs

* Go into the `ec2` directory in the release of Mesos you downloaded.
* Run `./mesos-ec2 -k <keypair> -i <key-file> login <cluster-name>` to SSH into the cluster, where `<keypair>` and `<key-file>` are as above. (This is just for convenience; you could also use Elasticfox or the EC2 console.)
* Copy your code to all the nodes. To do this, you can use the provided script `~/mesos-ec2/copy-dir`, which, given a directory path, RSYNCs it to the same location on all the slaves.
* If your job needs to access large datasets, the fastest way to do that is to load them from Amazon S3 or an Amazon EBS device into an instance of the Hadoop Distributed File System (HDFS) on your nodes. The `mesos-ec2` script already sets up a HDFS instance for you. It's installed in `/root/ephemeral-hdfs`, and can be accessed using the `bin/hadoop` script in that directory. Note that the data in this HDFS goes away when you stop and restart a machine.
* There is also a _persistent HDFS_ instance in `/root/presistent-hdfs` that will keep data across cluster restarts. Typically each node has relatively little space of persistent data (about 3 GB), but you can use the `--ebs-vol-size` option to `mesos-ec2` to attach a persistent EBS volume to each node for storing the persistent HDFS.

If you get an "Executor on slave X disconnected" error when running your framework, you probably haven't copied your code the slaves. Use the `~/mesos-ec2/copy-dir` script to do that. If you keep getting the error, though, look at the slave's logs for that framework using the Mesos web UI. Please see [logging and debugging](logging-and-debugging.md) for details.

## Terminating a Cluster

_*Note that there is no way to recover data on EC2 nodes after shutting them down! Make sure you have copied everything important off the nodes before stopping them.*_

* Go into the `ec2` directory in the release of Mesos you downloaded.
* Run `./mesos-ec2 destroy <cluster-name>`.

## Pausing and Restarting EBS-Backed Clusters

The `mesos-ec2` script also supports pausing a cluster if you are using EBS-backed virtual machines (which all of our machine images are by default). In this case, the VMs are stopped but not terminated, so they _*lose all data on ephemeral disks (/mnt, ephemeral-hdfs)*_ but keep the data in their root partitions and their `persistent-hdfs`. Stopped machines will not cost you any EC2 cycles, but _*will*_ continue to cost money for EBS storage.

* To stop one of your clusters, go into the `ec2` directory and run `./mesos-ec2 stop <cluster-name>`.
* To restart it later, run `./mesos-ec2 -i <key-file> start <cluster-name>`.
* To ultimately destroy the cluster and stop consuming EBS space, run `./mesos-ec2 destroy <cluster-name>` as described in the previous section.

## Limitations

* The `mesos-ec2` script currently does not use the [deploy scripts](deploy-scripts.md) included with Mesos to manage its clusters. This will likely be fixed in the future.