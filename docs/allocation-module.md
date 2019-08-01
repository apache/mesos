---
title: Apache Mesos - Allocation Modules
layout: documentation
---

# Mesos Allocation Modules

The logic that the Mesos master uses to determine which frameworks to make resource offers to is encapsulated in the master's _allocator module_. The allocator is a pluggable component that organizations can use to implement their own sharing policy, e.g. fair-sharing, priority, etc., or tune the default hierarchical Dominant Resource Fairness algorithm (see [the DRF paper](https://www.cs.berkeley.edu/~alig/papers/drf.pdf)).

To use a custom allocator in Mesos, one must:

- [Implement](#writing-a-custom-allocator) the `Allocator` interface as defined in `mesos/allocator/allocator.hpp`,

- [Wrap](#wiring-up-a-custom-allocator) the allocator implementation in a module and load it in the Mesos master.

<a name="writing-a-custom-allocator"></a>
## Writing a custom allocator

Allocator modules are implemented in C++, the same language in which Mesos is written. They must subclass the `Allocator` interface defined in `mesos/allocator/allocator.hpp`. However, your implementation can be a C++ proxy, which delegates calls to an actual allocator written in a language of your choice.

The default allocator is `HierarchicalDRFAllocatorProcess`, which lives in `$MESOS_HOME/src/master/allocator/mesos/hierarchical.hpp`. Like most Mesos components, it is actor-based, which means all interface methods are non-blocking and return immediately after putting the corresponding action into the actor's queue. If you would like to design your custom allocator in a similar manner, subclass `MesosAllocatorProcess` from `$MESOS_HOME/src/master/allocator/mesos/allocator.hpp` and wrap your actor-based allocator in `MesosAllocator`. This dispatches calls to the underlying actor and controls its lifetime. You can refer to `HierarchicalDRFAllocatorProcess` as a starting place if you choose to write your own actor-based allocation module.


Additionally, the built-in hierarchical allocator can be extended without the need to reimplement the entirety of the allocation logic. This is possible through the use of the `Sorter` abstraction. Sorters define the order in which hierarchy layers (e.g. roles or frameworks) should be offered resources by taking "client" objects and some information about those clients and returning an ordered list of clients.

Sorters are implemented in C++ and inherit the `Sorter` class defined in `$MESOS_HOME/src/master/allocator/mesos/sorter/sorter.hpp`. The default sorter is `DRFSorter`, which implements fair sharing and can be found in `$MESOS_HOME/src/master/allocator/mesos/sorter/drf/sorter.hpp`. This sorter is capable of expressing priorities by specifying weights in `Sorter::add()`. Each client's share is divided by its weight. For example, a role that has a weight of 2 will be offered twice as many resources as a role with weight 1.

<a name="wiring-up-a-custom-allocator"></a>
## Wiring up a custom allocator

Once a custom allocator has been written, the next step is to override the built-in implementation with your own. This process consists of several steps:

- Wrap your allocator in a Mesos allocator module,

- Load this module in Mesos master.

An allocator module is a factory function and a module description, as defined in `mesos/module/allocator.hpp`. Assuming the allocation logic is implemented by the `ExternalAllocator` class declared in `external_allocator.hpp`, the following snippet describes the implementation of an allocator module named `ExternalAllocatorModule`:

~~~{.cpp}
#include <mesos/allocator/allocator.hpp>
#include <mesos/module/allocator.hpp>
#include <stout/try.hpp>

#include "external_allocator.hpp"

using namespace mesos;
using mesos::allocator::Allocator;
using mesos::internal::master::allocator::HierarchicalDRFAllocator;

static Allocator* createExternalAllocator(const Parameters& parameters)
{
  Try<Allocator*> allocator = ExternalAllocator::create();
  if (allocator.isError()) {
    return nullptr;
  }

  return allocator.get();
}

// Declares an ExternalAllocator module named 'ExternalAllocatorModule'.
mesos::modules::Module<Allocator> ExternalAllocatorModule(
    MESOS_MODULE_API_VERSION,
    MESOS_VERSION,
    "Mesos Contributor",
    "engineer@example.com",
    "External Allocator module.",
    nullptr,
    createExternalAllocator);
~~~

Refer to the [Mesos Modules documentation](modules.md) for instructions on how to compile and load a module in Mesos master.
