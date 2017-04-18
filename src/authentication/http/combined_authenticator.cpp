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

#include <mesos/authentication/http/combined_authenticator.hpp>

#include <list>
#include <string>
#include <utility>
#include <vector>

#include <process/id.hpp>
#include <process/loop.hpp>
#include <process/process.hpp>

#include <stout/foreach.hpp>
#include <stout/hashset.hpp>
#include <stout/strings.hpp>

namespace mesos {
namespace http {
namespace authentication {

using std::list;
using std::make_pair;
using std::pair;
using std::string;
using std::vector;

using process::Break;
using process::Continue;
using process::ControlFlow;
using process::Failure;
using process::Future;
using process::Owned;
using process::Process;
using process::UPID;

using process::http::Forbidden;
using process::http::Request;
using process::http::Unauthorized;

using process::http::authentication::AuthenticationResult;
using process::http::authentication::Authenticator;


class CombinedAuthenticatorProcess
  : public Process<CombinedAuthenticatorProcess>
{
public:
  CombinedAuthenticatorProcess(
      const string& _realm,
      vector<Owned<Authenticator>>&& _authenticators);

  Future<AuthenticationResult> authenticate(const Request& request);

private:
  typedef pair<string, Try<AuthenticationResult>> SchemeResultPair;

  static bool anyUnauthorized(
      const list<SchemeResultPair>& authenticationResults);

  static bool anyForbidden(const list<SchemeResultPair>& authenticationResults);

  static bool anyError(const list<SchemeResultPair>& authenticationResults);

  static vector<string> extractUnauthorizedHeaders(
      const list<SchemeResultPair>& authenticationResults);

  static vector<string> extractUnauthorizedBodies(
      const list<SchemeResultPair>& authenticationResults);

  static vector<string> extractForbiddenBodies(
      const list<SchemeResultPair>& authenticationResults);

  static vector<string> extractErrorMessages(
      const list<SchemeResultPair>& authenticationResults);

  static Future<ControlFlow<AuthenticationResult>> combineFailed(
      const list<SchemeResultPair>& results);

  const vector<Owned<Authenticator>> authenticators;
  const string realm;
};


CombinedAuthenticatorProcess::CombinedAuthenticatorProcess(
    const string& _realm,
    vector<Owned<Authenticator>>&& _authenticators)
  : ProcessBase(process::ID::generate("__combined_authenticator__")),
    authenticators(std::move(_authenticators)),
    realm(_realm) {}


bool CombinedAuthenticatorProcess::anyUnauthorized(
    const list<SchemeResultPair>& authenticationResults)
{
  foreach (const SchemeResultPair& result, authenticationResults) {
    if (result.second.isSome() && result.second->unauthorized.isSome()) {
      return true;
    }
  }

  return false;
}


bool CombinedAuthenticatorProcess::anyForbidden(
    const list<SchemeResultPair>& authenticationResults)
{
  foreach (const SchemeResultPair& result, authenticationResults) {
    if (result.second.isSome() && result.second->forbidden.isSome()) {
      return true;
    }
  }

  return false;
}


bool CombinedAuthenticatorProcess::anyError(
    const list<SchemeResultPair>& authenticationResults)
{
  foreach (const SchemeResultPair& result, authenticationResults) {
    if (result.second.isError()) {
      return true;
    }
  }

  return false;
}


vector<string> CombinedAuthenticatorProcess::extractUnauthorizedHeaders(
    const list<SchemeResultPair>& authenticationResults)
{
  vector<string> headers;

  foreach (const SchemeResultPair& result, authenticationResults) {
    if (result.second.isSome() &&
        result.second->unauthorized.isSome() &&
        result.second->unauthorized->headers.contains("WWW-Authenticate")) {
      headers.push_back(
          result.second->unauthorized->headers.at("WWW-Authenticate"));
    }
  }

  return headers;
}


vector<string> CombinedAuthenticatorProcess::extractUnauthorizedBodies(
    const list<SchemeResultPair>& authenticationResults)
{
  vector<string> bodies;

  foreachpair (
      const string& scheme,
      const Try<AuthenticationResult>& result,
      authenticationResults) {
    if (result.isSome() &&
        result->unauthorized.isSome() &&
        result->unauthorized->body != "") {
      bodies.push_back(
          "\"" + scheme + "\" authenticator returned:\n" +
          result->unauthorized->body);
    }
  }

  return bodies;
}


vector<string> CombinedAuthenticatorProcess::extractForbiddenBodies(
    const list<SchemeResultPair>& authenticationResults)
{
  vector<string> bodies;

  foreachpair (
      const string& scheme,
      const Try<AuthenticationResult>& result,
      authenticationResults) {
    if (result.isSome() &&
        result->forbidden.isSome() &&
        result->forbidden->body != "") {
      bodies.push_back(
          "\"" + scheme + "\" authenticator returned:\n" +
          result->forbidden->body);
    }
  }

  return bodies;
}


vector<string> CombinedAuthenticatorProcess::extractErrorMessages(
    const list<SchemeResultPair>& authenticationResults)
{
  vector<string> messages;

  foreachpair (
      const string& scheme,
      const Try<AuthenticationResult>& result,
      authenticationResults) {
    if (result.isError()) {
      messages.push_back(
          "\"" + scheme + "\" authenticator returned:\n" +
          result.error());
    }
  }

  return messages;
}


// Creates a single authentication result for the authenticator to return
// in the case that all authentication attempts have failed.
Future<ControlFlow<AuthenticationResult>>
  CombinedAuthenticatorProcess::combineFailed(
      const list<SchemeResultPair>& results)
{
  AuthenticationResult combinedResult;

  if (anyUnauthorized(results)) {
    combinedResult.unauthorized = Unauthorized(
        {strings::join(",", extractUnauthorizedHeaders(results))},
        strings::join("\n\n", extractUnauthorizedBodies(results)));

    return Break(combinedResult);
  }

  if (anyForbidden(results)) {
    combinedResult.forbidden =
      Forbidden(strings::join("\n\n", extractForbiddenBodies(results)));

    return Break(combinedResult);
  }

  // This case serves to surface errors from failed futures to libprocess.
  if (anyError(results)) {
    return Failure(strings::join("\n\n", extractErrorMessages(results)));
  }

  // Here, it is possible that we return a default-initialized
  // `AuthenticationResult`. Libprocess considers such a result invalid and will
  // fail the HTTP request in that case. We allow it here to maintain existing
  // behavior, in which libprocess is responsible for enforcing this constraint.
  return Break(combinedResult);
}


Future<AuthenticationResult> CombinedAuthenticatorProcess::authenticate(
    const Request& request)
{
  // Variables to hold the state of the authentication loop.
  auto iter = authenticators.begin();
  auto end = authenticators.end();
  // Each pair contains a string representing the scheme of the authenticator
  // and a `Try<AuthenticationResult>` which is used to capture failure messages
  // in the event that an authenticator returns a failed future.
  list<SchemeResultPair> results;

  UPID self_ = self();

  // Loop over all installed authenticators.
  return loop(
      self(),
      [iter, end]() mutable {
        return iter == end ? Option<Owned<Authenticator>>::none() : *(iter++);
      },
      [request, results, self_](
          const Option<Owned<Authenticator>>& authenticator) mutable
              -> Future<ControlFlow<AuthenticationResult>> {
        // All authentication attempts have failed. Combine them and return.
        if (authenticator.isNone()) {
          return combineFailed(results);
        }

        // Instead of capturing `authenticator` we capture the scheme by copy,
        // since that's all we need. This avoids an issue during teardown of the
        // authenticator: if the `CombinedAuthenticator` is destroyed before the
        // callback below executes, the `authenticator` reference here could be
        // the last remaining reference to that authenticator. Capturing this
        // reference could cause the authenticator's destructor to be called
        // from within its own context when the callback completes, leading to a
        // deadlock. See MESOS-7065.
        const string scheme = authenticator.get()->scheme();

        return authenticator.get()->authenticate(request)
          .then(defer(
              self_,
              [&results, scheme](const AuthenticationResult& result)
                  -> ControlFlow<AuthenticationResult> {
                // Validate that exactly 1 member is set.
                size_t count =
                  (result.principal.isSome()    ? 1 : 0) +
                  (result.unauthorized.isSome() ? 1 : 0) +
                  (result.forbidden.isSome()    ? 1 : 0);

                if (count != 1) {
                  LOG(WARNING) << "HTTP authenticator for scheme '" << scheme
                               << "' returned a result with " << count
                               << " members set, which is an error";
                  return Continue();
                }

                if (result.principal.isSome()) {
                  // Authentication successful; break and return the result.
                  return Break(result);
                }

                // Authentication unsuccessful; append the result and continue.
                results.push_back(make_pair(scheme, result));
                return Continue();
              }))
          .repair([&results, scheme](
              const Future<ControlFlow<AuthenticationResult>>& failedResult)
                  -> ControlFlow<AuthenticationResult> {
            results.push_back(make_pair(scheme, Error(failedResult.failure())));
            return Continue();
          });
      });
}


CombinedAuthenticator::CombinedAuthenticator(
    const string& _realm,
    vector<Owned<Authenticator>>&& _authenticators)
{
  // Initialize the set of offered authentication schemes.
  foreach (const Owned<Authenticator>& authenticator, _authenticators) {
    schemes.insert(authenticator->scheme());
  }

  process = Owned<CombinedAuthenticatorProcess>(
      new CombinedAuthenticatorProcess(_realm, std::move(_authenticators)));

  spawn(process.get());
}


CombinedAuthenticator::~CombinedAuthenticator()
{
  terminate(process.get());
  wait(process.get());
}


Future<AuthenticationResult> CombinedAuthenticator::authenticate(
    const Request& request)
{
  return dispatch(
      process.get(), &CombinedAuthenticatorProcess::authenticate, request);
}


string CombinedAuthenticator::scheme() const
{
  return strings::join(" ", schemes);
}

} // namespace authentication {
} // namespace http {
} // namespace mesos {
