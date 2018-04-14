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

#include "jwt_keys.hpp"

using process::http::authentication::JWT;
using process::http::authentication::JWTError;

using process::network::openssl::generate_hmac_sha256;
using process::network::openssl::sign_rsa_sha256;
using process::network::openssl::RSA_shared_ptr;
using process::network::openssl::pemToRSAPrivateKey;
using process::network::openssl::pemToRSAPublicKey;

using std::string;

template<typename SignatureBuilder>
string create_token(string header, string payload,
  SignatureBuilder signatureBuilder) {
  header = base64::encode_url_safe(header, false);
  payload = base64::encode_url_safe(payload, false);

  const string rawSignature = signatureBuilder(header, payload);
  const string signature = base64::encode_url_safe(rawSignature, false);

  return strings::join(".", header, payload, signature);
}

std::function<string(string, string)> create_hs256_token_generator(
  string secret) {
  auto generate_mac = [secret](string header, string payload) {
    return generate_hmac_sha256(
      strings::join(".", header, payload), secret).get();
  };
  return [generate_mac](string header, string payload) {
    return create_token(header, payload, generate_mac);
  };
}

std::function<string(string, string)> create_rs256_token_generator(
  RSA_shared_ptr privateKey) {
  auto generate_rsa = [privateKey](string header, string payload) {
    return sign_rsa_sha256(
        strings::join(".", header, payload), privateKey).get();
  };

  return [generate_rsa](string header, string payload) {
    return create_token(header, payload, generate_rsa);
  };
}

TEST(JWTTest, Parse)
{
  const string secret = "secret";

  Try<RSA_shared_ptr> privateKey = pemToRSAPrivateKey(PRIVATE_KEY);
  Try<RSA_shared_ptr> publicKey = pemToRSAPublicKey(PUBLIC_KEY);

  auto create_hs256_token = create_hs256_token_generator(secret);
  auto create_rs256_token = create_rs256_token_generator(privateKey.get());

  // Invalid token header.
  {
    // HS256
    {
      const string token = create_hs256_token(
          "NOT A VALID HEADER",
          "{\"exp\":9999999999,\"sub\":\"foo\"}");

      const Try<JWT, JWTError> jwt = JWT::parse(token, secret);

      EXPECT_ERROR(jwt);
    }
    // RS256
    {
      const string token = create_rs256_token(
          "NOT A VALID HEADER",
          "{\"exp\":9999999999,\"sub\":\"foo\"}");

      const Try<JWT, JWTError> jwt = JWT::parse(token, publicKey.get());

      EXPECT_ERROR(jwt);
    }
  }

  // Invalid token payload.
  {
    // HS256
    {
      const string token = create_hs256_token(
          "{\"alg\":\"HS256\",\"typ\":\"JWT\"}",
          "INVALID PAYLOAD");

      const Try<JWT, JWTError> jwt = JWT::parse(token, secret);

      EXPECT_ERROR(jwt);
    }
    // RS256
    {
      const string token = create_rs256_token(
          "{\"alg\":\"RS256\",\"typ\":\"JWT\"}",
          "INVALID PAYLOAD");

      const Try<JWT, JWTError> jwt = JWT::parse(token, publicKey.get());

      EXPECT_ERROR(jwt);
    }
  }

  // Unsupported token alg.
  {
    // HS256
    {
      const string token = create_hs256_token(
          "{\"alg\":\"RS512\",\"typ\":\"JWT\"}",
          "{\"exp\":9999999999,\"sub\":\"foo\"}");

      const Try<JWT, JWTError> jwt = JWT::parse(token, secret);

      EXPECT_ERROR(jwt);
    }
    // RS256
    {
      const string token = create_rs256_token(
          "{\"alg\":\"RS512\",\"typ\":\"JWT\"}",
          "{\"exp\":9999999999,\"sub\":\"foo\"}");

      const Try<JWT, JWTError> jwt = JWT::parse(token, publicKey.get());

      EXPECT_ERROR(jwt);
    }
  }

  // Unknown token alg.
  {
    // HS256
    {
      const string token = create_hs256_token(
          "{\"alg\":\"NOT A VALID ALG\",\"typ\":\"JWT\"}",
          "{\"exp\":9999999999,\"sub\":\"foo\"}");

      const Try<JWT, JWTError> jwt = JWT::parse(token, secret);

      EXPECT_ERROR(jwt);
    }
    // RS256
    {
      const string token = create_rs256_token(
          "{\"alg\":\"NOT A VALID ALG\",\"typ\":\"JWT\"}",
          "{\"exp\":9999999999,\"sub\":\"foo\"}");

      const Try<JWT, JWTError> jwt = JWT::parse(token, publicKey.get());

      EXPECT_ERROR(jwt);
    }
  }

  // 'crit' in header.
  {
    // HS256
    {
      const string token = create_hs256_token(
          "{\"alg\":\"HS256\",\"crit\":[\"exp\"],\"exp\":99,\"typ\":\"JWT\"}",
          "{\"exp\":9999999999,\"sub\":\"foo\"}");

      const Try<JWT, JWTError> jwt = JWT::parse(token, secret);

      EXPECT_ERROR(jwt);
    }
    // RS256
    {
      const string token = create_rs256_token(
          "{\"alg\":\"RS256\",\"crit\":[\"exp\"],\"exp\":99,\"typ\":\"JWT\"}",
          "{\"exp\":9999999999,\"sub\":\"foo\"}");

      const Try<JWT, JWTError> jwt = JWT::parse(token, publicKey.get());

      EXPECT_ERROR(jwt);
    }
  }

  // Missing signature.
  {
    // HS256
    {
      const string token =
        base64::encode_url_safe("{\"alg\":\"HS256\",\"typ\":\"JWT\"}", false) +
        "." +
        base64::encode_url_safe("{\"exp\":9999999999,\"sub\":\"foo\"}", false) +
        ".";

      const Try<JWT, JWTError> jwt = JWT::parse(token, secret);

      EXPECT_ERROR(jwt);
    }
    // RS256
    {
      const string token =
        base64::encode_url_safe("{\"alg\":\"RS256\",\"typ\":\"JWT\"}", false) +
        "." +
        base64::encode_url_safe("{\"exp\":9999999999,\"sub\":\"foo\"}", false) +
        ".";

      const Try<JWT, JWTError> jwt = JWT::parse(token, publicKey.get());

      EXPECT_ERROR(jwt);
    }
  }

  // Wrong signature.
  {
    // HS256
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
    // RS256
    {
      const string token =
        base64::encode_url_safe("{\"alg\":\"RS256\",\"typ\":\"JWT\"}", false) +
        "." +
        base64::encode_url_safe("{\"exp\":9999999999,\"sub\":\"foo\"}", false) +
        "." +
        "rm4sQe5q4sdHIJgt7mjKsnuZeP4eRquoZuncSsscqbQ";

      const Try<JWT, JWTError> jwt = JWT::parse(token, publicKey.get());

      EXPECT_ERROR(jwt);
    }
  }

  // 'none' alg with signature.
  {
    const string token = create_hs256_token(
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
    // HS256
    {
      const string token = create_hs256_token(
          "{\"alg\":\"HS256\",\"typ\":\"JWT\"}",
          "{\"exp\":\"NOT A NUMBER\",\"sub\":\"foo\"}");

      const Try<JWT, JWTError> jwt = JWT::parse(token, secret);

      EXPECT_ERROR(jwt);
    }
    // RS256
    {
      const string token = create_rs256_token(
          "{\"alg\":\"RS256\",\"typ\":\"JWT\"}",
          "{\"exp\":\"NOT A NUMBER\",\"sub\":\"foo\"}");

      const Try<JWT, JWTError> jwt = JWT::parse(token, publicKey.get());

      EXPECT_ERROR(jwt);
    }
  }

  // Expiration date expired.
  {
    // HS256
    {
      const string token = create_hs256_token(
          "{\"alg\":\"HS256\",\"typ\":\"JWT\"}",
          "{\"exp\":0,\"sub\":\"foo\"}");

      const Try<JWT, JWTError> jwt = JWT::parse(token, secret);

      EXPECT_ERROR(jwt);
    }
    // RS256
    {
      const string token = create_rs256_token(
          "{\"alg\":\"RS256\",\"typ\":\"JWT\"}",
          "{\"exp\":0,\"sub\":\"foo\"}");

      const Try<JWT, JWTError> jwt = JWT::parse(token, publicKey.get());

      EXPECT_ERROR(jwt);
    }
  }

  // Expiration date not set.
  {
    // HS256
    {
      const string token = create_hs256_token(
          "{\"alg\":\"HS256\",\"typ\":\"JWT\"}",
          "{\"sub\":\"foo\"}");

      const Try<JWT, JWTError> jwt = JWT::parse(token, secret);

      // TODO(nfnt): Change this to `EXPECT_SOME(jwt)`
      // once MESOS-7220 is resolved.
      EXPECT_TRUE(jwt.isSome());
    }
    // RS256
    {
      const string token = create_rs256_token(
          "{\"alg\":\"RS256\",\"typ\":\"JWT\"}",
          "{\"sub\":\"foo\"}");

      const Try<JWT, JWTError> jwt = JWT::parse(token, publicKey.get());

      // TODO(nfnt): Change this to `EXPECT_SOME(jwt)`
      // once MESOS-7220 is resolved.
      EXPECT_TRUE(jwt.isSome());
    }
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
    const string token = create_hs256_token(
        "{\"alg\":\"HS256\",\"typ\":\"JWT\"}",
        "{\"exp\":9999999999,\"sub\":\"foo\"}");

    const Try<JWT, JWTError> jwt = JWT::parse(token, secret);

    // TODO(nfnt): Change this to `EXPECT_SOME(jwt)`
    // once MESOS-7220 is resolved.
    EXPECT_TRUE(jwt.isSome());
  }

  // Valid RS256 token.
  {
    const string token = create_rs256_token(
        "{\"alg\":\"RS256\",\"typ\":\"JWT\"}",
        "{\"exp\":9999999999,\"sub\":\"foo\"}");

    const Try<JWT, JWTError> jwt = JWT::parse(token, publicKey.get());

    EXPECT_TRUE(jwt.isSome());
  }
}


TEST(JWTTest, Create)
{
  const string secret = "secret";
  Try<RSA_shared_ptr> privateKey = pemToRSAPrivateKey(PRIVATE_KEY);

  auto create_hs256_signature = [secret](const JSON::Object& payload) {
    const string message = strings::join(
        ".",
        base64::encode_url_safe("{\"alg\":\"HS256\",\"typ\":\"JWT\"}", false),
        base64::encode_url_safe(stringify(payload), false));

    const string mac = generate_hmac_sha256(message, secret).get();

    return base64::encode_url_safe(mac, false);
  };

  auto create_rs256_signature = [privateKey](const JSON::Object& payload) {
    const string message = strings::join(
        ".",
        base64::encode_url_safe("{\"alg\":\"RS256\",\"typ\":\"JWT\"}", false),
        base64::encode_url_safe(stringify(payload), false));

    const string mac = sign_rsa_sha256(message, privateKey.get()).get();

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

    EXPECT_SOME_EQ(create_hs256_signature(payload), jwt->signature);
  }

  // RS256 signed JWT.
  {
    const Try<JWT, JWTError> jwt = JWT::create(payload, privateKey.get());

    // TODO(nfnt): Change this to `EXPECT_SOME(jwt)`
    // once MESOS-7220 is resolved.
    EXPECT_TRUE(jwt.isSome());

    EXPECT_EQ(JWT::Alg::RS256, jwt->header.alg);
    EXPECT_SOME_EQ("JWT", jwt->header.typ);

    EXPECT_EQ(payload, jwt->payload);

    EXPECT_SOME_EQ(create_rs256_signature(payload), jwt->signature);
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
  // HS256
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
  // RS256
  {
    Try<RSA_shared_ptr> privateKey = pemToRSAPrivateKey(PRIVATE_KEY);
    JSON::Object payload;
    payload.values["exp"] = 9999999999;
    payload.values["sub"] = "foo";

    const Try<JWT, JWTError> jwt = JWT::create(payload, privateKey.get());

    // TODO(nfnt): Change this to `EXPECT_SOME(jwt)`
    // once MESOS-7220 is resolved.
    EXPECT_TRUE(jwt.isSome());

    const string token = stringify(jwt.get());

    EXPECT_EQ(
        "eyJhbGciOiJSUzI1NiIsInR5cCI6IkpXVCJ9."
        "eyJleHAiOjk5OTk5OTk5OTksInN1YiI6ImZvbyJ9."
        "kNVM5tQWMHCsKzxgoUYrQ-oNrRDaTdnXXT01_Kf3DG9rGAWegQ1GC9H"
        "iKJr0Nces_C7kDg3xhg0TAKc4sumlRHnQf40Y6P6NGAw__71qTvCptb"
        "NS97sQypPeI7iFGcZGg-WfO2e1u0ztbZZi0PnrSO_5TL4qPXNE0UZTw"
        "Si3f8nOPbBoIDdXHZKBWDVbP7evgcsSTeg26i0kwNI3SMLFa0nUt3rw"
        "BVflxaAPK2PDD16s6hEmg0EB9MXHXYQGmh2Q01G5o7XKWsAe5H46CWD"
        "LnJFpU3NN4iGd4EkbN_wPjOQ0FjlzypCTqF0QRM0Stf219qwVIw4_rt"
        "j8V4bZUdp-wg",
        token);
  }
}
