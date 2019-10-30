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

#include "master/allocator/mesos/sorter/random/sorter.hpp"
#include "master/allocator/mesos/sorter/random/utils.hpp"
#include "master/constants.hpp"

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

using std::pair;
using std::set;
using std::string;
using std::vector;

using process::UPID;

namespace mesos {
namespace internal {
namespace master {
namespace allocator {


RandomSorter::RandomSorter()
  : sortInfo(this), root(new Node("", Node::INTERNAL, nullptr)) {}


RandomSorter::RandomSorter(
    const UPID& allocator,
    const string& metricsPrefix)
  : sortInfo(this), root(new Node("", Node::INTERNAL, nullptr)) {}


RandomSorter::~RandomSorter()
{
  delete root;
}


void RandomSorter::initialize(
    const Option<set<string>>& _fairnessExcludeResourceNames) {}


void RandomSorter::add(const string& clientPath)
{
  CHECK(!clients.contains(clientPath)) << clientPath;

  sortInfo.dirty = true;

  // Adding a client is a two phase algorithm:
  //
  //            root
  //          /  |  \       Three interesting cases:
  //         a   e   w        Add a                     (i.e. phase 1(a))
  //         |      / \       Add e/f, e/f/g, e/f/g/... (i.e. phase 1(b))
  //         b     .   z      Add w/x, w/x/y, w/x/y/... (i.e. phase 1(c))
  //
  //   Phase 1: Walk down the tree until:
  //     (a) we run out of tokens -> add "." node
  //     (b) or, we reach a leaf -> transform the leaf into internal + "."
  //     (c) or, we're at an internal node but can't find the next child
  //
  //   Phase 2: For any remaining tokens, walk down creating children:
  //     (a) if last token of the client path -> create INACTIVE_LEAF
  //     (b) else, create INTERNAL and keep going

  vector<string> tokens = strings::split(clientPath, "/");
  auto token = tokens.begin();

  // Traverse the tree to add new nodes for each element of the path,
  // if that node doesn't already exist (similar to `mkdir -p`).
  Node* current = root;

  // Phase 1:
  while (true) {
    // Case (a).
    if (token == tokens.end()) {
      Node* virt = new Node(".", Node::INACTIVE_LEAF, current);

      current->addChild(virt);
      current = virt;

      break;
    }

    // Case (b).
    if (current->isLeaf()) {
      Node::Kind oldKind = current->kind;

      current->parent->removeChild(current);
      current->kind = Node::INTERNAL;
      current->parent->addChild(current);

      Node* virt = new Node(".", oldKind, current);
      virt->allocation = current->allocation;

      current->addChild(virt);
      clients[virt->clientPath()] = virt;

      break;
    }

    Option<Node*> child = [&]() -> Option<Node*> {
      foreach (Node* c, current->children) {
        if (c->name == *token) return c;
      }
      return None();
    }();

    // Case (c).
    if (child.isNone()) break;

    current = *child;
    ++token;
  }

  // Phase 2:
  for (; token != tokens.end(); ++token) {
    Node::Kind kind = (token == tokens.end() - 1)
      ? Node::INACTIVE_LEAF : Node::INTERNAL;

    Node* child = new Node(*token, kind, current);

    current->addChild(child);
    current = child;
  }

  CHECK(current->children.empty());
  CHECK(current->kind == Node::INACTIVE_LEAF);

  CHECK_EQ(clientPath, current->clientPath());
  CHECK(!clients.contains(clientPath)) << clientPath;

  clients[clientPath] = current;
}


void RandomSorter::remove(const string& clientPath)
{
  sortInfo.dirty = true;

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
    // `parent`.
    foreachpair (const SlaveID& slaveId,
                 const Resources& resources,
                 leafAllocation) {
      parent->allocation.subtract(slaveId, resources);
    }

    if (current->children.empty()) {
      // Simply delete the current node if it has no children.
      parent->removeChild(current);
      delete current;
    } else if (current->children.size() == 1) {
      // If `current` has only one virtual node ".", we can collapse
      // and remove that node, and turn `current` back into a
      // leaf node.
      Node* child = *(current->children.begin());
      if (child->name == ".") {
        CHECK(child->isLeaf());
        CHECK(clients.contains(current->path));
        CHECK_EQ(child, clients.at(current->path));

        current->kind = child->kind;
        current->removeChild(child);
        delete child;

        // `current` has changed kind (from `INTERNAL` to a leaf,
        // which might be active or inactive). We need to change
        // its position in the `children` list.
        CHECK_NOTNULL(current->parent);
        current->parent->removeChild(current);
        current->parent->addChild(current);

        clients[current->path] = current;
      }
    }

    current = parent;
  }
}


void RandomSorter::activate(const string& clientPath)
{
  sortInfo.dirty = true;

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
  sortInfo.dirty = true;

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
  sortInfo.dirty = true;

  weights[path] = weight;

  // Update the weight of the corresponding internal node,
  // if it exists (this client may not exist despite there
  // being a weight).
  Node* node = find(path);

  if (node == nullptr) {
    return;
  }

  // If there is a virtual leaf, we need to move up one level.
  if (node->name == ".") {
    node = CHECK_NOTNULL(node->parent);
  }

  CHECK_EQ(path, node->path);

  node->weight = weight;
}


void RandomSorter::allocated(
    const string& clientPath,
    const SlaveID& slaveId,
    const Resources& resources)
{
  Node* current = CHECK_NOTNULL(find(clientPath));

  while (current != nullptr) {
    current->allocation.add(slaveId, resources);
    current = current->parent;
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

  while (current != nullptr) {
    current->allocation.update(slaveId, oldAllocation, newAllocation);
    current = current->parent;
  }
}


void RandomSorter::unallocated(
    const string& clientPath,
    const SlaveID& slaveId,
    const Resources& resources)
{
  Node* current = CHECK_NOTNULL(find(clientPath));

  while (current != nullptr) {
    current->allocation.subtract(slaveId, resources);
    current = current->parent;
  }
}


const hashmap<SlaveID, Resources>& RandomSorter::allocation(
    const string& clientPath) const
{
  const Node* client = CHECK_NOTNULL(find(clientPath));
  return client->allocation.resources;
}


const ResourceQuantities& RandomSorter::allocationScalarQuantities(
    const string& clientPath) const
{
  const Node* client = CHECK_NOTNULL(find(clientPath));
  return client->allocation.totals;
}


const ResourceQuantities& RandomSorter::allocationScalarQuantities() const
{
  return root->allocation.totals;
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


vector<string> RandomSorter::sort()
{
  pair<vector<string>, vector<double>> clientsAndWeights =
    sortInfo.getClientsAndWeights();

  weightedShuffle(
      clientsAndWeights.first.begin(),
      clientsAndWeights.first.end(),
      clientsAndWeights.second,
      generator);

  return std::move(clientsAndWeights.first);
}


bool RandomSorter::contains(const string& clientPath) const
{
  return find(clientPath) != nullptr;
}


size_t RandomSorter::count() const
{
  return clients.size();
}


double RandomSorter::getWeight(const Node* node) const
{
  if (node->weight.isNone()) {
    node->weight = weights.get(node->path).getOrElse(DEFAULT_WEIGHT);
  }

  return CHECK_NOTNONE(node->weight);
}


hashset<RandomSorter::Node*> RandomSorter::activeInternalNodes() const
{
  // Using post-order traversal to find all the internal nodes
  // that have at least one active leaf descendant and store
  // them in the input `result` hashset.
  //
  // Returns true if the subtree at the input `node` contains any
  // active leaf node.
  std::function<bool(Node*, hashset<Node*>&)> searchActiveInternal =
    [&searchActiveInternal](Node* node, hashset<Node*>& result) {
      switch (node->kind) {
        case Node::ACTIVE_LEAF: return true;

        case Node::INACTIVE_LEAF: return false;

        case Node::INTERNAL: {
          bool active = false;
          foreach (Node* child, node->children) {
            if (searchActiveInternal(child, result)) {
              active = true;
            }
          }

          if (active) {
            result.insert(node);
          }
          return active;
        }
      }
      UNREACHABLE();
    };

  hashset<Node*> result;
  searchActiveInternal(root, result);

  return result;
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


pair<vector<string>, vector<double>>
RandomSorter::SortInfo::getClientsAndWeights()
{
  updateRelativeWeights();

  return std::make_pair(clients, weights);
}


void RandomSorter::SortInfo::updateRelativeWeights()
{
  if (!dirty) {
    return;
  }

  hashset<Node*> activeInternalNodes = sorter->activeInternalNodes();

  auto isActive = [&activeInternalNodes](Node* node) {
    return node->kind == Node::ACTIVE_LEAF ||
           activeInternalNodes.contains(node);
  };

  clients.clear();
  weights.clear();

  // Note, though we reserve here, the size of the vector will always
  // grow (as we add more roles).
  clients.reserve(sorter->clients.size());
  weights.reserve(sorter->clients.size());

  // We use the following formula to compute the relative weights (Rw):
  //
  //                                    weight(node)
  // Rw(node) = Rw(parent) * -------------------------------------------
  //                         weight(node) + SUM(weight(active siblings))
  //
  // Using pre-order traversal to calculate node's relative weight.
  // Active leaves and their relative weights are stored in `clients`
  // and `weights`.
  std::function<void(Node*, double, double)> calculateRelativeWeights =
    [&](Node* node, double siblingWeights, double parentRelativeWeight) {
      // We only calculate relative weights for active nodes.
      if (!isActive(node)) {
        return;
      }
      double relativeWeight = parentRelativeWeight * sorter->getWeight(node) /
                              (sorter->getWeight(node) + siblingWeights);

      // Store the result for active leaves.
      if (node->kind == Node::ACTIVE_LEAF) {
        clients.push_back(node->clientPath());
        weights.push_back(relativeWeight);
      }

      // Calculate active children's total weights.
      double totalWeights_ = 0.0;

      foreach (Node* child, node->children) {
        totalWeights_ += isActive(child) ? sorter->getWeight(child) : 0.0;
      }

      foreach (Node* child, node->children) {
        if (isActive(child)) {
          calculateRelativeWeights(
              child, totalWeights_ - sorter->getWeight(child), relativeWeight);
        }
      }
    };

  calculateRelativeWeights(sorter->root, 0.0, 1.0);

  dirty = false;
}

} // namespace allocator {
} // namespace master {
} // namespace internal {
} // namespace mesos {
