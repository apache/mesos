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

#include <list>

#include <mesos/authorizer/authorizer.hpp>

#include <process/collect.hpp>
#include <process/future.hpp>
#include <process/http.hpp>

#include <stout/stringify.hpp>
#include <stout/strings.hpp>
#include <stout/utils.hpp>

#include "master/weights.hpp"

namespace http = process::http;

using google::protobuf::RepeatedPtrField;

using std::list;
using std::string;
using std::vector;

using http::Accepted;
using http::BadRequest;
using http::Conflict;
using http::Forbidden;
using http::OK;

using process::Future;
using process::Owned;

namespace mesos {
namespace internal {
namespace master {

Future<http::Response> Master::WeightsHandler::update(
    const http::Request& request,
    const Option<std::string>& principal) const
{
  VLOG(1) << "Updating weights from request: '" << request.body << "'";

  // Check that the request type is PUT which is guaranteed by the master.
  CHECK_EQ("PUT", request.method);

  Try<JSON::Array> parse = JSON::parse<JSON::Array>(request.body);
  if (parse.isError()) {
    return BadRequest(
        "Failed to parse update request JSON ('" + request.body + "': " +
        parse.error());
  }

  // Create Protobuf representation of weights.
  Try<RepeatedPtrField<WeightInfo>> weightInfos =
    ::protobuf::parse<RepeatedPtrField<WeightInfo>>(parse.get());

  if (weightInfos.isError()) {
    return BadRequest(
        "Failed to convert weights JSON array to protobuf ('" +
        request.body + "'): " + weightInfos.error());
  }

  vector<WeightInfo> validatedWeightInfos;
  vector<string> roles;
  for (WeightInfo& weightInfo : weightInfos.get()) {
    string role = strings::trim(weightInfo.role());

    if (role.empty()) {
      return BadRequest(
          "Role cannot be empty for weight '" +
          stringify(weightInfo.weight()) + "'");
    }

    if (weightInfo.weight() <= 0) {
      return BadRequest(
          "Invalid weight '" + stringify(weightInfo.weight()) +
          "' for role '" + role + "'. Weights must be positive.");
    }

    if (!master->isWhitelistedRole(role)) {
      return BadRequest(
          "Invalid role: '" + role + "', which must exist in the static " +
          "list of roles, specified when the master is started" +
          " (via the --roles flag).");
    }
    weightInfo.set_role(role);
    validatedWeightInfos.push_back(weightInfo);
    roles.push_back(role);
  }

  return authorize(principal, roles)
    .then(defer(master->self(), [=](bool authorized) -> Future<http::Response> {
      if (!authorized) {
        return Forbidden();
      }

      return _update(validatedWeightInfos);
    }));
}


Future<http::Response> Master::WeightsHandler::_update(
    const vector<WeightInfo>& weightInfos) const
{
  // Update the registry and acknowledge the request.
  return master->registrar->apply(Owned<Operation>(
      new weights::UpdateWeights(weightInfos)))
    .then(defer(master->self(), [=](bool result) -> Future<http::Response> {
      CHECK(result);

      // Update weights.
      foreach (const WeightInfo& weightInfo, weightInfos) {
        master->weights[weightInfo.role()] = weightInfo.weight();
      }

      // Notify allocator for updating weights.
      master->allocator->updateWeights(weightInfos);

      // If any active role is updated, we rescind all outstanding offers,
      // to facilitate satisfying the updated weights.
      // NOTE: We update weights before we rescind to avoid a race. If we were
      // to rescind first, then recovered resources may get allocated again
      // before our call to `updateWeights` was handled.
      // The consequence of updating weights first is that (in the hierarchical
      // allocator) it will trigger an allocation if at least one of the
      // updated roles has registered frameworks. This means the rescinded
      // offer resources will only be available to the updated weights once
      // another allocation is invoked.
      // This can be resolved in the future with an explicit allocation call,
      // and this solution is preferred to having the race described earlier.
      rescindOffers(weightInfos);

      return OK();
    }));
}


void Master::WeightsHandler::rescindOffers(
    const std::vector<WeightInfo>& weightInfos) const
{
  bool rescind = false;

  foreach (const WeightInfo& weightInfo, weightInfos) {
    const string& role = weightInfo.role();

    // This should have been validated earlier.
    CHECK(master->isWhitelistedRole(role));

    // Rescind all outstanding offers if at least one of the
    // updated roles has a registered frameworks.
    if (master->activeRoles.contains(role)) {
      rescind = true;
      break;
    }
  }

  if (rescind) {
    foreachvalue (const Slave* slave, master->slaves.registered) {
      foreach (Offer* offer, utils::copy(slave->offers)) {
        master->allocator->recoverResources(
            offer->framework_id(),
            offer->slave_id(),
            offer->resources(),
            None());

        master->removeOffer(offer, true);
      }
    }
  }
}


Future<bool> Master::WeightsHandler::authorize(
    const Option<string>& principal,
    const vector<string>& roles) const
{
  if (master->authorizer.isNone()) {
    return true;
  }

  LOG(INFO) << "Authorizing principal '"
            << (principal.isSome() ? principal.get() : "ANY")
            << "' to update weights for roles '" << stringify(roles) << "'";

  authorization::Request request;
  request.set_action(authorization::UPDATE_WEIGHTS_WITH_ROLE);

  if (principal.isSome()) {
    request.mutable_subject()->set_value(principal.get());
  }

  list<Future<bool>> authorizations;
  foreach (const string& role, roles) {
    request.mutable_object()->set_value(role);
    authorizations.push_back(master->authorizer.get()->authorized(request));
  }

  if (authorizations.empty()) {
    return master->authorizer.get()->authorized(request);
  }

  return await(authorizations)
      .then([](const std::list<Future<bool>>& authorizations)
            -> Future<bool> {
        // Compute a disjunction.
        for (const Future<bool>& authorization : authorizations) {
          if (!authorization.get()) {
            return false;
          }
        }
        return true;
      });
}

} // namespace master {
} // namespace internal {
} // namespace mesos {
