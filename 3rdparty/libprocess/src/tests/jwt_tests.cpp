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

#include <gtest/gtest.h>

#include <string>

#include <process/jwt.hpp>

#include <process/ssl/utilities.hpp>

#include <stout/base64.hpp>
#include <stout/gtest.hpp>
#include <stout/stringify.hpp>
#include <stout/strings.hpp>

using process::http::authentication::JWT;
using process::http::authentication::JWTError;

using process::network::openssl::generate_hmac_sha256;

using std::string;


TEST(JWTTest, Parse)
{
  const string secret = "secret";

  auto create_token = [secret](string header, string payload) {
    header = base64::encode_url_safe(header, false);
    payload = base64::encode_url_safe(payload, false);

    const string mac =
      generate_hmac_sha256(strings::join(".", header, payload), secret).get();

    const string signature = base64::encode_url_safe(mac, false);

    return strings::join(".", header, payload, signature);
  };

  // Invalid token header.
  {
    const string token = create_token(
        "NOT A VALID HEADER",
        "{\"exp\":9999999999,\"sub\":\"foo\"}");

    const Try<JWT, JWTError> jwt = JWT::parse(token, secret);

    EXPECT_ERROR(jwt);
  }

  // Invalid token payload.
  {
    const string token = create_token(
        "{\"alg\":\"HS256\",\"typ\":\"JWT\"}",
        "INVALID PAYLOAD");

    const Try<JWT, JWTError> jwt = JWT::parse(token, secret);

    EXPECT_ERROR(jwt);
  }

  // Unsupported token alg.
  {
    const string token = create_token(
        "{\"alg\":\"RS256\",\"typ\":\"JWT\"}",
        "{\"exp\":9999999999,\"sub\":\"foo\"}");

    const Try<JWT, JWTError> jwt = JWT::parse(token, secret);

    EXPECT_ERROR(jwt);
  }

  // Unknown token alg.
  {
    const string token = create_token(
        "{\"alg\":\"NOT A VALID ALG\",\"typ\":\"JWT\"}",
        "{\"exp\":9999999999,\"sub\":\"foo\"}");

    const Try<JWT, JWTError> jwt = JWT::parse(token, secret);

    EXPECT_ERROR(jwt);
  }

  // 'crit' in header.
  {
    const string token = create_token(
        "{\"alg\":\"HS256\",\"crit\":[\"exp\"],\"exp\":99,\"typ\":\"JWT\"}",
        "{\"exp\":9999999999,\"sub\":\"foo\"}");

    const Try<JWT, JWTError> jwt = JWT::parse(token, secret);

    EXPECT_ERROR(jwt);
  }

  // Missing signature.
  {
    const string token =
      base64::encode_url_safe("{\"alg\":\"HS256\",\"typ\":\"JWT\"}", false) +
      "." +
      base64::encode_url_safe("{\"exp\":9999999999,\"sub\":\"foo\"}", false) +
      ".";

    const Try<JWT, JWTError> jwt = JWT::parse(token, secret);

    EXPECT_ERROR(jwt);
  }

  // Wrong signature.
  {
    const string token =
      base64::encode_url_safe("{\"alg\":\"HS256\",\"typ\":\"JWT\"}", false) +
      "." +
      base64::encode_url_safe("{\"exp\":9999999999,\"sub\":\"foo\"}", false) +
      "." +
      "rm4sQe5q4sdHIJgt7mjKsnuZeP4eRquoZuncSsscqbQ";

    const Try<JWT, JWTError> jwt = JWT::parse(token, secret);

    EXPECT_ERROR(jwt);
  }

  // 'none' alg with signature.
  {
    const string token = create_token(
        "{\"alg\":\"none\",\"typ\":\"JWT\"}",
        "{\"exp\":9999999999,\"sub\":\"foo\"}");

    const Try<JWT, JWTError> jwt = JWT::parse(token);

    EXPECT_ERROR(jwt);
  }

  // 'none' alg missing dot.
  {
    const string token =
      base64::encode_url_safe("{\"alg\":\"none\",\"typ\":\"JWT\"}", false) +
      "." +
      base64::encode_url_safe("{\"exp\":9999999999,\"sub\":\"foo\"}", false);

    const Try<JWT, JWTError> jwt = JWT::parse(token);

    EXPECT_ERROR(jwt);
  }

  // Expiration date is not a number.
  {
    const string token = create_token(
        "{\"alg\":\"HS256\",\"typ\":\"JWT\"}",
        "{\"exp\":\"NOT A NUMBER\",\"sub\":\"foo\"}");

    const Try<JWT, JWTError> jwt = JWT::parse(token, secret);

    EXPECT_ERROR(jwt);
  }

  // Expiration date expired.
  {
    const string token = create_token(
        "{\"alg\":\"HS256\",\"typ\":\"JWT\"}",
        "{\"exp\":0,\"sub\":\"foo\"}");

    const Try<JWT, JWTError> jwt = JWT::parse(token, secret);

    EXPECT_ERROR(jwt);
  }

  // Expiration date not set.
  {
    const string token = create_token(
        "{\"alg\":\"HS256\",\"typ\":\"JWT\"}",
        "{\"sub\":\"foo\"}");

    const Try<JWT, JWTError> jwt = JWT::parse(token, secret);

    // TODO(nfnt): Change this to `EXPECT_SOME(jwt)`
    // once MESOS-7220 is resolved.
    EXPECT_TRUE(jwt.isSome());
  }

  // Valid unsecure token.
  {
    const string token =
      base64::encode_url_safe("{\"alg\":\"none\",\"typ\":\"JWT\"}", false) +
      "." +
      base64::encode_url_safe("{\"exp\":9999999999,\"sub\":\"foo\"}", false) +
      ".";

    const Try<JWT, JWTError> jwt = JWT::parse(token);

    // TODO(nfnt): Change this to `EXPECT_SOME(jwt)`
    // once MESOS-7220 is resolved.
    EXPECT_TRUE(jwt.isSome());
  }

  // Valid HS256 token.
  {
    const string token = create_token(
        "{\"alg\":\"HS256\",\"typ\":\"JWT\"}",
        "{\"exp\":9999999999,\"sub\":\"foo\"}");

    const Try<JWT, JWTError> jwt = JWT::parse(token, secret);

    // TODO(nfnt): Change this to `EXPECT_SOME(jwt)`
    // once MESOS-7220 is resolved.
    EXPECT_TRUE(jwt.isSome());
  }
}


TEST(JWTTest, Create)
{
  const string secret = "secret";

  auto create_signature = [secret](const JSON::Object& payload) {
    const string message = strings::join(
        ".",
        base64::encode_url_safe("{\"alg\":\"HS256\",\"typ\":\"JWT\"}", false),
        base64::encode_url_safe(stringify(payload), false));

    const string mac = generate_hmac_sha256(message, secret).get();

    return base64::encode_url_safe(mac, false);
  };

  JSON::Object payload;
  payload.values["exp"] = 9999999999;
  payload.values["sub"] = "foo";

  // HS256 signed JWT.
  {
    const Try<JWT, JWTError> jwt = JWT::create(payload, secret);

    // TODO(nfnt): Change this to `EXPECT_SOME(jwt)`
    // once MESOS-7220 is resolved.
    EXPECT_TRUE(jwt.isSome());

    EXPECT_EQ(JWT::Alg::HS256, jwt->header.alg);
    EXPECT_SOME_EQ("JWT", jwt->header.typ);

    EXPECT_EQ(payload, jwt->payload);

    EXPECT_SOME_EQ(create_signature(payload), jwt->signature);
  }

  // Unsecured JWT.
  {
    const Try<JWT, JWTError> jwt = JWT::create(payload);

    // TODO(nfnt): Change this to `EXPECT_SOME(jwt)`
    // once MESOS-7220 is resolved.
    EXPECT_TRUE(jwt.isSome());

    EXPECT_EQ(JWT::Alg::None, jwt->header.alg);
    EXPECT_SOME_EQ("JWT", jwt->header.typ);

    EXPECT_EQ(payload, jwt->payload);

    EXPECT_NONE(jwt->signature);
  }
}


TEST(JWTTest, Stringify)
{
  JSON::Object payload;
  payload.values["exp"] = 9999999999;
  payload.values["sub"] = "foo";

  const Try<JWT, JWTError> jwt = JWT::create(payload, "secret");

  // TODO(nfnt): Change this to `EXPECT_SOME(jwt)`
  // once MESOS-7220 is resolved.
  EXPECT_TRUE(jwt.isSome());

  const string token = stringify(jwt.get());

  EXPECT_EQ(
      "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9."
      "eyJleHAiOjk5OTk5OTk5OTksInN1YiI6ImZvbyJ9."
      "7dwSK1mIRKqJTPQT8-AGnI-r8nnefw2hhai3kgBg7bs",
      token);
}
