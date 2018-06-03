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

#include "master/allocator/sorter/random/sorter.hpp"
#include "master/allocator/sorter/random/utils.hpp"

#include <set>
#include <string>
#include <vector>

#include <mesos/mesos.hpp>
#include <mesos/resources.hpp>
#include <mesos/values.hpp>

#include <process/pid.hpp>

#include <stout/check.hpp>
#include <stout/foreach.hpp>
#include <stout/hashmap.hpp>
#include <stout/option.hpp>
#include <stout/strings.hpp>

using std::set;
using std::string;
using std::vector;

using process::UPID;

namespace mesos {
namespace internal {
namespace master {
namespace allocator {


RandomSorter::RandomSorter()
  : root(new Node("", Node::INTERNAL, nullptr)) {}


RandomSorter::RandomSorter(
    const UPID& allocator,
    const string& metricsPrefix)
  : root(new Node("", Node::INTERNAL, nullptr)) {}


RandomSorter::~RandomSorter()
{
  delete root;
}


void RandomSorter::initialize(
    const Option<set<string>>& _fairnessExcludeResourceNames) {}


void RandomSorter::add(const string& clientPath)
{
  vector<string> pathElements = strings::tokenize(clientPath, "/");
  CHECK(!pathElements.empty());

  Node* current = root;
  Node* lastCreatedNode = nullptr;

  // Traverse the tree to add new nodes for each element of the path,
  // if that node doesn't already exist (similar to `mkdir -p`).
  foreach (const string& element, pathElements) {
    Node* node = nullptr;

    foreach (Node* child, current->children) {
      if (child->name == element) {
        node = child;
        break;
      }
    }

    if (node != nullptr) {
      current = node;
      continue;
    }

    // We didn't find `element`, so add a new child to `current`.
    //
    // If adding this child would result in turning `current` from a
    // leaf node into an internal node, we need to create an
    // additional child node: `current` must have been associated with
    // a client and clients must always be associated with leaf nodes.
    if (current->isLeaf()) {
      Node* parent = CHECK_NOTNULL(current->parent);

      parent->removeChild(current);

      // Create a node under `parent`. This internal node will take
      // the place of `current` in the tree.
      Node* internal = new Node(current->name, Node::INTERNAL, parent);
      parent->addChild(internal);
      internal->allocation = current->allocation;

      CHECK_EQ(current->path, internal->path);

      // Update `current` to become a virtual leaf node and a child of
      // `internal`.
      current->name = ".";
      current->parent = internal;
      current->path = strings::join("/", parent->path, current->name);

      internal->addChild(current);

      CHECK_EQ(internal->path, current->clientPath());

      current = internal;
    }

    // Now actually add a new child to `current`.
    Node* newChild = new Node(element, Node::INTERNAL, current);
    current->addChild(newChild);

    current = newChild;
    lastCreatedNode = newChild;
  }

  CHECK(current->kind == Node::INTERNAL);

  // `current` is the node associated with the last element of the
  // path. If we didn't add `current` to the tree above, create a leaf
  // node now. For example, if the tree contains "a/b" and we add a
  // new client "a", we want to create a new leaf node "a/." here.
  if (current != lastCreatedNode) {
    Node* newChild = new Node(".", Node::INACTIVE_LEAF, current);
    current->addChild(newChild);
    current = newChild;
  } else {
    // If we created `current` in the loop above, it was marked an
    // `INTERNAL` node. It should actually be an inactive leaf node.
    current->kind = Node::INACTIVE_LEAF;

    // `current` has changed from an internal node to an inactive
    // leaf, so remove and re-add it to its parent. This moves it to
    // the end of the parent's list of children.
    CHECK_NOTNULL(current->parent);

    current->parent->removeChild(current);
    current->parent->addChild(current);
  }

  // `current` is the newly created node associated with the last
  // element of the path. `current` should be an inactive leaf node.
  CHECK(current->children.empty());
  CHECK(current->kind == Node::INACTIVE_LEAF);

  // Add a new entry to the lookup table. The full path of the newly
  // added client should not already exist in `clients`.
  CHECK_EQ(clientPath, current->clientPath());
  CHECK(!clients.contains(clientPath));

  clients[clientPath] = current;
}


void RandomSorter::remove(const string& clientPath)
{
  Node* current = CHECK_NOTNULL(find(clientPath));

  // Save a copy of the leaf node's allocated resources, because we
  // destroy the leaf node below.
  const hashmap<SlaveID, Resources> leafAllocation =
    current->allocation.resources;

  // Remove the lookup table entry for the client.
  CHECK(clients.contains(clientPath));
  clients.erase(clientPath);

  // To remove a client from the tree, we have to do two things:
  //
  //   (1) Update the tree structure to reflect the removal of the
  //       client. This means removing the client's leaf node, then
  //       walking back up the tree to remove any internal nodes that
  //       are now unnecessary.
  //
  //   (2) Update allocations of ancestor nodes to reflect the removal
  //       of the client.
  //
  // We do both things at once: find the leaf node, remove it, and
  // walk up the tree, updating ancestor allocations and removing
  // ancestors when possible.
  while (current != root) {
    Node* parent = CHECK_NOTNULL(current->parent);

    // Update `parent` to reflect the fact that the resources in the
    // leaf node are no longer allocated to the subtree rooted at
    // `parent`. We skip `root`, because we never update the
    // allocation made to the root node.
    if (parent != root) {
      foreachpair (const SlaveID& slaveId,
                   const Resources& resources,
                   leafAllocation) {
        parent->allocation.subtract(slaveId, resources);
      }
    }

    if (current->children.empty()) {
      parent->removeChild(current);
      delete current;
    } else if (current->children.size() == 1) {
      // If `current` has only one child that was created to
      // accommodate inserting `clientPath` (see `RandomSorter::add()`),
      // we can remove the child node and turn `current` back into a
      // leaf node.
      Node* child = *(current->children.begin());

      if (child->name == ".") {
        CHECK(child->isLeaf());
        CHECK(clients.contains(current->path));
        CHECK_EQ(child, clients.at(current->path));

        current->kind = child->kind;
        current->removeChild(child);

        // `current` has changed kind (from `INTERNAL` to a leaf,
        // which might be active or inactive). Hence we might need to
        // change its position in the `children` list.
        if (current->kind == Node::INTERNAL) {
          CHECK_NOTNULL(current->parent);

          current->parent->removeChild(current);
          current->parent->addChild(current);
        }

        clients[current->path] = current;

        delete child;
      }
    }

    current = parent;
  }
}


void RandomSorter::activate(const string& clientPath)
{
  Node* client = CHECK_NOTNULL(find(clientPath));

  if (client->kind == Node::INACTIVE_LEAF) {
    client->kind = Node::ACTIVE_LEAF;

    // `client` has been activated, so move it to the beginning of its
    // parent's list of children.
    CHECK_NOTNULL(client->parent);

    client->parent->removeChild(client);
    client->parent->addChild(client);
  }
}


void RandomSorter::deactivate(const string& clientPath)
{
  Node* client = CHECK_NOTNULL(find(clientPath));

  if (client->kind == Node::ACTIVE_LEAF) {
    client->kind = Node::INACTIVE_LEAF;

    // `client` has been deactivated, so move it to the end of its
    // parent's list of children.
    CHECK_NOTNULL(client->parent);

    client->parent->removeChild(client);
    client->parent->addChild(client);
  }
}


void RandomSorter::updateWeight(const string& path, double weight)
{
  weights[path] = weight;
}


void RandomSorter::allocated(
    const string& clientPath,
    const SlaveID& slaveId,
    const Resources& resources)
{
  Node* current = CHECK_NOTNULL(find(clientPath));

  // NOTE: We don't currently update the `allocation` for the root
  // node. This is debatable, but the current implementation doesn't
  // require looking at the allocation of the root node.
  while (current != root) {
    current->allocation.add(slaveId, resources);
    current = CHECK_NOTNULL(current->parent);
  }
}


void RandomSorter::update(
    const string& clientPath,
    const SlaveID& slaveId,
    const Resources& oldAllocation,
    const Resources& newAllocation)
{
  // TODO(bmahler): Check invariants between old and new allocations.
  // Namely, the roles and quantities of resources should be the same!

  Node* current = CHECK_NOTNULL(find(clientPath));

  // NOTE: We don't currently update the `allocation` for the root
  // node. This is debatable, but the current implementation doesn't
  // require looking at the allocation of the root node.
  while (current != root) {
    current->allocation.update(slaveId, oldAllocation, newAllocation);
    current = CHECK_NOTNULL(current->parent);
  }
}


void RandomSorter::unallocated(
    const string& clientPath,
    const SlaveID& slaveId,
    const Resources& resources)
{
  Node* current = CHECK_NOTNULL(find(clientPath));

  // NOTE: We don't currently update the `allocation` for the root
  // node. This is debatable, but the current implementation doesn't
  // require looking at the allocation of the root node.
  while (current != root) {
    current->allocation.subtract(slaveId, resources);
    current = CHECK_NOTNULL(current->parent);
  }
}


const hashmap<SlaveID, Resources>& RandomSorter::allocation(
    const string& clientPath) const
{
  const Node* client = CHECK_NOTNULL(find(clientPath));
  return client->allocation.resources;
}


const Resources& RandomSorter::allocationScalarQuantities(
    const string& clientPath) const
{
  const Node* client = CHECK_NOTNULL(find(clientPath));
  return client->allocation.scalarQuantities;
}


hashmap<string, Resources> RandomSorter::allocation(
    const SlaveID& slaveId) const
{
  hashmap<string, Resources> result;

  // We want to find the allocation that has been made to each client
  // on a particular `slaveId`. Rather than traversing the tree
  // looking for leaf nodes (clients), we can instead just iterate
  // over the `clients` hashmap.
  //
  // TODO(jmlvanre): We can index the allocation by slaveId to make
  // this faster.  It is a tradeoff between speed vs. memory. For now
  // we use existing data structures.
  foreachvalue (const Node* client, clients) {
    if (client->allocation.resources.contains(slaveId)) {
      // It is safe to use `at()` here because we've just checked the
      // existence of the key. This avoids unnecessary copies.
      string path = client->clientPath();
      CHECK(!result.contains(path));
      result.emplace(path, client->allocation.resources.at(slaveId));
    }
  }

  return result;
}


Resources RandomSorter::allocation(
    const string& clientPath,
    const SlaveID& slaveId) const
{
  const Node* client = CHECK_NOTNULL(find(clientPath));

  if (client->allocation.resources.contains(slaveId)) {
    return client->allocation.resources.at(slaveId);
  }

  return Resources();
}


const Resources& RandomSorter::totalScalarQuantities() const
{
  return total_.scalarQuantities;
}


void RandomSorter::add(const SlaveID& slaveId, const Resources& resources)
{
  if (!resources.empty()) {
    // Add shared resources to the total quantities when the same
    // resources don't already exist in the total.
    const Resources newShared = resources.shared()
      .filter([this, slaveId](const Resource& resource) {
        return !total_.resources[slaveId].contains(resource);
      });

    total_.resources[slaveId] += resources;

    const Resources scalarQuantities =
      (resources.nonShared() + newShared).createStrippedScalarQuantity();

    total_.scalarQuantities += scalarQuantities;

    foreach (const Resource& resource, scalarQuantities) {
      total_.totals[resource.name()] += resource.scalar();
    }
  }
}


void RandomSorter::remove(const SlaveID& slaveId, const Resources& resources)
{
  if (!resources.empty()) {
    CHECK(total_.resources.contains(slaveId));
    CHECK(total_.resources[slaveId].contains(resources))
      << total_.resources[slaveId] << " does not contain " << resources;

    total_.resources[slaveId] -= resources;

    // Remove shared resources from the total quantities when there
    // are no instances of same resources left in the total.
    const Resources absentShared = resources.shared()
      .filter([this, slaveId](const Resource& resource) {
        return !total_.resources[slaveId].contains(resource);
      });

    const Resources scalarQuantities =
      (resources.nonShared() + absentShared).createStrippedScalarQuantity();

    foreach (const Resource& resource, scalarQuantities) {
      total_.totals[resource.name()] -= resource.scalar();
    }

    CHECK(total_.scalarQuantities.contains(scalarQuantities));
    total_.scalarQuantities -= scalarQuantities;

    if (total_.resources[slaveId].empty()) {
      total_.resources.erase(slaveId);
    }
  }
}


vector<string> RandomSorter::sort()
{
  std::function<void (Node*)> shuffleTree = [this, &shuffleTree](Node* node) {
    // Inactive leaves are always stored at the end of the
    // `children` vector; this means that we should only shuffle
    // the prefix of the vector before the first inactive leaf.
    auto inactiveBegin = std::find_if(
        node->children.begin(),
        node->children.end(),
        [](Node* n) { return n->kind == Node::INACTIVE_LEAF; });

    vector<double> weights(inactiveBegin - node->children.begin());

    for (int i = 0; i < inactiveBegin - node->children.begin(); ++i) {
      weights[i] = findWeight(node->children[i]);
    }

    weightedShuffle(node->children.begin(), inactiveBegin, weights, generator);

    foreach (Node* child, node->children) {
      if (child->kind == Node::INTERNAL) {
        shuffleTree(child);
      } else if (child->kind == Node::INACTIVE_LEAF) {
        break;
      }
    }
  };

  shuffleTree(root);

  // Return all active leaves in the tree via pre-order traversal.
  // The children of each node are already shuffled, with
  // inactive leaves stored after active leaves and internal nodes.
  vector<string> result;
  result.reserve(clients.size());

  std::function<void (const Node*)> listClients =
      [&listClients, &result](const Node* node) {
    foreach (const Node* child, node->children) {
      switch (child->kind) {
        case Node::ACTIVE_LEAF:
          result.push_back(child->clientPath());
          break;

        case Node::INACTIVE_LEAF:
          // As soon as we see the first inactive leaf, we can stop
          // iterating over the current node's list of children.
          return;

        case Node::INTERNAL:
          listClients(child);
          break;
      }
    }
  };

  listClients(root);

  return result;
}


bool RandomSorter::contains(const string& clientPath) const
{
  return find(clientPath) != nullptr;
}


size_t RandomSorter::count() const
{
  return clients.size();
}


double RandomSorter::findWeight(const Node* node) const
{
  Option<double> weight = weights.get(node->path);

  if (weight.isNone()) {
    return 1.0;
  }

  return weight.get();
}


RandomSorter::Node* RandomSorter::find(const string& clientPath) const
{
  Option<Node*> client_ = clients.get(clientPath);

  if (client_.isNone()) {
    return nullptr;
  }

  Node* client = client_.get();

  CHECK(client->isLeaf());

  return client;
}

} // namespace allocator {
} // namespace master {
} // namespace internal {
} // namespace mesos {
