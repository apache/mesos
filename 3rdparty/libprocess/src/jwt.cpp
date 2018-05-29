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

#include <process/jwt.hpp>

#include <memory>
#include <vector>

#include <process/clock.hpp>

#include <process/ssl/utilities.hpp>

#include <stout/base64.hpp>
#include <stout/strings.hpp>

namespace process {
namespace http {
namespace authentication {

using process::Clock;

using process::network::openssl::generate_hmac_sha256;
using process::network::openssl::sign_rsa_sha256;
using process::network::openssl::verify_rsa_sha256;

using std::ostream;
using std::shared_ptr;
using std::string;
using std::vector;

namespace {

Try<JSON::Object> decode(const string& component)
{
  const Try<string> decoded = base64::decode_url_safe(component);

  if (decoded.isError()) {
    return Error("Failed to base64url-decode: " + decoded.error());
  }

  const Try<JSON::Object> json = JSON::parse<JSON::Object>(decoded.get());

  if (json.isError()) {
    return Error("Failed to parse into JSON: " + json.error());
  }

  return json;
}


Try<JWT::Header> parse_header(const string& component)
{
  Try<JSON::Object> header = decode(component);

  if (header.isError()) {
    return Error("Failed to decode token header: " + header.error());
  }

  // Validate JOSE header.

  Option<string> typ = None();

  const Result<JSON::Value> typ_json = header->find<JSON::Value>("typ");

  if (typ_json.isSome()) {
    if (!typ_json->is<JSON::String>()) {
      return Error("Token 'typ' is not a string");
    }

    typ = typ_json->as<JSON::String>().value;
  }

  const Result<JSON::Value> alg_json = header->find<JSON::Value>("alg");

  if (alg_json.isNone()) {
    return Error("Failed to locate 'alg' in token JSON header");
  }

  if (alg_json.isError()) {
    return Error(
        "Error when extracting 'alg' field from token JSON header: " +
        alg_json.error());
  }

  if (!alg_json->is<JSON::String>()) {
    return Error("Token 'alg' field is not a string");
  }

  const string alg_value = alg_json->as<JSON::String>().value;

  JWT::Alg alg;

  if (alg_value == "none") {
    alg = JWT::Alg::None;
  } else if (alg_value == "HS256") {
    alg = JWT::Alg::HS256;
  } else if (alg_value == "RS256") {
    alg = JWT::Alg::RS256;
  } else {
    return Error("Unsupported token algorithm: " + alg_value);
  }

  const Result<JSON::Value> crit_json = header->find<JSON::Value>("crit");

  // The 'crit' header parameter indicates extensions that must be understood.
  // As we're not supporting any extensions, the JWT header is deemed to be
  // invalid upon the presence of this parameter.
  if (crit_json.isSome()) {
    return Error("Token 'crit' field is unsupported");
  }

  return JWT::Header{alg, typ};
}


Try<JSON::Object> parse_payload(const string& component)
{
  Try<JSON::Object> payload = decode(component);

  if (payload.isError()) {
    return Error("Failed to decode token payload: " + payload.error());
  }

  // Validate standard claims.

  const Result<JSON::Value> exp_json = payload->find<JSON::Value>("exp");

  if (exp_json.isError()) {
    return Error(
        "Error when extracting 'exp' field from token JSON payload: " +
        exp_json.error());
  }

  if (exp_json.isSome()) {
    if (!exp_json->is<JSON::Number>()) {
      return Error("JSON payload 'exp' field is not a number");
    }

    const int64_t exp = exp_json->as<JSON::Number>().as<int64_t>();
    const int64_t now = Clock::now().secs();

    if (exp < now) {
      return Error(
          "Token has expired: exp(" +
          stringify(exp) + ") < now(" + stringify(now) + ")");
    }
  }

  // TODO(nfnt): Validate other standard claims.
  return payload;
}


// Implements equality between strings which run in constant time by either
// comparing the sizes, and thus ignoring their content, or checking the whole
// content of them, thus avoiding timing attacks when comparing hashes.
bool constantTimeEquals(const string& left, const string& right)
{
  if (left.size() != right.size()) {
    return false;
  }

  unsigned valid = 0;
  for (size_t i = 0; i < left.size(); ++i) {
    valid |= left[i] ^ right[i];
  }

  return valid == 0;
}

} // namespace {


Try<JWT, JWTError> JWT::parse(const std::string& token)
{
  const vector<string> components = strings::split(token, ".");

  if (components.size() != 3) {
    return JWTError(
        "Expected 3 components in token, got " + stringify(components.size()),
        JWTError::Type::INVALID_TOKEN);
  }

  Try<JWT::Header> header = parse_header(components[0]);

  if (header.isError()) {
    return JWTError(header.error(), JWTError::Type::INVALID_TOKEN);
  }

  if (header->alg != JWT::Alg::None) {
    return JWTError(
        "Token 'alg' value \"" + stringify(header->alg) +
        "\" does not match, expected \"none\"",
        JWTError::Type::INVALID_TOKEN);
  }

  Try<JSON::Object> payload = parse_payload(components[1]);

  if (payload.isError()) {
    return JWTError(payload.error(), JWTError::Type::INVALID_TOKEN);
  }

  if (!components[2].empty()) {
    return JWTError(
        "Unsecured JWT contains a signature",
        JWTError::Type::INVALID_TOKEN);
  }

  return JWT(header.get(), payload.get(), None());
}


Try<JWT, JWTError> JWT::parse(const string& token, const string& secret)
{
  const vector<string> components = strings::split(token, ".");

  if (components.size() != 3) {
    return JWTError(
        "Expected 3 components in token, got " + stringify(components.size()),
        JWTError::Type::INVALID_TOKEN);
  }

  Try<JWT::Header> header = parse_header(components[0]);

  if (header.isError()) {
    return JWTError(header.error(), JWTError::Type::INVALID_TOKEN);
  }

  if (header->alg != JWT::Alg::HS256) {
    return JWTError(
        "Token 'alg' value \"" + stringify(header->alg) +
        "\" does not match, expected \"HS256\"",
        JWTError::Type::INVALID_TOKEN);
  }

  Try<JSON::Object> payload = parse_payload(components[1]);

  if (payload.isError()) {
    return JWTError(payload.error(), JWTError::Type::INVALID_TOKEN);
  }

  const Try<string> signature = base64::decode_url_safe(components[2]);

  if (signature.isError()) {
    return JWTError(
        "Failed to base64url-decode token signature: " + signature.error(),
        JWTError::Type::INVALID_TOKEN);
  }

  // Validate HMAC SHA256 signature.

  Try<string> hmac = generate_hmac_sha256(
      components[0] + "." + components[1],
      secret);

  if (hmac.isError()) {
    return JWTError(
        "Failed to generate HMAC signature: " + hmac.error(),
        JWTError::Type::UNKNOWN);
  }

  if (!constantTimeEquals(hmac.get(), signature.get())) {
    return JWTError(
        "Token signature does not match",
        JWTError::Type::INVALID_TOKEN);
  }

  return JWT(header.get(), payload.get(), signature.get());
}


Try<JWT, JWTError> JWT::parse(const string& token, shared_ptr<RSA> publicKey)
{
  CHECK_NOTNULL(publicKey.get());

  const vector<string> components = strings::split(token, ".");

  if (components.size() != 3) {
    return JWTError(
        "Expected 3 components in token, got " + stringify(components.size()),
        JWTError::Type::INVALID_TOKEN);
  }

  Try<JWT::Header> header = parse_header(components[0]);

  if (header.isError()) {
    return JWTError(header.error(), JWTError::Type::INVALID_TOKEN);
  }

  if (header->alg != JWT::Alg::RS256) {
    return JWTError(
        "Token 'alg' value \"" + stringify(header->alg) +
        "\" does not match, expected \"RS256\"",
        JWTError::Type::INVALID_TOKEN);
  }

  Try<JSON::Object> payload = parse_payload(components[1]);

  if (payload.isError()) {
    return JWTError(payload.error(), JWTError::Type::INVALID_TOKEN);
  }

  const Try<string> signature = base64::decode_url_safe(components[2]);

  if (signature.isError()) {
    return JWTError(
        "Failed to base64url-decode token signature: " + signature.error(),
        JWTError::Type::INVALID_TOKEN);
  }

  // Validate RSA SHA-256 signature.

  const Try<Nothing> valid = verify_rsa_sha256(
      components[0] + "." + components[1], signature.get(), publicKey);

  if (valid.isError()) {
    return JWTError(
        "Failed to verify token: " + valid.error(),
        JWTError::Type::INVALID_TOKEN);
  }

  return JWT(header.get(), payload.get(), signature.get());
}


Try<JWT, JWTError> JWT::create(const JSON::Object& payload)
{
  const Header header{Alg::None, "JWT"};

  return JWT(header, payload, None());
}


Try<JWT, JWTError> JWT::create(
    const JSON::Object& payload,
    const string& secret)
{
  const Header header{Alg::HS256, "JWT"};

  const Try<string> hmac = generate_hmac_sha256(
      base64::encode_url_safe(stringify(header), false) + "." +
        base64::encode_url_safe(stringify(payload), false),
      secret);

  if (hmac.isError()) {
    return JWTError(
        "Failed to generate HMAC signature: " + hmac.error(),
        JWTError::Type::UNKNOWN);
  }

  return JWT(header, payload, base64::encode_url_safe(hmac.get(), false));
}


Try<JWT, JWTError> JWT::create(
    const JSON::Object& payload,
    shared_ptr<RSA> privateKey)
{
  CHECK_NOTNULL(privateKey.get());

  const Header header{Alg::RS256, "JWT"};

  const string message = base64::encode_url_safe(stringify(header), false) +
    "." + base64::encode_url_safe(stringify(payload), false);

  const Try<string> signature = sign_rsa_sha256(message, privateKey);

  if (signature.isError()) {
    return JWTError(
        "Failed to generate RSA signature: " + signature.error(),
        JWTError::Type::UNKNOWN);
  }

  return JWT(
    header,
    payload,
    base64::encode_url_safe(signature.get(), false));
}


JWT::JWT(
    const Header& _header,
    const JSON::Object& _payload,
    const Option<string>& _signature)
  : header(_header), payload(_payload), signature(_signature) {}


ostream& operator<<(ostream& stream, const JWT::Alg& alg)
{
  switch (alg) {
    case JWT::Alg::None:
      stream << "none";
      break;
    case JWT::Alg::HS256:
      stream << "HS256";
      break;
    case JWT::Alg::RS256:
      stream << "RS256";
      break;
  }

  return stream;
}


ostream& operator<<(ostream& stream, const JWT::Header& header)
{
  JSON::Object json;

  json.values["alg"] = stringify(header.alg);
  if (header.typ.isSome()) {
    json.values["typ"] = header.typ.get();
  }

  stream << stringify(json);
  return stream;
}


ostream& operator<<(ostream& stream, const JWT& jwt)
{
  stream << base64::encode_url_safe(stringify(jwt.header), false) + "."
         << base64::encode_url_safe(stringify(jwt.payload), false) + ".";

  if (jwt.signature.isSome()) {
    stream << jwt.signature.get();
  }

  return stream;
}

} // namespace authentication {
} // namespace http {
} // namespace process {
