NOTE: this document is grossly out of date and probably not useful at all!

Debugging notes:
-----------------
- Be *very* careful about naming in config files, I *think* that FQDNs will work if you use them for both pbs_mom and pbs_server but a node keeps showing up with status "down" when I add it without using the .eecs.berkeley.edu (this is on my own laptop as both slave and server)


Mesos TORQUE framework readme
--------------------------------------------
This framework is a wrapper around the Torque cluster resource manager for the cluster which integrates with a cluster scheduler such as pbs_sched, maui, or moab.

Installing TORQUE:
------------------

option #1) install from source

option #2) sudo apt-get install torque-server, torque-mom (also potentially relevant: torque-dev, torque-client)


==Structure Overview of the Framework==
---------------------------------------

==FRAMEWORK EXECUTOR==
The mesos executor for this framework will run pbs_mom and tell it to look at the framework scheduler as its host

can interact with pbs_mom using `momctl`

==FRAMEWORK SCHEDULER==
The torque FW scheduler is responsible for managing the pbs_server daemon. This includees starting it if necessary, adding and removing nodes using the `pbsmgr` command as it accepts resource offers to dynamically grow (but never beyond its 'safe allocation') or shrink or releases resources.

For example, the FW scheduler can shrink mesos tasks (i.e. torque compute nodes) or killing them (i.e. remove compute notes) in response to a the scheduler queue becoming more (or entirely) empty in order to free resources for another active framework in the cluster (while maintining some minimum number of resources). It can also relaunch and grow tasks to account for increased queue lengths

The minimum number of resources torque should hang on to is configurable:
MIN_TORQUE_COMPUTE_NODE = resource vector of resources per compute node min
MIN_TORQUE_CLUSTER_SIZE = # compute nodes to keep around min

===Torque Scheduler===
The framework can use whichever torque compatible scheduler that is desired. By default it will use the default torque fifo scheduler (pbs_sched)

Permissions:
------------

****Currently****
As of right now, to run this framework, the node that the framework scheduler is run on will need to have pbs_server installed.

The framework scheduler will launch pbs_server for you, I *think* you need to be root to run the pbs_server daemon (I haven't figured out how to do it otherwise). 

****Future Alternative****
An alternate way that we could structure this framework is to require that pbs_server is running on some server already but that it is running with the intention to be fully managed by the torque mesos framework. I think that the management commands can be set up to work for non root users, which the framework could then run as. Then the mesos torque framework scheduler would take the address of the pbs_server as a parameter and would assume it has permissions to add and remove nodes from the server 


TODO:
-----
- explore an install a torque UI
 -- maybe PBSWeb  (http://www.clusterresources.com/pipermail/torqueusers/2004-March/000411.html) or apt-get install torque-gui (which has gui clients)
- figure out permissions better (this page http://www.clusterresources.com/torquedocs21/commands/qrun.shtml mentions "PBS Operation or Manager privilege.")
- might want to add mpi to this framework too (so that people can submit mpi jobs to torque framework)
