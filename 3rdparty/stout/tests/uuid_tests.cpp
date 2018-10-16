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

#include <string>

#include <gtest/gtest.h>

#include <gmock/gmock.h>

#include <stout/check.hpp>
#include <stout/gtest.hpp>
#include <stout/uuid.hpp>

using id::UUID;

using std::string;


TEST(UUIDTest, Test)
{
  UUID uuid1 = UUID::random();
  UUID uuid2 = UUID::fromBytes(uuid1.toBytes()).get();
  UUID uuid3 = uuid2;

  EXPECT_EQ(uuid1, uuid2);
  EXPECT_EQ(uuid2, uuid3);
  EXPECT_EQ(uuid1, uuid3);

  string bytes1 = uuid1.toBytes();
  string bytes2 = uuid2.toBytes();
  string bytes3 = uuid3.toBytes();

  EXPECT_EQ(bytes1, bytes2);
  EXPECT_EQ(bytes2, bytes3);
  EXPECT_EQ(bytes1, bytes3);

  string string1 = uuid1.toString();
  string string2 = uuid2.toString();
  string string3 = uuid3.toString();

  EXPECT_EQ(string1, string2);
  EXPECT_EQ(string2, string3);
  EXPECT_EQ(string1, string3);

  EXPECT_SOME_EQ(uuid1, UUID::fromString(string1));
  EXPECT_SOME_EQ(uuid2, UUID::fromString(string2));
  EXPECT_SOME_EQ(uuid3, UUID::fromString(string3));
}


TEST(UUIDTest, Metadata)
{
  UUID uuid = UUID::random();

  EXPECT_EQ(16u, uuid.size());
  EXPECT_EQ(UUID::variant_rfc_4122, uuid.variant());
  EXPECT_EQ(UUID::version_random_number_based, uuid.version());
}


TEST(UUIDTest, MalformedUUID)
{
  EXPECT_SOME(UUID::fromBytes(UUID::random().toBytes()));
  EXPECT_ERROR(UUID::fromBytes("malformed-uuid"));
  EXPECT_ERROR(UUID::fromBytes("invalidstringmsg"));
  EXPECT_SOME(UUID::fromString(UUID::random().toString()));
  EXPECT_ERROR(UUID::fromString("malformed-uuid"));
}
