/**
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License
*/

#include "authentication_router.hpp"

#include <string>

#include <process/authenticator.hpp>
#include <process/dispatch.hpp>
#include <process/future.hpp>
#include <process/http.hpp>
#include <process/id.hpp>
#include <process/owned.hpp>
#include <process/process.hpp>

#include <stout/hashmap.hpp>
#include <stout/nothing.hpp>
#include <stout/path.hpp>

using std::string;

namespace process {
namespace http {
namespace authentication {


class AuthenticationRouterProcess : public Process<AuthenticationRouterProcess>
{
public:
  AuthenticationRouterProcess();

  Future<Nothing> setAuthenticator(
      const string& realm,
      Owned<Authenticator> authenticator);

  Future<Nothing> unsetAuthenticator(
      const string& realm);

  Future<Nothing> addEndpoint(
      const string& endpoint,
      const string& realm);

  Future<Nothing> removeEndpoint(
      const string& realm);

  Future<Option<AuthenticationResult>> authenticate(
      const Request& request);

private:
  hashmap<string, Owned<Authenticator>> authenticators_;
  hashmap<string, string> endpoints_;
};


AuthenticationRouterProcess::AuthenticationRouterProcess()
  : ProcessBase(ID::generate("AuthenticationRouter")) {}


Future<Nothing> AuthenticationRouterProcess::setAuthenticator(
    const string& realm,
    Owned<Authenticator> authenticator)
{
  CHECK_NOTNULL(authenticator.get());
  authenticators_[realm] = authenticator;
  return Nothing();
}


Future<Nothing> AuthenticationRouterProcess::unsetAuthenticator(
    const string& realm)
{
  authenticators_.erase(realm);
  return Nothing();
}


Future<Nothing> AuthenticationRouterProcess::addEndpoint(
    const string& endpoint,
    const string& realm)
{
  endpoints_[endpoint] = realm;
  return Nothing();
}


Future<Nothing> AuthenticationRouterProcess::removeEndpoint(
    const string& endpoint)
{
  endpoints_.erase(endpoint);
  return Nothing();
}


Future<Option<AuthenticationResult>> AuthenticationRouterProcess::authenticate(
    const Request& request)
{
  // Finds the longest prefix path which matches a realm.
  Option<string> realm;
  string name = request.url.path;

  while (Path(name).dirname() != name) {
    if (endpoints_.contains(name)) {
      realm = endpoints_[name];
      break;
    }

    name = Path(name).dirname();
  }

  if (realm.isNone()) {
    return None(); // Request doesn't need authentication.
  }

  if (!authenticators_.contains(realm.get())) {
    VLOG(2) << "Request for '" << request.url.path << "' requires"
            << " authentication in realm '" << realm.get() << "'"
            << " but no authenticator found";
    return None();
  }

  return authenticators_[realm.get()]->authenticate(request)
    .then([](const AuthenticationResult& authentication)
        -> Future<Option<AuthenticationResult>> {
      // Validate that exactly 1 member is set!
      size_t count =
        (authentication.principal.isSome()    ? 1 : 0) +
        (authentication.unauthorized.isSome() ? 1 : 0) +
        (authentication.forbidden.isSome()    ? 1 : 0);

      if (count != 1) {
        return Failure("Expecting one of 'principal', 'unauthorized',"
                       " or 'forbidden' to be set");
      }

      return authentication;
    });
}


AuthenticationRouter::AuthenticationRouter()
  : process(new AuthenticationRouterProcess())
{
  spawn(process.get());
}


AuthenticationRouter::~AuthenticationRouter()
{
  terminate(process.get());
  wait(process.get());
}


Future<Nothing> AuthenticationRouter::setAuthenticator(
    const string& realm,
    Owned<Authenticator> authenticator)
{
  return dispatch(
      process.get(),
      &AuthenticationRouterProcess::setAuthenticator,
      realm,
      authenticator);
}


Future<Nothing> AuthenticationRouter::unsetAuthenticator(
    const string& realm)
{
  return dispatch(
      process.get(),
      &AuthenticationRouterProcess::unsetAuthenticator,
      realm);
}


Future<Nothing> AuthenticationRouter::addEndpoint(
    const string& endpoint,
    const string& realm)
{
  return dispatch(
      process.get(),
      &AuthenticationRouterProcess::addEndpoint,
      endpoint,
      realm);
}


Future<Nothing> AuthenticationRouter::removeEndpoint(
    const string& realm)
{
  return dispatch(
      process.get(),
      &AuthenticationRouterProcess::removeEndpoint,
      realm);
}


Future<Option<AuthenticationResult>> AuthenticationRouter::authenticate(
    const Request& request)
{
  return dispatch(
      process.get(),
      &AuthenticationRouterProcess::authenticate,
      request);
}

} // namespace authentication {
} // namespace http {
} // namespace process {
