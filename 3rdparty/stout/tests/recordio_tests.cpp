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

#include <deque>
#include <string>

#include <gtest/gtest.h>

#include <stout/error.hpp>
#include <stout/gtest.hpp>
#include <stout/recordio.hpp>
#include <stout/some.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>

using std::deque;
using std::string;


template <typename T>
bool operator==(Try<T> lhs, Try<T> rhs)
{
  if (lhs.isSome() != rhs.isSome()) {
    return false;
  }

  if (lhs.isSome()) {
    return lhs.get() == rhs.get();
  }

  return lhs.error() == rhs.error();
}


template <typename T>
bool operator!=(Try<T> lhs, Try<T> rhs)
{
  return !(lhs == rhs);
}


template <typename T>
bool operator==(deque<T> rhs, deque<T> lhs)
{
  if (rhs.size() != lhs.size()) {
    return false;
  }

  auto it1 = rhs.begin();
  auto it2 = lhs.begin();

  while (it1 != rhs.end()) {
    if (*it1 != *it2) {
      return false;
    }

    ++it1;
    ++it2;
  }

  return true;
}


TEST(RecordIOTest, Encoder)
{
  recordio::Encoder<string> encoder(strings::upper);

  string data;

  data += encoder.encode("hello!");
  data += encoder.encode("");
  data += encoder.encode(" ");
  data += encoder.encode("13 characters");

  EXPECT_EQ(
      "6\nHELLO!"
      "0\n"
      "1\n "
      "13\n13 CHARACTERS",
      data);

  // Make sure these can be decoded.
  recordio::Decoder<string> decoder(
      [=](const string& data) {
        return Try<string>(strings::lower(data));
      });

  deque<Try<string>> records;
  records.push_back("hello!");
  records.push_back("");
  records.push_back(" ");
  records.push_back("13 characters");

  EXPECT_SOME_EQ(records, decoder.decode(data));
}


TEST(RecordIOTest, Decoder)
{
  // Deserializing brings to lower case, but add an
  // error case to test deserialization failures.
  auto deserialize = [](const string& data) -> Try<string> {
    if (data == "error") {
      return Error("error");
    }
    return strings::lower(data);
  };

  recordio::Decoder<string> decoder(deserialize);

  deque<Try<string>> records;

  // Empty data should not result in an error.
  records.clear();

  EXPECT_SOME_EQ(records, decoder.decode(""));

  // Should decode more than 1 record when possible.
  records.clear();
  records.push_back("hello!");
  records.push_back("");
  records.push_back(" ");

  EXPECT_SOME_EQ(records, decoder.decode("6\nHELLO!0\n1\n "));

  // An entry which cannot be decoded should not
  // fail the decoder permanently.
  records.clear();
  records.push_back(Error("error"));

  EXPECT_SOME_EQ(records, decoder.decode("5\nerror"));

  // Record should only be decoded once complete.
  records.clear();

  EXPECT_SOME_EQ(records, decoder.decode("1"));
  EXPECT_SOME_EQ(records, decoder.decode("3"));
  EXPECT_SOME_EQ(records, decoder.decode("\n"));
  EXPECT_SOME_EQ(records, decoder.decode("13 CHARACTER"));

  records.clear();
  records.push_back("13 characters");

  EXPECT_SOME_EQ(records, decoder.decode("S"));

  // If the format is bad, the decoder should fail permanently.
  EXPECT_ERROR(decoder.decode("not a number\n"));
  EXPECT_ERROR(decoder.decode("1\n"));
}
