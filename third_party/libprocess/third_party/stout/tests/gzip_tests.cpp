#include <gtest/gtest.h>

#include <gmock/gmock.h>

#include <string>

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
#endif // HAVE_LIBZ
