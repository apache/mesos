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

#include <stout/gtest.hpp>
#include <stout/gzip.hpp>

using std::string;


#ifdef HAVE_LIBZ
TEST(GzipTest, CompressDecompressString)
{
  // Test bad compression levels, outside of [-1, Z_BEST_COMPRESSION].
  ASSERT_ERROR(gzip::compress("", -2));
  ASSERT_ERROR(gzip::compress("", Z_BEST_COMPRESSION + 1));

  string s =
    "Lorem ipsum dolor sit amet, consectetur adipisicing elit, sed do "
    "eiusmod tempor incididunt ut labore et dolore magna aliqua. Ut enim ad "
    "minim veniam, quis nostrud exercitation ullamco laboris nisi ut "
    "aliquip ex ea commodo consequat. Duis aute irure dolor in reprehenderit "
    "in voluptate velit esse cillum dolore eu fugiat nulla pariatur. "
    "Excepteur sint occaecat cupidatat non proident, sunt in culpa qui "
    "officia deserunt mollit anim id est laborum.";

  Try<string> compressed = gzip::compress(s);
  ASSERT_SOME(compressed);
  Try<string> decompressed = gzip::decompress(compressed.get());
  ASSERT_SOME(decompressed);
  ASSERT_EQ(s, decompressed.get());

  // Test with a 1MB random string!
  s = "";
  while (s.length() < (1024 * 1024)) {
    s.append(1, ' ' + (rand() % ('~' - ' ')));
  }
  compressed = gzip::compress(s);
  ASSERT_SOME(compressed);
  decompressed = gzip::decompress(compressed.get());
  ASSERT_SOME(decompressed);
  ASSERT_EQ(s, decompressed.get());

  s = "";
  compressed = gzip::compress(s);
  ASSERT_SOME(compressed);
  decompressed = gzip::decompress(compressed.get());
  ASSERT_SOME(decompressed);
  ASSERT_EQ(s, decompressed.get());
}


TEST(GzipTest, Decompressor)
{
  string s =
    "Lorem ipsum dolor sit amet, consectetur adipisicing elit, sed do "
    "eiusmod tempor incididunt ut labore et dolore magna aliqua. Ut enim ad "
    "minim veniam, quis nostrud exercitation ullamco laboris nisi ut "
    "aliquip ex ea commodo consequat. Duis aute irure dolor in reprehenderit "
    "in voluptate velit esse cillum dolore eu fugiat nulla pariatur. "
    "Excepteur sint occaecat cupidatat non proident, sunt in culpa qui "
    "officia deserunt mollit anim id est laborum.";

  Try<string> compressed = gzip::compress(s);
  ASSERT_SOME(compressed);

  gzip::Decompressor decompressor;

  // Decompress 1 byte at a time.
  string decompressed;
  size_t i = 0;

  while (i < compressed->size()) {
    size_t chunkSize = 1;
    string chunk = compressed->substr(i, chunkSize);

    Try<string> decompressedChunk = decompressor.decompress(chunk);
    ASSERT_SOME(decompressedChunk);
    decompressed += decompressedChunk.get();

    i += chunkSize;
  }

  EXPECT_TRUE(decompressor.finished());

  ASSERT_EQ(s, decompressed);
}
#endif // HAVE_LIBZ
