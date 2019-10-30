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

#include "master/allocator/mesos/sorter/drf/sorter.hpp"
#include "master/constants.hpp"

#include <iterator>
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


DRFSorter::DRFSorter()
  : root(new Node("", Node::INTERNAL, nullptr)) {}


DRFSorter::DRFSorter(
    const UPID& allocator,
    const string& metricsPrefix)
  : root(new Node("", Node::INTERNAL, nullptr)),
    metrics(Metrics(allocator, *this, metricsPrefix)) {}


DRFSorter::~DRFSorter()
{
  delete root;
}


void DRFSorter::initialize(
    const Option<set<string>>& _fairnessExcludeResourceNames)
{
  fairnessExcludeResourceNames = _fairnessExcludeResourceNames;
}


void DRFSorter::add(const string& clientPath)
{
  CHECK(!clients.contains(clientPath)) << clientPath;

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

  // TODO(neilc): Avoid dirtying the tree in some circumstances.
  dirty = true;

  if (metrics.isSome()) {
    metrics->add(clientPath);
  }
}


void DRFSorter::remove(const string& clientPath)
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

  // TODO(neilc): Avoid dirtying the tree in some circumstances.
  dirty = true;

  if (metrics.isSome()) {
    metrics->remove(clientPath);
  }
}


void DRFSorter::activate(const string& clientPath)
{
  Node* client = CHECK_NOTNULL(find(clientPath));

  if (client->kind == Node::INACTIVE_LEAF) {
    client->kind = Node::ACTIVE_LEAF;

    // `client` has been activated, so move it to the beginning of its
    // parent's list of children. We mark the tree dirty, so that the
    // client's share is updated correctly and it is sorted properly.
    //
    // TODO(neilc): We could instead calculate share here and insert
    // the client into the appropriate place here, which would avoid
    // dirtying the whole tree.
    CHECK_NOTNULL(client->parent);

    client->parent->removeChild(client);
    client->parent->addChild(client);

    dirty = true;
  }
}


void DRFSorter::deactivate(const string& clientPath)
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


void DRFSorter::updateWeight(const string& path, double weight)
{
  weights[path] = weight;

  // TODO(neilc): Avoid dirtying the tree in some circumstances.
  dirty = true;

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


void DRFSorter::allocated(
    const string& clientPath,
    const SlaveID& slaveId,
    const Resources& resources)
{
  Node* current = CHECK_NOTNULL(find(clientPath));

  // Walk up the tree adjusting allocations. If the tree is
  // sorted, we keep it sorted (see below).
  while (current != nullptr) {
    current->allocation.add(slaveId, resources);

    // Note that inactive leaves are not sorted, and are always
    // stored in `children` after the active leaves and internal
    // nodes. See the comment on `Node::children`.
    if (current != root && !dirty && current->kind != Node::INACTIVE_LEAF) {
      current->share = calculateShare(current);

      vector<Node*>& children = current->parent->children;

      // Locate the node position in the parent's children
      // and shift it into its sorted position.
      //
      // TODO(bmahler): Consider storing the node's position
      // in the parent's children to avoid scanning.
      auto position = std::find(children.begin(), children.end(), current);
      CHECK(position != children.end());

      // Shift left until done (if needed).
      while (position != children.begin() &&
             DRFSorter::Node::compareDRF(current, *std::prev(position))) {
        std::swap(*position, *std::prev(position));
        --position;
      }

      // Or, shift right until done (if needed). Note that when
      // shifting right, we need to stop once we reach the
      // inactive leaves (see `Node::children`).
      while (std::next(position) != children.end() &&
             (*std::next(position))->kind != Node::INACTIVE_LEAF &&
             DRFSorter::Node::compareDRF(*std::next(position), current)) {
        std::swap(*position, *std::next(position));
        ++position;
      }
    }

    current = current->parent;
  }
}


void DRFSorter::update(
    const string& clientPath,
    const SlaveID& slaveId,
    const Resources& oldAllocation,
    const Resources& newAllocation)
{
  // TODO(bmahler): Check if the quantities of resources between the old and new
  // allocations are the same. If not, we need to re-calculate the shares, as is
  // being currently done, for safety.

  Node* current = CHECK_NOTNULL(find(clientPath));

  while (current != nullptr) {
    current->allocation.update(slaveId, oldAllocation, newAllocation);
    current = current->parent;
  }

  // Just assume the total has changed, per the TODO above.
  dirty = true;
}


void DRFSorter::unallocated(
    const string& clientPath,
    const SlaveID& slaveId,
    const Resources& resources)
{
  Node* current = CHECK_NOTNULL(find(clientPath));

  while (current != nullptr) {
    current->allocation.subtract(slaveId, resources);
    current = current->parent;
  }

  // TODO(bmahler): Similar to `allocated()`, avoid dirtying the
  // tree here in favor of adjusting the node positions. This
  // hasn't been critical to do since we don't allocate upon
  // resource recovery in the allocator.
  dirty = true;
}


const hashmap<SlaveID, Resources>& DRFSorter::allocation(
    const string& clientPath) const
{
  const Node* client = CHECK_NOTNULL(find(clientPath));
  return client->allocation.resources;
}


const ResourceQuantities& DRFSorter::allocationScalarQuantities(
    const string& clientPath) const
{
  const Node* client = CHECK_NOTNULL(find(clientPath));
  return client->allocation.totals;
}


const ResourceQuantities& DRFSorter::allocationScalarQuantities() const
{
  return root->allocation.totals;
}


Resources DRFSorter::allocation(
    const string& clientPath,
    const SlaveID& slaveId) const
{
  const Node* client = CHECK_NOTNULL(find(clientPath));

  if (client->allocation.resources.contains(slaveId)) {
    return client->allocation.resources.at(slaveId);
  }

  return Resources();
}


void DRFSorter::addSlave(
    const SlaveID& slaveId,
    const ResourceQuantities& scalarQuantities)
{
  bool inserted = total_.agentResourceQuantities.emplace(
      slaveId, scalarQuantities).second;

  CHECK(inserted) << "Attempted to add already added agent " << slaveId;

  total_.totals += scalarQuantities;

  // We have to recalculate all shares when the total resources
  // change, but we put it off until `sort` is called so that if
  // something else changes before the next allocation we don't
  // recalculate everything twice.
  dirty = true;
}


void DRFSorter::removeSlave(const SlaveID& slaveId)
{
  const auto agent = total_.agentResourceQuantities.find(slaveId);
  CHECK(agent != total_.agentResourceQuantities.end())
    << "Attempted to remove unknown agent " << slaveId;

  CHECK_CONTAINS(total_.totals, agent->second);
  total_.totals -= agent->second;

  total_.agentResourceQuantities.erase(agent);
  dirty = true;
}


vector<string> DRFSorter::sort()
{
  if (dirty) {
    std::function<void (Node*)> sortTree = [this, &sortTree](Node* node) {
      // Inactive leaves are always stored at the end of the
      // `children` vector; this means that as soon as we see an
      // inactive leaf, we can stop calculating shares, and we only
      // need to sort the prefix of the vector before that point.
      auto childIter = node->children.begin();

      while (childIter != node->children.end()) {
        Node* child = *childIter;

        if (child->kind == Node::INACTIVE_LEAF) {
          break;
        }

        child->share = calculateShare(child);
        ++childIter;
      }

      std::sort(node->children.begin(),
                childIter,
                DRFSorter::Node::compareDRF);

      foreach (Node* child, node->children) {
        if (child->kind == Node::INTERNAL) {
          sortTree(child);
        } else if (child->kind == Node::INACTIVE_LEAF) {
          break;
        }
      }
    };

    sortTree(root);

    dirty = false;
  }

  // Return all active leaves in the tree via pre-order traversal.
  // The children of each node are already sorted in DRF order, with
  // inactive leaves sorted after active leaves and internal nodes.
  vector<string> result;

  // TODO(bmahler): This over-reserves where there are inactive
  // clients, only reserve the number of active clients.
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


bool DRFSorter::contains(const string& clientPath) const
{
  return find(clientPath) != nullptr;
}


size_t DRFSorter::count() const
{
  return clients.size();
}


double DRFSorter::calculateShare(const Node* node) const
{
  double share = 0.0;

  // TODO(benh): This implementation of "dominant resource fairness"
  // currently does not take into account resources that are not
  // scalars.

  foreachpair (const string& resourceName,
               const Value::Scalar& scalar,
               total_.totals) {
    // Filter out the resources excluded from fair sharing.
    if (fairnessExcludeResourceNames.isSome() &&
        fairnessExcludeResourceNames->count(resourceName) > 0) {
      continue;
    }

    if (scalar.value() <= 0.0) {
      continue;
    }

    Value::Scalar allocation = node->allocation.totals.get(resourceName);

    share = std::max(share, allocation.value() / scalar.value());
  }

  return share / getWeight(node);
}


double DRFSorter::getWeight(const Node* node) const
{
  if (node->weight.isNone()) {
    node->weight = weights.get(node->path).getOrElse(DEFAULT_WEIGHT);
  }

  return CHECK_NOTNONE(node->weight);
}


DRFSorter::Node* DRFSorter::find(const string& clientPath) const
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
