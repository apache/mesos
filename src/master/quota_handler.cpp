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
// limitations under the License

#include "master/master.hpp"

#include <memory>
#include <list>
#include <vector>

#include <mesos/resources.hpp>

#include <mesos/authorizer/authorizer.hpp>

#include <mesos/quota/quota.hpp>

#include <process/collect.hpp>
#include <process/defer.hpp>
#include <process/future.hpp>
#include <process/http.hpp>
#include <process/owned.hpp>

#include <stout/json.hpp>
#include <stout/protobuf.hpp>
#include <stout/stringify.hpp>
#include <stout/strings.hpp>
#include <stout/utils.hpp>

#include "logging/logging.hpp"

#include "master/quota.hpp"
#include "master/registrar.hpp"

namespace http = process::http;

using http::Accepted;
using http::BadRequest;
using http::Conflict;
using http::Forbidden;
using http::OK;

using mesos::authorization::createSubject;

using mesos::quota::QuotaInfo;
using mesos::quota::QuotaRequest;
using mesos::quota::QuotaStatus;

using process::Future;
using process::Owned;

using process::http::authentication::Principal;

using std::list;
using std::string;
using std::unique_ptr;
using std::vector;

namespace mesos {
namespace internal {
namespace master {

// Represents the tree of roles that have quota. The quota of a child
// node is "contained" in the quota of its parent node. This has two
// implications:
//
//   (1) The quota of a parent must be greater than or equal to the
//       sum of the quota of its children.
//
//   (2) When computing the total resources guaranteed by quota, we
//       don't want to double-count resource guarantees between a
//       parent role and its children.
class QuotaTree
{
public:
  QuotaTree(const hashmap<string, Quota>& quotas)
    : root(new Node(""))
  {
    foreachpair (const string& role, const Quota& quota, quotas) {
      insert(role, quota);
    }
  }

  void insert(const string& role, const Quota& quota)
  {
    // Create the path from root->leaf in the tree. Any missing nodes
    // are created implicitly.
    vector<string> components = strings::tokenize(role, "/");
    CHECK(!components.empty());

    Node* current = root.get();
    foreach (const string& component, components) {
      if (!current->children.contains(component)) {
        current->children[component] = unique_ptr<Node>(new Node(component));
      }

      current = current->children.at(component).get();
    }

    // Update `current` with the guaranteed quota resources for this
    // role. A path in the tree should be associated with at most one
    // quota guarantee, so the current guarantee should be empty.
    CHECK(current->quota.info.guarantee().empty());
    current->quota = quota;
  }

  // Check whether the tree satisfies the "parent >= sum(children)"
  // constraint described above.
  Option<Error> validate() const
  {
    // Don't check the root node because it does not have quota set.
    foreachvalue (const unique_ptr<Node>& child, root->children) {
      Option<Error> error = child->validate();
      if (error.isSome()) {
        return error;
      }
    }

    return None();
  }

  // Returns the total resources requested by all quotas in the
  // tree. Since a role's quota must be greater than or equal to the
  // sum of the quota of its children, we can just sum the quota of
  // the top-level roles.
  Resources total() const
  {
    Resources result;

    // Don't include the root node because it does not have quota set.
    foreachvalue (const unique_ptr<Node>& child, root->children) {
      result += child->quota.info.guarantee();
    }

    return result;
  }

private:
  struct Node
  {
    Node(const string& _name) : name(_name) {}

    Option<Error> validate() const
    {
      foreachvalue (const unique_ptr<Node>& child, children) {
        Option<Error> error = child->validate();
        if (error.isSome()) {
          return error;
        }
      }

      Resources childResources;
      foreachvalue (const unique_ptr<Node>& child, children) {
        childResources += child->quota.info.guarantee();
      }

      Resources selfResources = quota.info.guarantee();

      if (!selfResources.contains(childResources)) {
        return Error("Invalid quota configuration. Parent role '" +
                     name + "' with quota " + stringify(selfResources) +
                     " does not contain the sum of its children's" +
                     " resources (" + stringify(childResources) + ")");
      }

      return None();
    }

    const string name;
    Quota quota;
    hashmap<string, unique_ptr<Node>> children;
  };

  unique_ptr<Node> root;
};


Option<Error> Master::QuotaHandler::capacityHeuristic(
    const QuotaInfo& request) const
{
  VLOG(1) << "Performing capacity heuristic check for a set quota request";

  // This should have been validated earlier.
  CHECK(master->isWhitelistedRole(request.role()));
  CHECK(!master->quotas.contains(request.role()));

  hashmap<string, Quota> quotaMap = master->quotas;

  // Check that adding the requested quota to the existing quotas does
  // not violate the capacity heuristic.
  quotaMap[request.role()] = Quota{request};

  QuotaTree quotaTree(quotaMap);

  CHECK_NONE(quotaTree.validate());

  Resources totalQuota = quotaTree.total();

  // Determine whether the total quota, including the new request, does
  // not exceed the sum of non-static cluster resources.
  //
  // NOTE: We do not necessarily calculate the full sum of non-static
  // cluster resources. We apply the early termination logic as it can
  // reduce the cost of the function significantly. This early exit does
  // not influence the declared inequality check.
  Resources nonStaticClusterResources;
  foreachvalue (Slave* slave, master->slaves.registered) {
    // We do not consider disconnected or inactive agents, because they
    // do not participate in resource allocation.
    if (!slave->connected || !slave->active) {
      continue;
    }

    // NOTE: Dynamic reservations are not excluded here because they do
    // not show up in `SlaveInfo` resources. In contrast to static
    // reservations, dynamic reservations may be unreserved at any time,
    // hence making resources available for quota'ed frameworks.
    Resources nonStaticAgentResources =
      Resources(slave->info.resources()).unreserved();

    nonStaticClusterResources += nonStaticAgentResources;

    // If we have found enough resources to satisfy the inequality, then
    // we can return early.
    if (nonStaticClusterResources.contains(totalQuota)) {
      return None();
    }
  }

  // If we reached this point, there are not enough available resources
  // in the cluster, hence the request does not pass the heuristic.
  return Error(
      "Not enough available cluster capacity to reasonably satisfy quota "
      "request; the force flag can be used to override this check");
}


void Master::QuotaHandler::rescindOffers(const QuotaInfo& request) const
{
  const string& role = request.role();

  // This should have been validated earlier.
  CHECK(master->isWhitelistedRole(role));

  int frameworksInRole = 0;
  if (master->roles.contains(role)) {
    Role* roleState = master->roles.at(role);
    foreachvalue (const Framework* framework, roleState->frameworks) {
      if (framework->active()) {
        ++frameworksInRole;
      }
    }
  }

  // The resources recovered by rescinding outstanding offers.
  Resources rescinded;

  int visitedAgents = 0;

  // Because resources are allocated in the allocator, there can be a race
  // between rescinding and allocating. This race makes it hard to determine
  // the exact amount of offers that should be rescinded in the master.
  //
  // We pessimistically assume that what seems like "available" resources
  // in the allocator will be gone. We greedily rescind all offers from an
  // agent at once until we have rescinded "enough" offers. Offers containing
  // resources irrelevant to the quota request may be rescinded, as we
  // rescind all offers on an agent. This is done to maintain the
  // coarse-grained nature of agent offers, and helps reduce fragmentation of
  // offers.
  //
  // Consider a quota request for role `role` for `requested` resources.
  // There are `numFiR` frameworks in `role`. Let `rescinded` be the total
  // number of rescinded resources and `numVA` be the number of visited
  // agents, from which at least one offer has been rescinded. Then the
  // algorithm can be summarized as follows:
  //
  //   while (there are agents with outstanding offers) do:
  //     if ((`rescinded` contains `requested`) && (`numVA` >= `numFiR`) break;
  //     fetch an agent `a` with outstanding offers;
  //     rescind all outstanding offers from `a`;
  //     update `rescinded`, inc(numVA);
  //   end.
  foreachvalue (const Slave* slave, master->slaves.registered) {
    // If we have rescinded offers with at least as many resources as the
    // quota request resources, then we are done.
    if (rescinded.contains(request.guarantee()) &&
        (visitedAgents >= frameworksInRole)) {
      break;
    }

    // As in the capacity heuristic, we do not consider disconnected or
    // inactive agents, because they do not participate in resource
    // allocation.
    if (!slave->connected || !slave->active) {
      continue;
    }

    // TODO(alexr): Consider only rescinding from agents that have at least
    // one resource relevant to the quota request.

    // Rescind all outstanding offers from the given agent.
    bool agentVisited = false;
    foreach (Offer* offer, utils::copy(slave->offers)) {
      master->allocator->recoverResources(
          offer->framework_id(), offer->slave_id(), offer->resources(), None());

      auto unallocated = [](const Resources& resources) {
        Resources result = resources;
        result.unallocate();
        return result;
      };

      rescinded += unallocated(offer->resources());
      master->removeOffer(offer, true);
      agentVisited = true;
    }

    if (agentVisited) {
      ++visitedAgents;
    }
  }
}


Future<http::Response> Master::QuotaHandler::status(
    const mesos::master::Call& call,
    const Option<Principal>& principal,
    ContentType contentType) const
{
  CHECK_EQ(mesos::master::Call::GET_QUOTA, call.type());

  return _status(principal)
    .then([contentType](const QuotaStatus& status) -> Future<http::Response> {
      mesos::master::Response response;
      response.set_type(mesos::master::Response::GET_QUOTA);
      response.mutable_get_quota()->mutable_status()->CopyFrom(status);

      return OK(serialize(contentType, evolve(response)),
                stringify(contentType));
    });
}


Future<http::Response> Master::QuotaHandler::status(
    const http::Request& request,
    const Option<Principal>& principal) const
{
  VLOG(1) << "Handling quota status request";

  // Check that the request type is GET which is guaranteed by the master.
  CHECK_EQ("GET", request.method);

  return _status(principal)
    .then([request](const QuotaStatus& status) -> Future<http::Response> {
      return OK(JSON::protobuf(status), request.url.query.get("jsonp"));
    });
}


Future<QuotaStatus> Master::QuotaHandler::_status(
    const Option<Principal>& principal) const
{
  // Quotas can be updated during preparation of the response.
  // Copy current view of the collection to avoid conflicts.
  vector<QuotaInfo> quotaInfos;
  quotaInfos.reserve(master->quotas.size());

  foreachvalue (const Quota& quota, master->quotas) {
    quotaInfos.push_back(quota.info);
  }

  // Create a list of authorization actions for each role we may return.
  //
  // TODO(alexr): Use an authorization filter here once they are available.
  list<Future<bool>> authorizedRoles;
  foreach (const QuotaInfo& info, quotaInfos) {
    authorizedRoles.push_back(authorizeGetQuota(principal, info));
  }

  return process::collect(authorizedRoles)
    .then(defer(
        master->self(),
        [=](const list<bool>& authorizedRolesCollected)
            -> Future<QuotaStatus> {
      CHECK(quotaInfos.size() == authorizedRolesCollected.size());

      QuotaStatus status;
      status.mutable_infos()->Reserve(static_cast<int>(quotaInfos.size()));

      // Create an entry (including role and resources) for each quota,
      // except those filtered out based on the authorizer's response.
      //
      // NOTE: This error-prone code will be removed with
      // the introduction of authorization filters.
      auto quotaInfoIt = quotaInfos.begin();
      foreach (const bool& authorized, authorizedRolesCollected) {
        if (authorized) {
          status.add_infos()->CopyFrom(*quotaInfoIt);
        }
        ++quotaInfoIt;
      }

      return status;
    }));
}


Future<http::Response> Master::QuotaHandler::set(
    const mesos::master::Call& call,
    const Option<Principal>& principal) const
{
  CHECK_EQ(mesos::master::Call::SET_QUOTA, call.type());
  CHECK(call.has_set_quota());

  return _set(call.set_quota().quota_request(), principal);
}


Future<http::Response> Master::QuotaHandler::set(
    const http::Request& request,
    const Option<Principal>& principal) const
{
  VLOG(1) << "Setting quota from request: '" << request.body << "'";

  // Check that the request type is POST which is guaranteed by the master.
  CHECK_EQ("POST", request.method);

  // Parse the request body into JSON.
  Try<JSON::Object> jsonRequest = JSON::parse<JSON::Object>(request.body);
  if (jsonRequest.isError()) {
    return BadRequest(
        "Failed to parse set quota request JSON '" + request.body + "': " +
        jsonRequest.error());
  }

  // Convert JSON request to the `QuotaRequest` protobuf.
  Try<QuotaRequest> protoRequest =
    ::protobuf::parse<QuotaRequest>(jsonRequest.get());

  if (protoRequest.isError()) {
    return BadRequest(
        "Failed to validate set quota request JSON '" + request.body + "': " +
        protoRequest.error());
  }

  return _set(protoRequest.get(), principal);
}


Future<http::Response> Master::QuotaHandler::_set(
    const QuotaRequest& quotaRequest,
    const Option<Principal>& principal) const
{
  Try<QuotaInfo> create = quota::createQuotaInfo(quotaRequest);
  if (create.isError()) {
    return BadRequest(
        "Failed to create 'QuotaInfo' from set quota request: " +
        create.error());
  }

  QuotaInfo quotaInfo = create.get();

  // Check that the `QuotaInfo` is a valid quota request.
  {
    Option<Error> error = quota::validation::quotaInfo(quotaInfo);
    if (error.isSome()) {
      return BadRequest(
          "Failed to validate set quota request: " + error->message);
    }
  }

  // Check that the role is on the role whitelist, if it exists.
  if (!master->isWhitelistedRole(quotaInfo.role())) {
    return BadRequest(
        "Failed to validate set quota request: Unknown role '" +
        quotaInfo.role() + "'");
  }

  // Check that we are not updating an existing quota.
  // TODO(joerg84): Update error message once quota update is in place.
  if (master->quotas.contains(quotaInfo.role())) {
    return BadRequest(
        "Failed to validate set quota request: Cannot set quota"
        " for role '" + quotaInfo.role() + "' which already has quota");
  }

  hashmap<string, Quota> quotaMap = master->quotas;

  // Validate that adding this quota does not violate the hierarchical
  // relationship between quotas.
  quotaMap[quotaInfo.role()] = Quota{quotaInfo};

  QuotaTree quotaTree(quotaMap);

  {
    Option<Error> error = quotaTree.validate();
    if (error.isSome()) {
      return BadRequest(
          "Failed to validate set quota request: " + error->message);
    }
  }

  // Setting quota on a nested role is temporarily disabled.
  //
  // TODO(neilc): Remove this check when MESOS-7402 is fixed.
  bool nestedRole = strings::contains(quotaInfo.role(), "/");
  if (nestedRole) {
    return BadRequest("Setting quota on nested role '" +
                      quotaInfo.role() + "' is not supported yet");
  }

  // The force flag is used to overwrite the `capacityHeuristic` check.
  const bool forced = quotaRequest.force();

  if (principal.isSome()) {
    // We assume that `principal->value.isSome()` is true. The master's HTTP
    // handlers enforce this constraint, and V0 authenticators will only return
    // principals of that form.
    CHECK_SOME(principal->value);

    quotaInfo.set_principal(principal->value.get());
  }

  return authorizeUpdateQuota(principal, quotaInfo)
    .then(defer(master->self(), [=](bool authorized) -> Future<http::Response> {
      return !authorized ? Forbidden() : __set(quotaInfo, forced);
    }));
}


Future<http::Response> Master::QuotaHandler::__set(
    const QuotaInfo& quotaInfo,
    bool forced) const
{
  if (forced) {
    VLOG(1) << "Using force flag to override quota capacity heuristic check";
  } else {
    // Validate whether a quota request can be satisfied.
    Option<Error> error = capacityHeuristic(quotaInfo);
    if (error.isSome()) {
      return Conflict(
          "Heuristic capacity check for set quota request failed: " +
          error.get().message);
    }
  }

  Quota quota = Quota{quotaInfo};

  // Populate master's quota-related local state. We do this before updating
  // the registry in order to make sure that we are not already trying to
  // satisfy a request for this role (since this is a multi-phase event).
  // NOTE: We do not need to remove quota for the role if the registry update
  // fails because in this case the master fails as well.
  master->quotas[quotaInfo.role()] = quota;

  // Update the registry with the new quota and acknowledge the request.
  return master->registrar->apply(Owned<Operation>(
      new quota::UpdateQuota(quotaInfo)))
    .then(defer(master->self(), [=](bool result) -> Future<http::Response> {
      // See the top comment in "master/quota.hpp" for why this check is here.
      CHECK(result);

      master->allocator->setQuota(quotaInfo.role(), quota);

      // Rescind outstanding offers to facilitate satisfying the quota request.
      // NOTE: We set quota before we rescind to avoid a race. If we were to
      // rescind first, then recovered resources may get allocated again
      // before our call to `setQuota` was handled.
      // The consequence of setting quota first is that (in the hierarchical
      // allocator) it will trigger an allocation. This means the rescinded
      // offer resources will only be available to quota once another
      // allocation is invoked.
      // This can be resolved in the future with an explicit allocation call,
      // and this solution is preferred to having the race described earlier.
      rescindOffers(quotaInfo);

      return OK();
    }));
}


Future<http::Response> Master::QuotaHandler::remove(
    const mesos::master::Call& call,
    const Option<Principal>& principal) const
{
  CHECK_EQ(mesos::master::Call::REMOVE_QUOTA, call.type());
  CHECK(call.has_remove_quota());

  return _remove(call.remove_quota().role(), principal);
}


Future<http::Response> Master::QuotaHandler::remove(
    const http::Request& request,
    const Option<Principal>& principal) const
{
  VLOG(1) << "Removing quota for request path: '" << request.url.path << "'";

  // Check that the request type is DELETE which is guaranteed by the master.
  CHECK_EQ("DELETE", request.method);

  // Extract role from url. We expect the request path to have the
  // format "/master/quota/role", where "role" is a role name. The
  // role name itself may contain one or more slashes. Note that
  // `strings::tokenize` returns the remainder of the string when the
  // specified maximum number of tokens is reached.
  vector<string> components = strings::tokenize(request.url.path, "/", 3u);
  if (components.size() < 3u) {
    return BadRequest("Failed to parse remove quota request for path '" +
                      request.url.path + "': expected 3 tokens, found " +
                      stringify(components.size()) + " tokens");
  }

  CHECK_EQ(3u, components.size());
  string role = components.back();

  // Check that the role is on the role whitelist, if it exists.
  if (!master->isWhitelistedRole(role)) {
    return BadRequest(
        "Failed to validate remove quota request for path '" +
        request.url.path + "': Unknown role '" + role + "'");
  }

  // Check that we are removing an existing quota.
  if (!master->quotas.contains(role)) {
    return BadRequest(
        "Failed to remove quota for path '" + request.url.path +
        "': Role '" + role + "' has no quota set");
  }

  hashmap<string, Quota> quotaMap = master->quotas;

  // Validate that removing the quota for `role` does not violate the
  // hierarchical relationship between quotas.
  quotaMap.erase(role);

  QuotaTree quotaTree(quotaMap);

  Option<Error> error = quotaTree.validate();
  if (error.isSome()) {
    return BadRequest(
        "Failed to remove quota for path '" + request.url.path +
        "': " + error->message);
  }

  return _remove(role, principal);
}


Future<http::Response> Master::QuotaHandler::_remove(
    const string& role,
    const Option<Principal>& principal) const
{
  return authorizeUpdateQuota(principal, master->quotas.at(role).info)
    .then(defer(master->self(), [=](bool authorized) -> Future<http::Response> {
      return !authorized ? Forbidden() : __remove(role);
    }));
}


Future<http::Response> Master::QuotaHandler::__remove(const string& role) const
{
  // Remove quota from the quota-related local state. We do this before
  // updating the registry in order to make sure that we are not already
  // trying to remove quota for this role (since this is a multi-phase event).
  // NOTE: We do not need to restore quota for the role if the registry
  // update fails because in this case the master fails as well and quota
  // will be restored automatically during the recovery.
  master->quotas.erase(role);

  // Update the registry with the removed quota and acknowledge the request.
  return master->registrar->apply(Owned<Operation>(
      new quota::RemoveQuota(role)))
    .then(defer(master->self(), [=](bool result) -> Future<http::Response> {
      // See the top comment in "master/quota.hpp" for why this check is here.
      CHECK(result);

      master->allocator->removeQuota(role);

      return OK();
    }));
}


Future<bool> Master::QuotaHandler::authorizeGetQuota(
    const Option<Principal>& principal,
    const QuotaInfo& quotaInfo) const
{
  if (master->authorizer.isNone()) {
    return true;
  }

  LOG(INFO) << "Authorizing principal '"
            << (principal.isSome() ? stringify(principal.get()) : "ANY")
            << "' to get quota for role '" << quotaInfo.role() << "'";

  authorization::Request request;
  request.set_action(authorization::GET_QUOTA);

  Option<authorization::Subject> subject = createSubject(principal);
  if (subject.isSome()) {
    request.mutable_subject()->CopyFrom(subject.get());
  }

  // TODO(alexr): The `value` field is set for backwards compatibility
  // reasons until after the deprecation cycle started with 1.2.0 ends.
  request.mutable_object()->mutable_quota_info()->CopyFrom(quotaInfo);
  request.mutable_object()->set_value(quotaInfo.role());

  return master->authorizer.get()->authorized(request);
}


Future<bool> Master::QuotaHandler::authorizeUpdateQuota(
    const Option<Principal>& principal,
    const QuotaInfo& quotaInfo) const
{
  if (master->authorizer.isNone()) {
    return true;
  }

  LOG(INFO) << "Authorizing principal '"
            << (principal.isSome() ? stringify(principal.get()) : "ANY")
            << "' to update quota for role '" << quotaInfo.role() << "'";

  authorization::Request request;
  request.set_action(authorization::UPDATE_QUOTA);

  Option<authorization::Subject> subject = createSubject(principal);
  if (subject.isSome()) {
    request.mutable_subject()->CopyFrom(subject.get());
  }

  request.mutable_object()->mutable_quota_info()->CopyFrom(quotaInfo);

  return master->authorizer.get()->authorized(request);
}

} // namespace master {
} // namespace internal {
} // namespace mesos {
