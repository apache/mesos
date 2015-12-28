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

#ifndef __PROCESS_AUTHENTICATION_ROUTER_HPP__
#define __PROCESS_AUTHENTICATION_ROUTER_HPP__

#include <string>

#include <process/authenticator.hpp>
#include <process/future.hpp>
#include <process/http.hpp>
#include <process/owned.hpp>

#include <stout/nothing.hpp>
#include <stout/option.hpp>

namespace process {
namespace http {
namespace authentication {

class AuthenticatorManagerProcess;


// Manages the realm -> Authenticator mapping for HTTP
// authentication in libprocess. Endpoints may map to
// a realm. Each realm may have an authenticator set
// here, through which all requests to matching
// endpoints will be authenticated.
class AuthenticatorManager
{
public:
  AuthenticatorManager();
  ~AuthenticatorManager();

  // Sets the authenticator for the realm; this will
  // overwrite any previous authenticator for the realm.
  Future<Nothing> setAuthenticator(
      const std::string& realm,
      Owned<Authenticator> authenticator);

  // Unsets the authenticator for the realm.
  Future<Nothing> unsetAuthenticator(const std::string& realm);

  // Authenticates the request, will return None if no
  // authentication is required because there is no
  // authenticator for the realm.
  Future<Option<AuthenticationResult>> authenticate(
      const Request& request,
      const std::string& realm);

private:
  Owned<AuthenticatorManagerProcess> process;
};

} // namespace authentication {
} // namespace http {
} // namespace process {

#endif // __PROCESS_REALM_MANAGER_HPP__
