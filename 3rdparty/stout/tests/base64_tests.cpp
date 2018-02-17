// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//  http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <gtest/gtest.h>

#include <stout/base64.hpp>
#include <stout/gtest.hpp>


TEST(Base64Test, Encode)
{
  EXPECT_EQ("dXNlcjpwYXNzd29yZA==", base64::encode("user:password"));
}


TEST(Base64Test, Decode)
{
  // We're able to parse without padding.
  EXPECT_SOME_EQ("user:password", base64::decode("dXNlcjpwYXNzd29yZA=="));
  EXPECT_SOME_EQ("user:password", base64::decode("dXNlcjpwYXNzd29yZA="));
  EXPECT_SOME_EQ("user:password", base64::decode("dXNlcjpwYXNzd29yZA"));

  // Whitespaces are allowed.
  EXPECT_SOME_EQ("user:password", base64::decode("d XNlcjpwYXNzd29yZA=="));
  EXPECT_SOME_EQ("user:password", base64::decode("d\tXNlcjpwYXNzd29yZA=="));
  EXPECT_SOME_EQ("user:password", base64::decode("d\nXNlcjpwYXNzd29yZA=="));
  EXPECT_SOME_EQ("user:password", base64::decode("d\vXNlcjpwYXNzd29yZA=="));
  EXPECT_SOME_EQ("user:password", base64::decode("d\fXNlcjpwYXNzd29yZA=="));
  EXPECT_SOME_EQ("user:password", base64::decode("d\rXNlcjpwYXNzd29yZA=="));
  EXPECT_SOME_EQ("user:password", base64::decode("d\r\nXNlcjpwYXNzd29yZA=="));

  // Invalid characters.
  EXPECT_ERROR(base64::decode("abc("));
  EXPECT_ERROR(base64::decode(">abc"));
  EXPECT_ERROR(base64::decode("ab,="));

  // These cases are not currently validated!
  //  EXPECT_ERROR(base64::decode("ab="));
  //  EXPECT_ERROR(base64::decode("ab=,"));
  //  EXPECT_ERROR(base64::decode("ab==="));
}


TEST(Base64Test, EncodeURLSafe)
{
  EXPECT_EQ(
      "dXNlcjpwYXNzd29yZH5-fg",
      base64::encode_url_safe("user:password~~~", false));

  EXPECT_EQ(
      "dXNlcjpwYXNzd29yZH5-fg==",
      base64::encode_url_safe("user:password~~~", true));

  EXPECT_EQ(
      u8"fn5-w7_Dv8O_w78",
      base64::encode_url_safe(u8"~~~\u00ff\u00ff\u00ff\u00ff", false));

  EXPECT_EQ(
      u8"fn5-w7_Dv8O_w78=",
      base64::encode_url_safe(u8"~~~\u00ff\u00ff\u00ff\u00ff", true));
}


TEST(Base64Test, DecodeURLSafe)
{
  EXPECT_SOME_EQ(
      "user:password~~~",
      base64::decode_url_safe("dXNlcjpwYXNzd29yZH5-fg"));

  EXPECT_SOME_EQ(
      "user:password~~~",
      base64::decode_url_safe("dXNlcjpwYXNzd29yZH5-fg=="));

  EXPECT_SOME_EQ(
      u8"~~~\u00ff\u00ff\u00ff\u00ff",
      base64::decode_url_safe(u8"fn5-w7_Dv8O_w78"));

  EXPECT_SOME_EQ(
      u8"~~~\u00ff\u00ff\u00ff\u00ff",
      base64::decode_url_safe(u8"fn5-w7_Dv8O_w78="));
}
