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

#include <process/jwk.hpp>

#include <stout/base64.hpp>
#include <stout/foreach.hpp>
#include <stout/json.hpp>

#include <process/ssl/utilities.hpp>

namespace process {
namespace http {
namespace authentication {

using namespace std;

using process::network::openssl::RSA_shared_ptr;

typedef std::unique_ptr<BIGNUM, void(*)(BIGNUM*)> BIGNUM_unique_ptr;

/**
 * Helper function finding a string in a JSON.
 */
Try<string> findString(const JSON::Object& json, const string& key)
{
  auto value_json = json.find<JSON::Value>(key);
  if (value_json.isNone()) {
    return Error("Failed to locate '" + key + "' in JWK");
  }

  if (!value_json->is<JSON::String>()) {
    return Error("Token '" + key + "' is not a string");
  }
  return value_json->as<JSON::String>().value;
}

/**
 * Helper function extracting a big num from the JWK.
 */
Try<BIGNUM_unique_ptr> extractBigNum(
    const JSON::Object& jwk,
    const string& paramKey)
{
  auto paramBase64 = findString(jwk, paramKey);
  if (paramBase64.isError()) {
    return Error(paramBase64.error());
  }

  auto param = base64::decode_url_safe(paramBase64.get());
  if (param.isError()) {
    return Error("Failed to base64url-decode: " + param.error());
  }

  BIGNUM_unique_ptr paramBn(
    BN_bin2bn(
      reinterpret_cast<const unsigned char*>(param.get().c_str()),
      param.get().size(),
      nullptr),
    BN_free);

  if (!paramBn) {
    return Error("Failed to convert '" + paramKey +"' to BIGNUM");
  }

  return paramBn;
}

/**
 * Extract a set of BIGNUMs based on their key name. This function returns
 * an error if one required key is missing or add an empty pointer to the
 * map if the key is optional.
 */
Try<map<string, BIGNUM_unique_ptr>> extractBigNums(
    const JSON::Object& jwk,
    const vector<string>& requiredKeys,
    const vector<string>& optionalKeys = vector<string>())
{
  map<string, BIGNUM_unique_ptr> bigNums;

  // Treat required keys
  foreach (const string& key, requiredKeys) {
    auto bigNum = extractBigNum(jwk, key);
    if (bigNum.isError()) {
      return Error(bigNum.error());
    }
    bigNums.insert(make_pair(key, move(bigNum.get())));
  }

  // Then treat optional keys
  foreach (const string& key, optionalKeys) {
    auto bigNum = extractBigNum(jwk, key);
    if (bigNum.isError()) {
      bigNums.insert(
        make_pair(key, move(BIGNUM_unique_ptr(nullptr, BN_free))));
    }
    else {
      bigNums.insert(make_pair(key, move(bigNum.get())));
    }
  }

  return bigNums;
}


Try<RSA_shared_ptr> jwkToRSAPublicKey(const JSON::Object& jwk)
{
  // e is the public exponent
  // n is the modulus
  auto params = extractBigNums(jwk, {"e", "n"});
  if (params.isError()) {
    return Error("Failed to create RSA public key: " + params.error());
  }

  RSA_shared_ptr rsaKey(RSA_new(), RSA_free);
  rsaKey->e = params->at("e").release();
  rsaKey->n = params->at("n").release();
  return rsaKey;
}


Try<RSA_shared_ptr> jwkToRSAPrivateKey(
    const JSON::Object& jwk)
{
  // e is the public exponent
  // n is the modulus
  // d is the private exponent
  // p and q are secret prime factors (optional in RSA)
  // dp is d mod (p-1) (optional in RSA)
  // dq is d mod (q-1) (optional in RSA)
  // qi is q^-1 mod p  (optional in RSA)
  auto params = extractBigNums(jwk,
    {"e", "n", "d"}, {"p", "q", "dp", "dq", "qi"});

  if (params.isError()) {
    return Error("Failed to create RSA private key: " + params.error());
  }

  RSA_shared_ptr rsaKey(RSA_new(), RSA_free);
  rsaKey->e = params->at("e").release();
  rsaKey->n = params->at("n").release();
  rsaKey->d = params->at("d").release();

  auto findOrNullptr = [&params](const string& key) -> BIGNUM* {
    auto it = params->find(key);
    if (it == params->end()) {
      return nullptr;
    }
    return it->second.release();
  };

  rsaKey->p = findOrNullptr("p");
  rsaKey->q = findOrNullptr("q");

  rsaKey->dmp1 = findOrNullptr("dp");
  rsaKey->dmq1 = findOrNullptr("dq");
  rsaKey->iqmp = findOrNullptr("qi");
  return rsaKey;
}


Try<RSA_shared_ptr> jwkToRSAKey(const JSON::Object& jwk)
{
  Try<string> d = findString(jwk, "d");
  if (d.isSome()) {
    return jwkToRSAPrivateKey(jwk);
  }
  return jwkToRSAPublicKey(jwk);
}


Try<pair<std::string, RSA_shared_ptr>> jwkToRSA(const JSON::Object& jwk)
{
  auto kty = findString(jwk, "kty");
  if (kty.isError()) {
    return Error("Failed to convert JWK to RSA key: " + kty.error());
  }

  auto kid = findString(jwk, "kid");
  if (kid.isError()) {
    return Error("Failed to convert JWK to RSA key: " + kid.error());
  }

  if (kty.get() == "RSA") {
    auto rsaKey = jwkToRSAKey(jwk);
    if (rsaKey.isError()) {
      return Error(rsaKey.error());
    }
    return make_pair(kid.get(), rsaKey.get());
  }

  return Error("Unsupported key type: " + kty.get());
}


Try<map<string, RSA_shared_ptr>> jwkSetToRSAKeys(const string& jwk)
{
  const Try<JSON::Object> json = JSON::parse<JSON::Object>(jwk);
  if (json.isError()) {
    return Error("Failed to parse into JSON: " + json.error());
  }

  auto keys_json = json->find<JSON::Value>("keys");
  if (keys_json.isNone()) {
    return Error("Failed to locate 'keys' in JWK");
  }

  if (!keys_json->is<JSON::Array>()) {
    return Error("Token 'keys' is not an array");
  }

  auto keys = keys_json->as<JSON::Array>().values;

  map<string, RSA_shared_ptr> rsaKeysByKid;
  foreach (const JSON::Value& key_json, keys) {
    if (!key_json.is<JSON::Object>()) {
      return Error("'keys' must contain objects only");
    }

    auto rsaKey = jwkToRSA(key_json.as<JSON::Object>());

    if (rsaKey.isError()) {
      return Error("Error while creating RSA key: " + rsaKey.error());
    }
    rsaKeysByKid.insert(rsaKey.get());
  }
  return rsaKeysByKid;
}

} // namespace authentication {
} // namespace http {
} // namespace process {
