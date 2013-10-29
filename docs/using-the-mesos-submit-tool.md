---
layout: documentation
---

# Using the Mesos Submit Tool

Sometimes you just want to run a command or launch a binary on all (or a subset) of the nodes in your cluster. `mesos-submit` lets you do just that!

Mesos-submit is a little framework that lets you run a binary in the Mesos cluster without having to keep a scheduler running on the machine you submitted it from. You call mesos-submit <binary> and the script will launch a framework with a single task. This task then takes over as the scheduler for this framework (using the scheduler failover feature), and the mesos-submit process (which was the initial scheduler) can safely exit. The task then goes on to run the command. This is useful for people who want to submit their schedulers to the cluster for example.