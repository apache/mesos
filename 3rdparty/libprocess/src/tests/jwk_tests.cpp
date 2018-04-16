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

#include <openssl/err.h>
#include <openssl/rsa.h>

#include <process/jwk.hpp>
#include <process/ssl/utilities.hpp>

#include <stout/gtest.hpp>

#include <process/jwt.hpp>


using namespace process::http::authentication;

using namespace std;

using process::network::openssl::RSA_shared_ptr;

bool isValidRSAPublicKeyOnly(RSA *rsa) {
  if (!rsa || rsa->d || !rsa->n || !rsa->e) {
      return false;
  }
  return BN_is_odd(rsa->e) && !BN_is_one(rsa->e);
}

TEST(JWKTest, BadJWKSet)
{
  // Invalid JWK Set
  {
    auto jwk = "{\"id\":\"test-jwk\",\"abc\":\"\"";
    auto rsaKeyByKid = jwkSetToRSAKeys(jwk);
    EXPECT_ERROR(rsaKeyByKid);
  }

  // JWK Set not having 'keys' key
  {
    auto jwk = "{\"id\":\"test-jwk\"}";
    auto rsaKeyByKid = jwkSetToRSAKeys(jwk);
    EXPECT_ERROR(rsaKeyByKid);
  }

  // JWK Set containing 'keys' that is not an array
  {
    auto jwk = "{\"id\":\"test-jwk\",\"keys\":\"string\"}";
    auto rsaKeyByKid = jwkSetToRSAKeys(jwk);
    EXPECT_ERROR(rsaKeyByKid);
  }
}

TEST(JWKTest, OneKeyInJWKSet)
{
  // JWK Set containing one key without 'kty'
  {
    auto jwk = "{\"id\":\"test-jwk\",\"keys\":[{\"kid\":\"abc\"}]}";
    auto rsaKeyByKid = jwkSetToRSAKeys(jwk);
    EXPECT_ERROR(rsaKeyByKid);
  }

  // JWK Set containing one key without 'kid'
  {
    auto jwk = "{\"id\":\"test-jwk\",\"keys\":[{\"kty\":\"abc\"}]}";
    auto rsaKeyByKid = jwkSetToRSAKeys(jwk);
    EXPECT_ERROR(rsaKeyByKid);
  }

  // JWK Set containing one key with unsupported key type
  {
    auto jwk = "{\"id\":\"test-jwk\",\"keys\":"
               "[{\"kid\":\"abc\",\"kty\":\"EC\"}]}";
    auto rsaKeyByKid = jwkSetToRSAKeys(jwk);
    EXPECT_ERROR(rsaKeyByKid);
  }

  // JWK Set containing one key with invalid RSA key (missing 'e')
  {
    auto jwk = "{\"id\":\"test-jwk\",\"keys\":"
               "[{\"kid\":\"abc\",\"kty\":\"RSA\",\"n\":\"abc\"}]}";
    auto rsaKeyByKid = jwkSetToRSAKeys(jwk);
    EXPECT_ERROR(rsaKeyByKid);
  }

  // JWK Set containing one key with invalid RSA key (missing 'n')
  {
    auto jwk = "{\"id\":\"test-jwk\",\"keys\":"
               "[{\"kid\":\"abc\",\"kty\":\"RSA\",\"e\":\"abc\"}]}";
    auto rsaKeyByKid = jwkSetToRSAKeys(jwk);
    EXPECT_ERROR(rsaKeyByKid);
  }

  // JWK Set containing one RSA public key with invalid base64 paremeters
  {
    auto jwk = "{\"id\":\"test-jwk\",\"keys\":"
               "[{\"kid\":\"abc\",\"kty\":\"RSA\","
               "\"n\":\"a(bc\",\"e\":\"a)bc\"}]}";
    auto rsaKeyByKid = jwkSetToRSAKeys(jwk);
    EXPECT_ERROR(rsaKeyByKid);
  }

  // JWK Set containing one valid RSA public key
  {
    auto jwkSet = R"({
  "id":"test-jwk",
  "keys":[{
    "kid": "mesos.com",
    "kty": "RSA",
    "use": "sig",
    "n": "ALhQ-ZVQM9gIxRI8yFjMAY7S60DcWl8tsJPWIsIPFDnmCXr5Bt__lFlwBLM7q6ie5av-LkjwG0xAm7cohOHU7xEhZqh6n8CmJPlRbz_E8uFYfW67eP0YmdcS9dDBYn_77t_Ji7L0T2w62k7rE_vZ4k0MoSQnYkRq6uYZoltwaAO_3pab6dPov9HtRcTERHDTlKkNR4WDBZ9zLJKo2UbNoIoJpJ0D1T6CQXQVkFRiGFW-dnd-IZi4b2Dw93-ISR0vpmb0uVuo3pAlyuBwIXgzcTrwROFdXbSC3STyRLMd1Gvdc_CBGmGvIsGzld8no3WVWdzR0sZrawEWAaaOSvQcOI0",
    "e": "AQAB"
  }]
})";
    auto rsaKeyByKid = jwkSetToRSAKeys(jwkSet);
    EXPECT_SOME(rsaKeyByKid);

    RSA_shared_ptr mesosKey = rsaKeyByKid.get()["mesos.com"];
    CHECK_NOTNULL(mesosKey.get());

    // Verify the public key is mathematically valid
    EXPECT_TRUE(isValidRSAPublicKeyOnly(mesosKey.get()));
  }

  // JWK Set containing one valid RSA private key based on all RSA params
  {
    auto jwkSet = R"({
  "id": "some-keys",
  "keys": [{
    "kid": "mesos.com",
    "kty": "RSA",
    "n": "ALhQ-ZVQM9gIxRI8yFjMAY7S60DcWl8tsJPWIsIPFDnmCXr5Bt__lFlwBLM7q6ie5av-LkjwG0xAm7cohOHU7xEhZqh6n8CmJPlRbz_E8uFYfW67eP0YmdcS9dDBYn_77t_Ji7L0T2w62k7rE_vZ4k0MoSQnYkRq6uYZoltwaAO_3pab6dPov9HtRcTERHDTlKkNR4WDBZ9zLJKo2UbNoIoJpJ0D1T6CQXQVkFRiGFW-dnd-IZi4b2Dw93-ISR0vpmb0uVuo3pAlyuBwIXgzcTrwROFdXbSC3STyRLMd1Gvdc_CBGmGvIsGzld8no3WVWdzR0sZrawEWAaaOSvQcOI0",
    "e": "AQAB",
    "d": "bzSD8V-LeBuKc39yzYiApCCDygVpDSXu9LNtEzKv3GL7c1OOn1V_txqL62vkHP-JyOS6Hk2n2rDcgnyS-AJWHzrMynf5rO1RP4-vlIUKmYWfYFECJYpTP110LHiRKnDhZeofPGCFDuLPVnAlBX4nOJ-XFc4hTvBHO39Z4tuGFkQFy5nMz6b24ku29NB3_-bebdpAbsY-tMIeY0-mtH9T3ysKv0OuNfRUvpHGfh_xgyHh1lnS70cuQEqxF46DuIsi0FoU-GOZkPyHQdoSNo1sy8fx4F6EOBa3mvuw3p2JwXWOgHu6oqmfhSSRVy_6JwhC8t9Gx-MBP_Fq05ufHZIMoQ",
    "p": "AOW4429p2EoIXZCWn04JViHKjL9buGP_xPVKdpnVKwyKdI8WgEa15Gu4ok0T4WbGXMumfS2iSCdVcaKACycR0B4favTNFwAmfhcygTNw4yAtCScfJOQR7ic24nTbZG37V_x_6tpoyrgC9H6IGRX63LVJjCpc0WWj-HZUDmCdBZ1J",
    "q": "AM1mbzS42560BHugeJ441KYYdkZWxUErct17FX7R3L2jR2f0Q2myghgxBSDL4oq7twSerL1xJSZ6p6bERwgxNBFvJgd8L4L6nSdXF20Td-RHREbtOg66Rvgmo4EgUVzCr0B8WWWyBeGj-YS-huUEqpSxZul9tKlqiezlavq0uZUl",
    "dp": "ALffsqQWG5q-cW3vMhn7XSb1Ao2Us9XO_u67qIzfVHLYTA3QG-L9apVSlw6M8Ckcc2BKpf2l3I0nViqUxNiD6IqD6U-C7XsgVGLq-QGcxR-XDLF0u0mWlIJs6vxQM2XY_gdMuEYUBNce_mZdN38hahHtibTK0IzDn3fPNibc6IaJ",
    "dq": "THATcHZe3Le3d15npNIXaNxvn4uJCtClhYDZpgFpeXU7DJedQsd4nJIZi3P0kZZ77I80T6e8oI5Ct9ARcx4Ed3x6lYyEjeS_-TTy9dep5V0ULqT31yVBZfXTISmqva-B0qi0CCFxCOCh6eGRh8btyDogx0HNqsKII43Y-wWojrU",
    "qi": "HwH4IZi4eIOcKC_ChC7LgkwCg7bAmGJrAKgSJJOTH0vU5UFcS1qqLpwkShDlFJiVJseEdeu4TjGjj_BiSdFxiMdgvmCeYh7drWDmQSuX39W1bJHgstjFX9-fNOGn5Xh2z6k-6sjPPr1lyl2U4YAWMFqvIWA6MOZokPiW0rW1HUA"
  }]
})";

    auto rsaKeyByKid = jwkSetToRSAKeys(jwkSet);
    EXPECT_SOME(rsaKeyByKid);

    RSA_shared_ptr mesosKey = rsaKeyByKid.get()["mesos.com"];
    CHECK_NOTNULL(mesosKey.get());

    // Verify the key is mathematically valid for OpenSSL
    EXPECT_EQ(1, RSA_check_key(mesosKey.get()));
  }

  // JWK Set containing one valid RSA private key based on partial RSA params
  {
    // We provide only minimal set of parameters to create the private key.
    auto jwkSet = R"({
  "id": "some-keys",
  "keys": [{
    "kid": "mesos.com",
    "kty": "RSA",
    "n": "ALhQ-ZVQM9gIxRI8yFjMAY7S60DcWl8tsJPWIsIPFDnmCXr5Bt__lFlwBLM7q6ie5av-LkjwG0xAm7cohOHU7xEhZqh6n8CmJPlRbz_E8uFYfW67eP0YmdcS9dDBYn_77t_Ji7L0T2w62k7rE_vZ4k0MoSQnYkRq6uYZoltwaAO_3pab6dPov9HtRcTERHDTlKkNR4WDBZ9zLJKo2UbNoIoJpJ0D1T6CQXQVkFRiGFW-dnd-IZi4b2Dw93-ISR0vpmb0uVuo3pAlyuBwIXgzcTrwROFdXbSC3STyRLMd1Gvdc_CBGmGvIsGzld8no3WVWdzR0sZrawEWAaaOSvQcOI0",
    "e": "AQAB",
    "d": "bzSD8V-LeBuKc39yzYiApCCDygVpDSXu9LNtEzKv3GL7c1OOn1V_txqL62vkHP-JyOS6Hk2n2rDcgnyS-AJWHzrMynf5rO1RP4-vlIUKmYWfYFECJYpTP110LHiRKnDhZeofPGCFDuLPVnAlBX4nOJ-XFc4hTvBHO39Z4tuGFkQFy5nMz6b24ku29NB3_-bebdpAbsY-tMIeY0-mtH9T3ysKv0OuNfRUvpHGfh_xgyHh1lnS70cuQEqxF46DuIsi0FoU-GOZkPyHQdoSNo1sy8fx4F6EOBa3mvuw3p2JwXWOgHu6oqmfhSSRVy_6JwhC8t9Gx-MBP_Fq05ufHZIMoQ"
  }]
})";

    auto rsaKeyByKid = jwkSetToRSAKeys(jwkSet);
    EXPECT_SOME(rsaKeyByKid);

    RSA_shared_ptr mesosKey = rsaKeyByKid->at("mesos.com");
    CHECK_NOTNULL(mesosKey.get());
  }
}

TEST(JWKTest, SeveralKeysInJWKSet)
{
  // JWK Set containing one valid RSA private key based on partial RSA params
  {
    // We provide only minimal set of parameters to compute the private key.
    auto jwkSet = R"({
  "id": "some-keys",
  "keys": [{
    "kid": "mesos.com",
    "kty": "RSA",
    "n": "ALhQ-ZVQM9gIxRI8yFjMAY7S60DcWl8tsJPWIsIPFDnmCXr5Bt__lFlwBLM7q6ie5av-LkjwG0xAm7cohOHU7xEhZqh6n8CmJPlRbz_E8uFYfW67eP0YmdcS9dDBYn_77t_Ji7L0T2w62k7rE_vZ4k0MoSQnYkRq6uYZoltwaAO_3pab6dPov9HtRcTERHDTlKkNR4WDBZ9zLJKo2UbNoIoJpJ0D1T6CQXQVkFRiGFW-dnd-IZi4b2Dw93-ISR0vpmb0uVuo3pAlyuBwIXgzcTrwROFdXbSC3STyRLMd1Gvdc_CBGmGvIsGzld8no3WVWdzR0sZrawEWAaaOSvQcOI0",
    "e": "AQAB",
    "d": "bzSD8V-LeBuKc39yzYiApCCDygVpDSXu9LNtEzKv3GL7c1OOn1V_txqL62vkHP-JyOS6Hk2n2rDcgnyS-AJWHzrMynf5rO1RP4-vlIUKmYWfYFECJYpTP110LHiRKnDhZeofPGCFDuLPVnAlBX4nOJ-XFc4hTvBHO39Z4tuGFkQFy5nMz6b24ku29NB3_-bebdpAbsY-tMIeY0-mtH9T3ysKv0OuNfRUvpHGfh_xgyHh1lnS70cuQEqxF46DuIsi0FoU-GOZkPyHQdoSNo1sy8fx4F6EOBa3mvuw3p2JwXWOgHu6oqmfhSSRVy_6JwhC8t9Gx-MBP_Fq05ufHZIMoQ"
  }, {
    "kid": "mesos2.com",
    "kty": "RSA",
    "n": "AODuL8Bb2xhY69Ctr4dLtDHi0poz74b_KaWgas7ZP807nIP1pNF23VMLDPpRuHmnnUdvDB2GOMuDP8tdj8JdUzFCjyzYe9UTMB1VQg7RpSih9DceegSMxo6hipEqUmVpE-TZPrcBGq0Faf-iXGBgJ2ad6RvMkWlSyJeg_nwYqJxP3zVdIO2FowaNVBlk00QQaBZQypO4Itv82u__QPEVJ0PVljHnPznCa0kqBTGzIAUrzHCGxrgtJLmefWLRTnO9_-GDmPJs-yVGSFSK8IhWb4xh8FgMkT-TdkX6DDUdsVQ24nUelf6JpUxEaEHRc30TES3qOfDNyMJA2QKXsCedd6k",
    "e": "AQAB",
    "d": "ANaHFhAXC84a8T6kiSc3MvPpbAgaxLcyolwPtg728X0i_9Jz9PC6t7i-b3BHhPSywrUg2qNGIuEnmy6xW617KR9wZfHVv7WniVpQuKI9nZI1dSEk9idkxPPAatKtVMzX_VtlQAV3DiQ7Z6-jAQwCaVHcBjq3T3DuvdawfEeLlTUOxp4fokUbC4upFVXpIDHpn4aEA88xpmdUyltLvQhUCdoXyCxp8-5x4lmarUaKAxs785y6qeiQ8Dno2UmO5pDHfINxCiS7mch5CGskUm8JUkBP_ICipanScejh8_lOnYm5UX0zm6Q4Zvbr2u0z8Mbbjfwf2_V1nzQsj0u8b7YrlCk"
}]
})";

    auto rsaKeyByKid = jwkSetToRSAKeys(jwkSet);
    EXPECT_SOME(rsaKeyByKid);
    EXPECT_TRUE(rsaKeyByKid->size() == 2);

    RSA_shared_ptr mesosKey = rsaKeyByKid->at("mesos.com");
    RSA_shared_ptr mesos2Key = rsaKeyByKid->at("mesos2.com");
    CHECK_NOTNULL(mesosKey.get());
    CHECK_NOTNULL(mesos2Key.get());
  }
}
