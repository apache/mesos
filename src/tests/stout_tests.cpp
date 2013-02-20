#include <gmock/gmock.h>

#include <cstdlib> // For rand.
#include <map>
#include <string>
#include <vector>

#include <stout/cache.hpp>
#include <stout/duration.hpp>
#include <stout/error.hpp>
#include <stout/fatal.hpp>
#include <stout/foreach.hpp>
#include <stout/format.hpp>
#include <stout/gzip.hpp>
#include <stout/hashmap.hpp>
#include <stout/hashset.hpp>
#include <stout/json.hpp>
#include <stout/lambda.hpp>
#include <stout/multihashmap.hpp>
#include <stout/none.hpp>
#include <stout/net.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/protobuf.hpp>
#include <stout/result.hpp>
#include <stout/stringify.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>
#include <stout/uuid.hpp>

#include "tests/utils.hpp"

#include "tests/utils.hpp"

using namespace std;


Error error1()
{
  return Error("Failed to ...");
}


Try<string> error2()
{
  return Error("Failed to ...");
}


Try<string> error3(const Try<string>& t)
{
  return t;
}


Result<string> error4()
{
  return Error("Failed to ...");
}


Result<string> error5(const Result<string>& r)
{
  return r;
}


TEST(Stout, Error)
{
  Try<string> t = error1();
  EXPECT_TRUE(t.isError());
  t = error2();
  EXPECT_TRUE(t.isError());
  t = error3(error1());
  EXPECT_TRUE(t.isError());

  Result<string> r = error1();
  EXPECT_TRUE(r.isError());
  r = error4();
  EXPECT_TRUE(r.isError());
  r = error5(error1());
  EXPECT_TRUE(r.isError());
}


None none1()
{
  return None();
}


Option<string> none2()
{
  return None();
}


Option<string> none3(const Option<string>& o)
{
  return o;
}


Result<string> none4()
{
  return None();
}


Result<string> none5(const Result<string>& r)
{
  return r;
}


TEST(Stout, None)
{
  Option<string> o = none1();
  EXPECT_TRUE(o.isNone());
  o = none2();
  EXPECT_TRUE(o.isNone());
  o = none3(none1());
  EXPECT_TRUE(o.isNone());

  Result<string> r = none1();
  EXPECT_TRUE(r.isNone());
  r = none4();
  EXPECT_TRUE(r.isNone());
  r = none5(none1());
  EXPECT_TRUE(r.isNone());
}


TEST(Stout, Duration)
{
  Try<Duration> _3hrs = Duration::parse("3hrs");
  ASSERT_SOME(_3hrs);

  EXPECT_EQ(Hours(3.0), _3hrs.get());
  EXPECT_EQ(Minutes(180.0), _3hrs.get());
  EXPECT_EQ(Seconds(10800.0), _3hrs.get());
  EXPECT_EQ(Milliseconds(10800000.0), _3hrs.get());

  EXPECT_EQ(Milliseconds(1000.0), Seconds(1.0));

  EXPECT_GT(Weeks(1.0), Days(6.0));

  EXPECT_LT(Hours(23.0), Days(1.0));

  EXPECT_LE(Hours(24.0), Days(1.0));
  EXPECT_GE(Hours(24.0), Days(1.0));

  EXPECT_NE(Minutes(59.0), Hours(1.0));

  Try<Duration> _3_5hrs = Duration::parse("3.5hrs");
  ASSERT_SOME(_3_5hrs);

  EXPECT_EQ(Hours(3.5), _3_5hrs.get());
}


TEST(StoutStringsTest, Format)
{
  Try<std::string> result = strings::format("%s %s", "hello", "world");
  ASSERT_SOME(result);
  EXPECT_EQ("hello world", result.get());

  result = strings::format("hello %d", 42);
  ASSERT_SOME(result);
  EXPECT_EQ("hello 42", result.get());

  result = strings::format("hello %s", "fourty-two");
  ASSERT_SOME(result);
  EXPECT_EQ("hello fourty-two", result.get());

  string hello = "hello";

  result = strings::format("%s %s", hello, "fourty-two");
  ASSERT_SOME(result);
  EXPECT_EQ("hello fourty-two", result.get());
}


TEST(StoutStringsTest, Remove)
{
  EXPECT_EQ("heo word", strings::remove("hello world", "l"));
  EXPECT_EQ("hel world", strings::remove("hello world", "lo"));
  EXPECT_EQ("home/", strings::remove("/home/", "/", strings::PREFIX));
  EXPECT_EQ("/home", strings::remove("/home/", "/", strings::SUFFIX));
}


TEST(StoutStringsTest, Trim)
{
  EXPECT_EQ("", strings::trim("", " "));
  EXPECT_EQ("", strings::trim("    ", " "));
  EXPECT_EQ("hello world", strings::trim("hello world", " "));
  EXPECT_EQ("hello world", strings::trim("  hello world", " "));
  EXPECT_EQ("hello world", strings::trim("hello world  ", " "));
  EXPECT_EQ("hello world", strings::trim("  hello world  ", " "));
  EXPECT_EQ("hello world", strings::trim(" \t hello world\t  ", " \t"));
  EXPECT_EQ("hello world", strings::trim(" \t hello world\t \n\r "));
}


TEST(StoutStringsTest, Tokenize)
{
  vector<string> tokens = strings::tokenize("hello world,  what's up?", " ");
  ASSERT_EQ(4u, tokens.size());
  EXPECT_EQ("hello",  tokens[0]);
  EXPECT_EQ("world,", tokens[1]);
  EXPECT_EQ("what's", tokens[2]);
  EXPECT_EQ("up?",    tokens[3]);
}


TEST(StoutStringsTest, TokenizeStringWithDelimsAtStart)
{
  vector<string> tokens = strings::tokenize("  hello world,  what's up?", " ");
  ASSERT_EQ(4u, tokens.size());
  EXPECT_EQ("hello",  tokens[0]);
  EXPECT_EQ("world,", tokens[1]);
  EXPECT_EQ("what's", tokens[2]);
  EXPECT_EQ("up?",    tokens[3]);
}


TEST(StoutStringsTest, TokenizeStringWithDelimsAtEnd)
{
  vector<string> tokens = strings::tokenize("hello world,  what's up?  ", " ");
  ASSERT_EQ(4u, tokens.size());
  EXPECT_EQ("hello",  tokens[0]);
  EXPECT_EQ("world,", tokens[1]);
  EXPECT_EQ("what's", tokens[2]);
  EXPECT_EQ("up?",    tokens[3]);
}


TEST(StoutStringsTest, TokenizeStringWithDelimsAtStartAndEnd)
{
  vector<string> tokens = strings::tokenize("  hello world,  what's up?  ", " ");
  ASSERT_EQ(4u, tokens.size());
  EXPECT_EQ("hello",  tokens[0]);
  EXPECT_EQ("world,", tokens[1]);
  EXPECT_EQ("what's", tokens[2]);
  EXPECT_EQ("up?",    tokens[3]);
}


TEST(StoutStringsTest, TokenizeWithMultipleDelims)
{
  vector<string> tokens = strings::tokenize("hello\tworld,  \twhat's up?",
                                            " \t");
  ASSERT_EQ(4u, tokens.size());
  EXPECT_EQ("hello",  tokens[0]);
  EXPECT_EQ("world,", tokens[1]);
  EXPECT_EQ("what's", tokens[2]);
  EXPECT_EQ("up?",    tokens[3]);
}


TEST(StoutStringsTest, TokenizeEmptyString)
{
  vector<string> tokens = strings::tokenize("", " ");
  ASSERT_EQ(0u, tokens.size());
}


TEST(StoutStringsTest, TokenizeDelimOnlyString)
{
  vector<string> tokens = strings::tokenize("   ", " ");
  ASSERT_EQ(0u, tokens.size());
}


TEST(StoutStringsTest, SplitEmptyString)
{
  vector<string> tokens = strings::split("", ",");
  ASSERT_EQ(1u, tokens.size());
  EXPECT_EQ("", tokens[0]);
}


TEST(StoutStringsTest, SplitDelimOnlyString)
{
  vector<string> tokens = strings::split(",,,", ",");
  ASSERT_EQ(4u, tokens.size());
  EXPECT_EQ("", tokens[0]);
  EXPECT_EQ("", tokens[1]);
  EXPECT_EQ("", tokens[2]);
  EXPECT_EQ("", tokens[3]);
}


TEST(StoutStringsTest, Split)
{
  vector<string> tokens = strings::split("foo,bar,,baz", ",");
  ASSERT_EQ(4u, tokens.size());
  EXPECT_EQ("foo", tokens[0]);
  EXPECT_EQ("bar", tokens[1]);
  EXPECT_EQ("",    tokens[2]);
  EXPECT_EQ("baz", tokens[3]);
}


TEST(StoutStringsTest, SplitStringWithDelimsAtStart)
{
  vector<string> tokens = strings::split(",,foo,bar,,baz", ",");
  ASSERT_EQ(6u, tokens.size());
  EXPECT_EQ("",    tokens[0]);
  EXPECT_EQ("",    tokens[1]);
  EXPECT_EQ("foo", tokens[2]);
  EXPECT_EQ("bar", tokens[3]);
  EXPECT_EQ("",    tokens[4]);
  EXPECT_EQ("baz", tokens[5]);
}


TEST(StoutStringsTest, SplitStringWithDelimsAtEnd)
{
  vector<string> tokens = strings::split("foo,bar,,baz,,", ",");
  ASSERT_EQ(6u, tokens.size());
  EXPECT_EQ("foo", tokens[0]);
  EXPECT_EQ("bar", tokens[1]);
  EXPECT_EQ("",    tokens[2]);
  EXPECT_EQ("baz", tokens[3]);
  EXPECT_EQ("",    tokens[4]);
  EXPECT_EQ("",    tokens[5]);
}


TEST(StoutStringsTest, SplitStringWithDelimsAtStartAndEnd)
{
  vector<string> tokens = strings::split(",,foo,bar,,", ",");
  ASSERT_EQ(6u, tokens.size());
  EXPECT_EQ("",    tokens[0]);
  EXPECT_EQ("",    tokens[1]);
  EXPECT_EQ("foo", tokens[2]);
  EXPECT_EQ("bar", tokens[3]);
  EXPECT_EQ("",    tokens[4]);
  EXPECT_EQ("",    tokens[5]);
}


TEST(StoutStringsTest, SplitWithMultipleDelims)
{
  vector<string> tokens = strings::split("foo.bar,.,.baz.", ",.");
  ASSERT_EQ(7u, tokens.size());
  EXPECT_EQ("foo", tokens[0]);
  EXPECT_EQ("bar", tokens[1]);
  EXPECT_EQ("",    tokens[2]);
  EXPECT_EQ("",    tokens[3]);
  EXPECT_EQ("",    tokens[4]);
  EXPECT_EQ("baz", tokens[5]);
  EXPECT_EQ("",    tokens[6]);
}


TEST(StoutStringsTest, Pairs)
{
  map<string, vector<string> > pairs = strings::pairs("one=1,two=2", ",", "=");
  ASSERT_EQ(2u, pairs.size());
  ASSERT_EQ(1u, pairs.count("one"));
  ASSERT_EQ(1u, pairs["one"].size());
  EXPECT_EQ("1", pairs["one"].front());
  ASSERT_EQ(1u, pairs.count("two"));
  ASSERT_EQ(1u, pairs["two"].size());
  EXPECT_EQ("2", pairs["two"].front());
}


TEST(StoutStringsTest, StartsWith)
{
  EXPECT_TRUE(strings::startsWith("hello world", "hello"));
  EXPECT_FALSE(strings::startsWith("hello world", "no"));
  EXPECT_FALSE(strings::startsWith("hello world", "ello"));
}


TEST(StoutStringsTest, Contains)
{
  EXPECT_TRUE(strings::contains("hello world", "world"));
  EXPECT_FALSE(strings::contains("hello world", "no"));
}


static hashset<std::string> listfiles(const std::string& dir)
{
  hashset<std::string> fileset;
  foreach (const std::string& file, os::ls(dir)) {
    fileset.insert(file);
  }
  return fileset;
}


class StoutUtilsTest : public ::testing::Test
{
protected:
  virtual void SetUp()
  {
    tmpdir = "/tmp/" + UUID::random().toString();
    os::mkdir(tmpdir);
  }

  virtual void TearDown()
  {
    os::rmdir(tmpdir);
  }

  std::string tmpdir;
};


TEST_F(StoutUtilsTest, rmdir)
{
  // TODO(John Sirois): It would be good to use something like mkdtemp, but
  // abstract away a proper platform independent /tmp dir.

  const hashset<std::string> EMPTY;

  hashset<std::string> expectedListing = EMPTY;
  EXPECT_EQ(expectedListing, listfiles(tmpdir));

  os::mkdir(tmpdir + "/a/b/c");
  os::mkdir(tmpdir + "/a/b/d");
  os::mkdir(tmpdir + "/e/f");

  expectedListing = EMPTY;
  expectedListing.insert("a");
  expectedListing.insert("e");
  EXPECT_EQ(expectedListing, listfiles(tmpdir));

  expectedListing = EMPTY;
  expectedListing.insert("b");
  EXPECT_EQ(expectedListing, listfiles(tmpdir + "/a"));

  expectedListing = EMPTY;
  expectedListing.insert("c");
  expectedListing.insert("d");
  EXPECT_EQ(expectedListing, listfiles(tmpdir + "/a/b"));

  expectedListing = EMPTY;
  EXPECT_EQ(expectedListing, listfiles(tmpdir + "/a/b/c"));
  EXPECT_EQ(expectedListing, listfiles(tmpdir + "/a/b/d"));

  expectedListing.insert("f");
  EXPECT_EQ(expectedListing, listfiles(tmpdir + "/e"));

  expectedListing = EMPTY;
  EXPECT_EQ(expectedListing, listfiles(tmpdir + "/e/f"));
}


TEST_F(StoutUtilsTest, nonblock)
{
  int pipes[2];
  ASSERT_NE(-1, pipe(pipes));

  Try<bool> isNonBlock = false;

  isNonBlock = os::isNonblock(pipes[0]);
  ASSERT_SOME(isNonBlock);
  EXPECT_FALSE(isNonBlock.get());

  ASSERT_SOME(os::nonblock(pipes[0]));

  isNonBlock = os::isNonblock(pipes[0]);
  ASSERT_SOME(isNonBlock);
  EXPECT_TRUE(isNonBlock.get());

  close(pipes[0]);
  close(pipes[1]);

  EXPECT_ERROR(os::nonblock(pipes[0]));
  EXPECT_ERROR(os::nonblock(pipes[0]));
}


TEST_F(StoutUtilsTest, touch)
{
  const std::string& testfile  = tmpdir + "/" + UUID::random().toString();

  ASSERT_SOME(os::touch(testfile));
  ASSERT_TRUE(os::exists(testfile));
}


TEST_F(StoutUtilsTest, readWriteString)
{
  const std::string& testfile  = tmpdir + "/" + UUID::random().toString();
  const std::string& teststr = "test";

  ASSERT_SOME(os::write(testfile, teststr));

  Try<std::string> readstr = os::read(testfile);

  ASSERT_SOME(readstr);
  EXPECT_EQ(teststr, readstr.get());
}


TEST_F(StoutUtilsTest, find)
{
  const std::string& testdir = tmpdir + "/" + UUID::random().toString();
  const std::string& subdir = testdir + "/test1";
  ASSERT_SOME(os::mkdir(subdir)); // Create the directories.

  // Now write some files.
  const std::string& file1 = testdir + "/file1.txt";
  const std::string& file2 = subdir + "/file2.txt";
  const std::string& file3 = subdir + "/file3.jpg";

  ASSERT_SOME(os::touch(file1));
  ASSERT_SOME(os::touch(file2));
  ASSERT_SOME(os::touch(file3));

  // Find "*.txt" files.
  Try<std::list<std::string> > result = os::find(testdir, ".txt");
  ASSERT_SOME(result);

  hashset<std::string> files;
  foreach (const std::string& file, result.get()) {
    files.insert(file);
  }

  ASSERT_EQ(2u, files.size());
  ASSERT_TRUE(files.contains(file1));
  ASSERT_TRUE(files.contains(file2));
}


TEST_F(StoutUtilsTest, uname)
{
  Try<os::UTSInfo> info = os::uname();

  ASSERT_SOME(info);
#ifdef __linux__
  EXPECT_EQ(info.get().sysname, "Linux");
#endif
#ifdef __APPLE__
  EXPECT_EQ(info.get().sysname, "Darwin");
#endif
}


TEST_F(StoutUtilsTest, sysname)
{
  Try<std::string> name = os::sysname();

  ASSERT_SOME(name);
#ifdef __linux__
  EXPECT_EQ(name.get(), "Linux");
#endif
#ifdef __APPLE__
  EXPECT_EQ(name.get(), "Darwin");
#endif
}


TEST_F(StoutUtilsTest, release)
{
  Try<os::Release> info = os::release();

  ASSERT_SOME(info);
}


TEST_F(StoutUtilsTest, hashset)
{
  hashset<std::string> hs1;
  hs1.insert(std::string("HS1"));
  hs1.insert(std::string("HS3"));

  hashset<std::string> hs2;
  hs2.insert(std::string("HS2"));

  hs1 = hs2;
  ASSERT_EQ(1u, hs1.size());
  ASSERT_TRUE(hs1.contains("HS2"));
  ASSERT_TRUE(hs1 == hs2);
}


TEST(StoutUUIDTest, test)
{
  UUID uuid1 = UUID::random();
  UUID uuid2 = UUID::fromBytes(uuid1.toBytes());
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
}


TEST(StoutMultihashmapTest, Put)
{
  multihashmap<string, uint16_t> map;

  map.put("foo", 1024);
  ASSERT_EQ(1u, map.get("foo").size());

  map.put("foo", 1025);
  ASSERT_EQ(2u, map.get("foo").size());

  ASSERT_EQ(2u, map.size());

  map.put("bar", 1024);
  ASSERT_EQ(1u, map.get("bar").size());

  map.put("bar", 1025);
  ASSERT_EQ(2u, map.get("bar").size());

  ASSERT_EQ(4u, map.size());
}


TEST(StoutMultihashmapTest, Remove)
{
  multihashmap<string, uint16_t> map;

  map.put("foo", 1024);
  map.remove("foo", 1024);
  ASSERT_EQ(0u, map.get("foo").size());

  ASSERT_EQ(0u, map.size());

  map.put("foo", 1024);
  map.put("foo", 1025);
  ASSERT_EQ(2u, map.get("foo").size());

  ASSERT_EQ(2u, map.size());

  map.remove("foo");
  ASSERT_EQ(0u, map.get("foo").size());
  ASSERT_EQ(0u, map.size());
}


TEST(StoutMultihashmapTest, Size)
{
  multihashmap<string, uint16_t> map;

  map.put("foo", 1024);
  map.put("foo", 1025);
  ASSERT_EQ(2u, map.get("foo").size());
  ASSERT_TRUE(map.contains("foo", 1024));
  ASSERT_TRUE(map.contains("foo", 1025));
  ASSERT_EQ(2u, map.size());

  map.put("bar", 1024);
  map.put("bar", 1025);
  ASSERT_EQ(2u, map.get("bar").size());
  ASSERT_TRUE(map.contains("bar", 1024));
  ASSERT_TRUE(map.contains("bar", 1025));
  ASSERT_EQ(4u, map.size());
}


TEST(StoutMultihashmapTest, Iterator)
{
  multihashmap<string, uint16_t> map;

  map.put("foo", 1024);
  map.put("foo", 1025);
  ASSERT_EQ(2u, map.get("foo").size());
  ASSERT_TRUE(map.contains("foo", 1024));
  ASSERT_TRUE(map.contains("foo", 1025));

  multihashmap<string, uint16_t>::iterator i = map.begin();

  ASSERT_TRUE(i != map.end());

  ASSERT_EQ("foo", i->first);
  ASSERT_EQ(1024, i->second);

  ++i;
  ASSERT_TRUE(i != map.end());

  ASSERT_EQ("foo", i->first);
  ASSERT_EQ(1025, i->second);

  ++i;
  ASSERT_TRUE(i == map.end());
}


TEST(StoutMultihashmapTest, Foreach)
{
  multihashmap<string, uint16_t> map;

  map.put("foo", 1024);
  map.put("bar", 1025);
  ASSERT_EQ(1u, map.get("foo").size());
  ASSERT_EQ(1u, map.get("bar").size());
  ASSERT_TRUE(map.contains("foo", 1024));
  ASSERT_TRUE(map.contains("bar", 1025));

  foreachpair (const string& key, uint16_t value, map) {
    if (key == "foo") {
      ASSERT_EQ(1024, value);
    } else if (key == "bar") {
      ASSERT_EQ(1025, value);
    } else {
      FAIL() << "Unexpected key/value in multihashmap";
    }
  }
}


TEST(StoutJsonTest, BinaryData)
{
  JSON::String s(string("\"\\/\b\f\n\r\t\x00\x19 !#[]\x7F\xFF", 17));

  EXPECT_EQ("\"\\\"\\\\\\/\\b\\f\\n\\r\\t\\u0000\\u0019 !#[]\\u007F\\u00FF\"",
            stringify(s));
}


#ifdef HAVE_LIBZ
TEST(StoutCompressionTest, Gzip)
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
