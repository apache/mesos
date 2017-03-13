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

#include <map>
#include <vector>

#include <process/dispatch.hpp>
#include <process/http.hpp>
#include <process/id.hpp>
#include <process/jwt.hpp>
#include <process/process.hpp>

#include <stout/foreach.hpp>
#include <stout/try.hpp>
#include <stout/unreachable.hpp>

namespace process {
namespace http {
namespace authentication {

using std::map;
using std::string;
using std::vector;


class JWTAuthenticatorProcess : public Process<JWTAuthenticatorProcess>
{
public:
  JWTAuthenticatorProcess(const string& realm, const string& secret);

  Future<AuthenticationResult> authenticate(const Request& request);

private:
  const string realm_;
  const string secret_;
};


JWTAuthenticatorProcess::JWTAuthenticatorProcess(
    const string& realm,
    const string& secret)
  : ProcessBase(ID::generate("__jwt_authenticator__")),
    realm_(realm),
    secret_(secret) {}


Future<AuthenticationResult> JWTAuthenticatorProcess::authenticate(
    const Request &request)
{
  AuthenticationResult result;

  Option<string> header = request.headers.get("Authorization");

  if (header.isNone()) {
    // Requests without any authentication information shouldn't include
    // error information (see RFC 6750, Section 3.1).
    result.unauthorized = Unauthorized({"Bearer realm=\"" + realm_ + "\""});
    return result;
  }

  const vector<string> token = strings::split(header.get(), " ");

  if (token.size() != 2) {
    result.unauthorized = Unauthorized({
      "Bearer realm=\"" + realm_ + "\", "
      "error=\"invalid_token\", "
      "error_description=\"Malformed 'Authorization' header\""});
    return result;
  }

  if (token[0] != "Bearer") {
    result.unauthorized = Unauthorized({
        "Bearer realm=\"" + realm_ + "\", "
        "error=\"invalid_token\", "
        "error_description=\"Scheme '" + token[0] + "' unsupported\""});
    return result;
  }

  const Try<JWT, JWTError> jwt = JWT::parse(token[1], secret_);

  if (jwt.isError()) {
    switch (jwt.error().type) {
      case JWTError::Type::INVALID_TOKEN:
        result.unauthorized = Unauthorized({
            "Bearer realm=\"" + realm_ + "\", "
            "error=\"invalid_token\", "
            "error_description=\"Invalid JWT: " + jwt.error().message + "\""});
        return result;

      case JWTError::Type::UNKNOWN:
        return Failure(jwt.error());

      UNREACHABLE();
    }
  }

  Principal principal(Option<string>::none());

  if (jwt->payload.values.empty()) {
    result.unauthorized = Unauthorized({
      "Bearer realm=\"" + realm_ + "\", "
      "error=\"invalid_token\", "
      "error_description=\"JWT claims missing\""});
    return result;
  }

  foreachpair (
      const string& key,
      const JSON::Value& value,
      jwt->payload.values) {
    // Calling 'stringify' on a 'JSON::String' would result in a quoted
    // string. Hence this case is treated differently.
    if (value.is<JSON::String>()) {
      principal.claims[key] = value.as<JSON::String>().value;
    } else {
      principal.claims[key] = stringify(value);
    }
  }

  result.principal = principal;
  return result;
}


JWTAuthenticator::JWTAuthenticator(const string& realm, const string& secret)
  : process_(new JWTAuthenticatorProcess(realm, secret))
{
  spawn(process_.get());
}


JWTAuthenticator::~JWTAuthenticator()
{
  terminate(process_.get());
  wait(process_.get());
}


Future<AuthenticationResult> JWTAuthenticator::authenticate(
    const Request& request)
{
  return dispatch(
      process_.get(),
      &JWTAuthenticatorProcess::authenticate,
      request);
}


string JWTAuthenticator::scheme() const
{
  return "Bearer";
}

} // namespace authentication {
} // namespace http {
} // namespace process {
