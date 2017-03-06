// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License

#include "authenticator_manager.hpp"

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


class AuthenticatorManagerProcess : public Process<AuthenticatorManagerProcess>
{
public:
  AuthenticatorManagerProcess();

  Future<Nothing> setAuthenticator(
      const string& realm,
      Owned<Authenticator> authenticator);

  Future<Nothing> unsetAuthenticator(
      const string& realm);

  Future<Option<AuthenticationResult>> authenticate(
      const Request& request,
      const string& realm);

private:
  hashmap<string, Owned<Authenticator>> authenticators_;
};


AuthenticatorManagerProcess::AuthenticatorManagerProcess()
  : ProcessBase(ID::generate("__authentication_router__")) {}


Future<Nothing> AuthenticatorManagerProcess::setAuthenticator(
    const string& realm,
    Owned<Authenticator> authenticator)
{
  CHECK_NOTNULL(authenticator.get());
  authenticators_[realm] = authenticator;
  return Nothing();
}


Future<Nothing> AuthenticatorManagerProcess::unsetAuthenticator(
    const string& realm)
{
  authenticators_.erase(realm);
  return Nothing();
}


Future<Option<AuthenticationResult>> AuthenticatorManagerProcess::authenticate(
    const Request& request,
    const string& realm)
{
  if (!authenticators_.contains(realm)) {
    VLOG(2) << "Request for '" << request.url.path << "' requires"
            << " authentication in realm '" << realm << "'"
            << " but no authenticator found";
    return None();
  }

  return authenticators_[realm]->authenticate(request)
    .then([](const AuthenticationResult& authentication)
        -> Future<Option<AuthenticationResult>> {
      // Validate that exactly 1 member is set!
      size_t count =
        (authentication.principal.isSome()    ? 1 : 0) +
        (authentication.unauthorized.isSome() ? 1 : 0) +
        (authentication.forbidden.isSome()    ? 1 : 0);

      if (count != 1) {
        return Failure(
            "HTTP authenticators must return only one of an authenticated "
            "principal, an Unauthorized response, or a Forbidden response");
      }

      if (authentication.principal.isSome()) {
        // Validate that at least one of `value` and `claims` is set.
        if (authentication.principal->value.isNone() &&
            authentication.principal->claims.empty()) {
          return Failure(
              "In the principal returned by an HTTP authenticator, at least one"
              " of 'value' and 'claims' must be set");
        }
      }

      return authentication;
    });
}


AuthenticatorManager::AuthenticatorManager()
  : process(new AuthenticatorManagerProcess())
{
  spawn(process.get());
}


AuthenticatorManager::~AuthenticatorManager()
{
  terminate(process.get());
  wait(process.get());
}


Future<Nothing> AuthenticatorManager::setAuthenticator(
    const string& realm,
    Owned<Authenticator> authenticator)
{
  return dispatch(
      process.get(),
      &AuthenticatorManagerProcess::setAuthenticator,
      realm,
      authenticator);
}


Future<Nothing> AuthenticatorManager::unsetAuthenticator(
    const string& realm)
{
  return dispatch(
      process.get(),
      &AuthenticatorManagerProcess::unsetAuthenticator,
      realm);
}


Future<Option<AuthenticationResult>> AuthenticatorManager::authenticate(
    const Request& request,
    const string& realm)
{
  return dispatch(
      process.get(),
      &AuthenticatorManagerProcess::authenticate,
      request,
      realm);
}

} // namespace authentication {
} // namespace http {
} // namespace process {
