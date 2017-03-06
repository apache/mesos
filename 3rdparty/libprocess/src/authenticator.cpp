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

#include <process/authenticator.hpp>
#include <process/id.hpp>

#include <string>
#include <vector>

#include <process/dispatch.hpp>
#include <process/future.hpp>
#include <process/http.hpp>
#include <process/process.hpp>

#include <stout/base64.hpp>
#include <stout/hashmap.hpp>
#include <stout/option.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>

namespace process {
namespace http {
namespace authentication {

using std::string;
using std::vector;


static void json(JSON::ObjectWriter* writer, const Principal& principal)
{
  if (principal.value.isSome()) {
    writer->field("value", principal.value.get());
  }
  if (!principal.claims.empty()) {
    writer->field("claims", principal.claims);
  }
}


std::ostream& operator<<(std::ostream& stream, const Principal& principal)
{
  // When only the `value` is set, simply output a string.
  if (principal.value.isSome() && principal.claims.empty()) {
    return stream << principal.value.get();
  }

  return stream << jsonify(principal);
};


class BasicAuthenticatorProcess : public Process<BasicAuthenticatorProcess>
{
public:
  BasicAuthenticatorProcess(
      const string& realm,
      const hashmap<string, string>& credentials);

  virtual Future<AuthenticationResult> authenticate(
      const http::Request& request);

private:
  const string realm_;
  const hashmap<string, string> credentials_;
};


BasicAuthenticatorProcess::BasicAuthenticatorProcess(
    const string& realm,
    const hashmap<string, string>& credentials)
  : ProcessBase(ID::generate("__basic_authenticator__")),
    realm_(realm),
    credentials_(credentials) {}


Future<AuthenticationResult> BasicAuthenticatorProcess::authenticate(
    const Request& request)
{
  AuthenticationResult unauthorized;
  unauthorized.unauthorized =
    Unauthorized({"Basic realm=\"" + realm_ + "\""});

  Option<string> credentials = request.headers.get("Authorization");

  if (credentials.isNone()) {
    return unauthorized;
  }

  vector<string> components = strings::split(credentials.get(), " ");

  if (components.size() != 2 || components[0] != "Basic") {
    return unauthorized;
  }

  Try<string> decoded = base64::decode(components[1]);

  if (decoded.isError()) {
    return unauthorized;
  }

  vector<string> credential = strings::split(decoded.get(), ":");

  if (credential.size() != 2 ||
      !credentials_.contains(credential[0]) ||
      credentials_.at(credential[0]) != credential[1]) {
    return unauthorized;
  }

  AuthenticationResult authenticated;
  authenticated.principal = Principal(credential[0]);
  return authenticated;
}


BasicAuthenticator::BasicAuthenticator(
    const string& realm,
    const hashmap<string, string>& credentials)
  : process_(new BasicAuthenticatorProcess(realm, credentials))
{
  spawn(*process_);
}


BasicAuthenticator::~BasicAuthenticator()
{
  terminate(*process_);
  wait(*process_);
}


Future<AuthenticationResult> BasicAuthenticator::authenticate(
    const Request& request)
{
  return dispatch(*process_, &BasicAuthenticatorProcess::authenticate, request);
}


string BasicAuthenticator::scheme() const
{
  return "Basic";
}

} // namespace authentication {
} // namespace http {
} // namespace process {
