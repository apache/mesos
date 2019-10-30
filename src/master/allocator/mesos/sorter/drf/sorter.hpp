// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef __MASTER_ALLOCATOR_MESOS_SORTER_DRF_SORTER_HPP__
#define __MASTER_ALLOCATOR_MESOS_SORTER_DRF_SORTER_HPP__

#include <algorithm>
#include <set>
#include <string>
#include <vector>

#include <mesos/mesos.hpp>
#include <mesos/resource_quantities.hpp>
#include <mesos/resources.hpp>
#include <mesos/values.hpp>

#include <stout/check.hpp>
#include <stout/hashmap.hpp>
#include <stout/option.hpp>

#include "master/allocator/mesos/sorter/drf/metrics.hpp"

#include "master/allocator/mesos/sorter/sorter.hpp"


namespace mesos {
namespace internal {
namespace master {
namespace allocator {

class DRFSorter : public Sorter
{
public:
  DRFSorter();

  explicit DRFSorter(
      const process::UPID& allocator,
      const std::string& metricsPrefix);

  ~DRFSorter() override;

  void initialize(
      const Option<std::set<std::string>>& fairnessExcludeResourceNames)
    override;

  void add(const std::string& clientPath) override;

  void remove(const std::string& clientPath) override;

  void activate(const std::string& clientPath) override;

  void deactivate(const std::string& clientPath) override;

  void updateWeight(const std::string& path, double weight) override;

  void allocated(
      const std::string& clientPath,
      const SlaveID& slaveId,
      const Resources& resources) override;

  void update(
      const std::string& clientPath,
      const SlaveID& slaveId,
      const Resources& oldAllocation,
      const Resources& newAllocation) override;

  void unallocated(
      const std::string& clientPath,
      const SlaveID& slaveId,
      const Resources& resources) override;

  const hashmap<SlaveID, Resources>& allocation(
      const std::string& clientPath) const override;

  const ResourceQuantities& allocationScalarQuantities(
      const std::string& clientPath) const override;
  const ResourceQuantities& allocationScalarQuantities()
      const override;

  Resources allocation(
      const std::string& clientPath,
      const SlaveID& slaveId) const override;

  void addSlave(
      const SlaveID& slaveId,
      const ResourceQuantities& scalarQuantities) override;

  void removeSlave(const SlaveID& slaveId) override;

  std::vector<std::string> sort() override;

  bool contains(const std::string& clientPath) const override;

  size_t count() const override;

private:
  // A node in the sorter's tree.
  struct Node;

  // Returns the dominant resource share for the node.
  double calculateShare(const Node* node) const;

  // Returns the weight associated with the node. If no weight has
  // been configured for the node's path, the default weight (1.0) is
  // returned.
  double getWeight(const Node* node) const;

  // Returns the client associated with the given path. Returns
  // nullptr if the path is not found or if the path identifies an
  // internal node in the tree (not a client).
  Node* find(const std::string& clientPath) const;

  // Resources (by name) that will be excluded from fair sharing.
  Option<std::set<std::string>> fairnessExcludeResourceNames;

  // If true, sort() will recalculate all shares and resort the tree.
  bool dirty = false;

  // The root node in the sorter tree.
  Node* root;

  // To speed lookups, we keep a map from client paths to the leaf
  // node associated with that client. There is an entry in this map
  // for every leaf node in the client tree (except for the root when
  // the tree is empty). Paths in this map do NOT contain the trailing
  // "." label we use for leaf nodes.
  hashmap<std::string, Node*> clients;

  // Weights associated with role paths. Setting the weight for a path
  // influences the share of all nodes in the subtree rooted at that
  // path. This hashmap might include weights for paths that are not
  // currently in the sorter tree.
  hashmap<std::string, double> weights;

  // Total resources.
  struct Total
  {
    // We keep the aggregated scalar resource quantities to speed
    // up share calculation. Note, resources shared count are ignored.
    // Because sharedness inherently refers to the identities of resources
    // and not quantities.
    ResourceQuantities totals;

    // We keep track of per-agent resource quantities to handle agent removal.
    //
    // Note that the only way to change the stored resource quantities
    // is to remove the agent from the sorter and add it with new resources.
    // Thus, when a resource shared count on an agent changes, multiple copies
    // of the same shared resource are still accounted for exactly once.
    hashmap<SlaveID, const ResourceQuantities> agentResourceQuantities;
  } total_;

  // Metrics are optionally exposed by the sorter.
  friend Metrics;
  Option<Metrics> metrics;
};


// Represents a node in the sorter's tree. The structure of the tree
// reflects the hierarchical relationships between the clients of the
// sorter. Some (but not all) nodes correspond to sorter clients; some
// nodes only exist to represent the structure of the sorter
// tree. Clients are always associated with leaf nodes.
//
// For example, if there are two sorter clients "a/b" and "c/d", the
// tree will contain five nodes: the root node, internal nodes for "a"
// and "c", and leaf nodes for the clients "a/b" and "c/d".
struct DRFSorter::Node
{
  // Indicates whether a node is an active leaf node, an inactive leaf
  // node, or an internal node. Sorter clients always correspond to
  // leaf nodes, and only leaf nodes can be activated or deactivated.
  // The root node is always an "internal" node.
  enum Kind
  {
    ACTIVE_LEAF,
    INACTIVE_LEAF,
    INTERNAL
  };

  Node(const std::string& _name, Kind _kind, Node* _parent)
    : name(_name), share(0), kind(_kind), parent(_parent)
  {
    // Compute the node's path. Three cases:
    //
    //  (1) If the root node, use the empty string
    //  (2) If a child of the root node, use the child's name
    //  (3) Otherwise, use the parent's name, "/", and the child's name.
    if (parent == nullptr) {
      path = "";
    } else if (parent->parent == nullptr) {
      path = name;
    } else {
      path = strings::join("/", parent->path, name);
    }
  }

  ~Node()
  {
    foreach (Node* child, children) {
      delete child;
    }
  }

  // The label of the edge from this node's parent to the
  // node. "Implicit" leaf nodes are always named ".".
  //
  // TODO(neilc): Consider naming implicit leaf nodes in a clearer
  // way, e.g., by making `name` an Option?
  std::string name;

  // Complete path from root to node. This includes the trailing "."
  // label for virtual leaf nodes.
  std::string path;

  // NOTE: Not computed for root node.
  double share;

  // Cached weight of the node, access this through `getWeight()`.
  // The value is cached by `getWeight()` and updated by
  // `updateWeight()`. Marked mutable since the caching writes
  // to this are logically const.
  mutable Option<double> weight;

  Kind kind;

  Node* parent;

  // Pointers to the child nodes. `children` is only non-empty if
  // `kind` is INTERNAL_NODE. Two ordering invariants are maintained
  // on the `children` vector:
  //
  // (1) All inactive leaves are stored at the end of the vector; that
  // is, each `children` vector consists of zero or more active leaves
  // and internal nodes, followed by zero or more inactive leaves. This
  // means that code that only wants to iterate over active children
  // can stop when the first inactive leaf is observed.
  //
  // (2) If the tree is not dirty, the active leaves and internal
  // nodes are kept sorted by DRF share.
  std::vector<Node*> children;

  // If this node represents a sorter client, this returns the path of
  // that client. Unlike the `path` field, this does NOT include the
  // trailing "." label for virtual leaf nodes.
  //
  // For example, if the sorter contains two clients "a" and "a/b",
  // the tree will contain four nodes: the root node, "a", "a/."
  // (virtual leaf), and "a/b". The `clientPath()` of "a/." is "a",
  // because that is the name of the client associated with that
  // virtual leaf node.
  const std::string& clientPath() const
  {
    if (name == ".") {
      CHECK(kind == ACTIVE_LEAF || kind == INACTIVE_LEAF);
      return CHECK_NOTNULL(parent)->path;
    }

    return path;
  }

  bool isLeaf() const
  {
    if (kind == ACTIVE_LEAF || kind == INACTIVE_LEAF) {
      CHECK(children.empty());
      return true;
    }

    return false;
  }

  void removeChild(const Node* child)
  {
    // Sanity check: ensure we are removing an extant node.
    auto it = std::find(children.begin(), children.end(), child);
    CHECK(it != children.end());

    children.erase(it);
  }

  void addChild(Node* child)
  {
    // Sanity check: don't allow duplicates to be inserted.
    auto it = std::find(children.begin(), children.end(), child);
    CHECK(it == children.end());

    // If we're inserting an inactive leaf, place it at the end of the
    // `children` vector; otherwise, place it at the beginning. This
    // maintains ordering invariant (1) above. It is up to the caller
    // to maintain invariant (2) -- e.g., by marking the tree dirty.
    if (child->kind == INACTIVE_LEAF) {
      children.push_back(child);
    } else {
      children.insert(children.begin(), child);
    }
  }

  // Allocation for a node.
  struct Allocation
  {
    Allocation() : count(0) {}

    void add(const SlaveID& slaveId, const Resources& toAdd)
    {
      // Add shared resources to the allocated quantities when the same
      // resources don't already exist in the allocation.
      const Resources sharedToAdd = toAdd.shared()
        .filter([this, slaveId](const Resource& resource) {
            return !resources[slaveId].contains(resource);
        });

      const ResourceQuantities quantitiesToAdd =
        ResourceQuantities::fromScalarResources(
            (toAdd.nonShared() + sharedToAdd).scalars());

      resources[slaveId] += toAdd;
      totals += quantitiesToAdd;

      count++;
    }

    void subtract(const SlaveID& slaveId, const Resources& toRemove)
    {
      CHECK(resources.contains(slaveId))
        << "Resources " << resources << " does not contain " << slaveId;
      CHECK(resources.at(slaveId).contains(toRemove))
        << "Resources " << resources.at(slaveId) << " at agent " << slaveId
        << " does not contain " << toRemove;

      resources[slaveId] -= toRemove;

      // Remove shared resources from the allocated quantities when there
      // are no instances of same resources left in the allocation.
      const Resources sharedToRemove = toRemove.shared()
        .filter([this, slaveId](const Resource& resource) {
            return !resources[slaveId].contains(resource);
        });

      const ResourceQuantities quantitiesToRemove =
        ResourceQuantities::fromScalarResources(
            (toRemove.nonShared() + sharedToRemove).scalars());

      CHECK(totals.contains(quantitiesToRemove))
        << totals << " does not contain " << quantitiesToRemove;

      totals -= quantitiesToRemove;

      if (resources.at(slaveId).empty()) {
        resources.erase(slaveId);
      }
    }

    void update(
        const SlaveID& slaveId,
        const Resources& oldAllocation,
        const Resources& newAllocation)
    {
      const ResourceQuantities oldAllocationQuantities =
        ResourceQuantities::fromScalarResources(oldAllocation.scalars());
      const ResourceQuantities newAllocationQuantities =
        ResourceQuantities::fromScalarResources(newAllocation.scalars());

      CHECK(resources.contains(slaveId))
        << "Resources " << resources << " does not contain " << slaveId;
      CHECK(resources[slaveId].contains(oldAllocation))
        << "Resources " << resources[slaveId] << " at agent " << slaveId
        << " does not contain " << oldAllocation;

      CHECK(totals.contains(oldAllocationQuantities))
        << totals << " does not contain " << oldAllocationQuantities;

      resources[slaveId] -= oldAllocation;
      resources[slaveId] += newAllocation;

      // It is possible that allocations can be updated to empty.
      // See MESOS-9015 and MESOS-9975.
      if (resources.at(slaveId).empty()) {
        resources.erase(slaveId);
      }

      totals -= oldAllocationQuantities;
      totals += newAllocationQuantities;
    }

    // We store the number of times this client has been chosen for
    // allocation so that we can fairly share the resources across
    // clients that have the same share. Note that this information is
    // not persisted across master failovers, but since the point is
    // to equalize the `count` across clients of the same `share`
    // having allocations restart at 0 after a master failover should
    // be sufficient (famous last words.)
    uint64_t count;

    // We maintain multiple copies of each shared resource allocated
    // to a client, where the number of copies represents the number
    // of times this shared resource has been allocated to (and has
    // not been recovered from) a specific client.
    hashmap<SlaveID, Resources> resources;

    // We keep the aggregated scalar resource quantities to speed
    // up share calculation. Note, resources shared count are ignored.
    // Because sharedness inherently refers to the identities of resources
    // and not quantities.
    ResourceQuantities totals;
  } allocation;

  // Compares two nodes according to DRF share.
  static bool compareDRF(const Node* left, const Node* right)
  {
    if (left->share != right->share) {
      return left->share < right->share;
    }

    if (left->allocation.count != right->allocation.count) {
      return left->allocation.count < right->allocation.count;
    }

    return left->path < right->path;
  }
};

} // namespace allocator {
} // namespace master {
} // namespace internal {
} // namespace mesos {

#endif // __MASTER_ALLOCATOR_MESOS_SORTER_DRF_SORTER_HPP__
