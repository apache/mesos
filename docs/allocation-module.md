---
layout: documentation
---

# Mesos Allocation Module

The logic that the Mesos master uses to determine which frameworks to make offer resource offers to is encapsulated in the Master's _allocation module_.  The allocation module is a pluggable component that organizations can use to implement their own sharing policy, e.g. fair-sharing, Dominant Resource Fairness (see [the DRF paper](http://www.eecs.berkeley.edu/Pubs/TechRpts/2010/EECS-2010-55.pdf)), priority, etc.

## Allocation Module API

Mesos is implemented in C++, so allocation modules are implemented in C++, and inherit the @AllocatorProcess@ class defined in @MESOS_HOME/src/master/allocator.hpp@. As of the time of this writing (5/29/13), the API for allocation modules is as follows:

```
  virtual ~AllocatorProcess() {}

  virtual void initialize(
      const Flags& flags,
      const process::PID<Master>& master,
      const hashmap<std::string, RoleInfo>& roles) = 0;

  virtual void frameworkAdded(
      const FrameworkID& frameworkId,
      const FrameworkInfo& frameworkInfo,
      const Resources& used) = 0;

  virtual void frameworkRemoved(
      const FrameworkID& frameworkId) = 0;

  virtual void frameworkActivated(
      const FrameworkID& frameworkId,
      const FrameworkInfo& frameworkInfo) = 0;

  virtual void frameworkDeactivated(
      const FrameworkID& frameworkId) = 0;

  virtual void slaveAdded(
      const SlaveID& slaveId,
      const SlaveInfo& slaveInfo,
      const hashmap<FrameworkID, Resources>& used) = 0;

  virtual void slaveRemoved(
      const SlaveID& slaveId) = 0;

  virtual void updateWhitelist(
      const Option<hashset<std::string> >& whitelist) = 0;

  virtual void resourcesRequested(
      const FrameworkID& frameworkId,
      const std::vector<Request>& requests) = 0;

  // Whenever resources are "recovered" in the cluster (e.g., a task
  // finishes, an offer is removed because a framework has failed or
  // is failing over), or a framework refuses them, the master
  // invokes this callback.
  virtual void resourcesRecovered(
      const FrameworkID& frameworkId,
      const SlaveID& slaveId,
      const Resources& resources,
      const Option<Filters>& filters) = 0;

  // Whenever a framework that has filtered resources wants to revive
  // offers for those resources the master invokes this callback.
  virtual void offersRevived(
      const FrameworkID& frameworkId) = 0;
```

The default allocation module is the HierarchicalAllocatorProcess, which can be found in @MESOS_HOME/src/master/hierarchical_allocator_process.hpp@. You can reference this as a starting place if you choose to write your own allocation module.

## Sorter API

Additionally, the hierarchical allocator module can be extended without the need to reimplement the entirety of the allocation logic through the use of the @Sorter@ abstraction.

Sorters define the order that roles or frameworks should be offered resources in by taking "client" objects and some information about those clients and returning an ordered list of clients.

Sorters are implemented in C++ and inherit the @Sorter@ class defined in @MESOS_HOME/src/master/sorter.hpp@. As of the time of this writing, the API for Sorters is as follows:

```
  virtual ~Sorter() {}

  // Adds a client to allocate resources to. A client
  // may be a user or a framework.
  virtual void add(const std::string& client, double weight = 1) = 0;

  // Removes a client.
  virtual void remove(const std::string& client) = 0;

  // Readds a client to the sort after deactivate.
  virtual void activate(const std::string& client) = 0;

  // Removes a client from the sort, so it won't get allocated to.
  virtual void deactivate(const std::string& client) = 0;

  // Specify that resources have been allocated to the given client.
  virtual void allocated(const std::string& client,
                         const Resources& resources) = 0;

  // Specify that resources have been unallocated from the given client.
  virtual void unallocated(const std::string& client,
                           const Resources& resources) = 0;

  // Returns the resources that have been allocated to this client.
  virtual Resources allocation(const std::string& client) = 0;

  // Add resources to the total pool of resources this
  // Sorter should consider.
  virtual void add(const Resources& resources) = 0;

  // Remove resources from the total pool.
  virtual void remove(const Resources& resources) = 0;

  // Returns a list of all clients, in the order that they
  // should be allocated to, according to this Sorter's policy.
  virtual std::list<std::string> sort() = 0;

  // Returns true if this Sorter contains the specified client,
  // either active or deactivated.
  virtual bool contains(const std::string& client) = 0;

  // Returns the number of clients this Sorter contains,
  // either active or deactivated.
  virtual int count() = 0;
```

The default @Sorter@ is the DRFSorter, which implements fair sharing and can be found at @MESOS_HOME/src/master/drf_sorter.hpp@.

For DRF, if weights are specified in Sorter::add, a client's share will be divided by the weight, creating a form of priority. For example, a role that has a weight of 2 will be offered twice as many resources as a role with weight 1.
