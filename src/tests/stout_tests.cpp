#include <gmock/gmock.h>

#include <map>
#include <string>
#include <vector>

#include <stout/cache.hpp>
#include <stout/fatal.hpp>
#include <stout/foreach.hpp>
#include <stout/format.hpp>
#include <stout/hashmap.hpp>
#include <stout/hashset.hpp>
#include <stout/json.hpp>
#include <stout/lambda.hpp>
#include <stout/multihashmap.hpp>
#include <stout/net.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/protobuf.hpp>
#include <stout/result.hpp>
#include <stout/stringify.hpp>
#include <stout/strings.hpp>
#include <stout/time.hpp>
#include <stout/timer.hpp>
#include <stout/try.hpp>
#include <stout/uuid.hpp>

using namespace std;


TEST(StoutStringsTest, Format)
{
  Try<std::string> result = strings::format("%s %s", "hello", "world");
  ASSERT_TRUE(result.isSome());
  EXPECT_EQ("hello world", result.get());

  result = strings::format("hello %d", 42);
  ASSERT_TRUE(result.isSome());
  EXPECT_EQ("hello 42", result.get());

  result = strings::format("hello %s", "fourty-two");
  ASSERT_TRUE(result.isSome());
  EXPECT_EQ("hello fourty-two", result.get());

  string hello = "hello";

  result = strings::format("%s %s", hello, "fourty-two");
  ASSERT_TRUE(result.isSome());
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


TEST(StoutStringsTest, SplitEmptyString)
{
  vector<string> tokens = strings::split("", " ");
  ASSERT_EQ(0, tokens.size());
}


TEST(StoutStringsTest, SplitDelimOnlyString)
{
  vector<string> tokens = strings::split("   ", " ");
  ASSERT_EQ(0, tokens.size());
}


TEST(StoutStringsTest, Split)
{
  vector<string> tokens = strings::split("hello world,  what's up?", " ");
  ASSERT_EQ(4, tokens.size());
  EXPECT_EQ("hello",  tokens[0]);
  EXPECT_EQ("world,", tokens[1]);
  EXPECT_EQ("what's", tokens[2]);
  EXPECT_EQ("up?",    tokens[3]);
}


TEST(StoutStringsTest, SplitStringWithDelimsAtStart)
{
  vector<string> tokens = strings::split("  hello world,  what's up?", " ");
  ASSERT_EQ(4, tokens.size());
  EXPECT_EQ("hello",  tokens[0]);
  EXPECT_EQ("world,", tokens[1]);
  EXPECT_EQ("what's", tokens[2]);
  EXPECT_EQ("up?",    tokens[3]);
}


TEST(StoutStringsTest, SplitStringWithDelimsAtEnd)
{
  vector<string> tokens = strings::split("hello world,  what's up?  ", " ");
  ASSERT_EQ(4, tokens.size());
  EXPECT_EQ("hello",  tokens[0]);
  EXPECT_EQ("world,", tokens[1]);
  EXPECT_EQ("what's", tokens[2]);
  EXPECT_EQ("up?",    tokens[3]);
}


TEST(StoutStringsTest, SplitStringWithDelimsAtStartAndEnd)
{
  vector<string> tokens = strings::split("  hello world,  what's up?  ", " ");
  ASSERT_EQ(4, tokens.size());
  EXPECT_EQ("hello",  tokens[0]);
  EXPECT_EQ("world,", tokens[1]);
  EXPECT_EQ("what's", tokens[2]);
  EXPECT_EQ("up?",    tokens[3]);
}


TEST(StoutStringsTest, SplitWithMultipleDelims)
{
  vector<string> tokens = strings::split("hello\tworld,  \twhat's up?", " \t");
  ASSERT_EQ(4, tokens.size());
  EXPECT_EQ("hello",  tokens[0]);
  EXPECT_EQ("world,", tokens[1]);
  EXPECT_EQ("what's", tokens[2]);
  EXPECT_EQ("up?",    tokens[3]);
}


TEST(StoutStringsTest, Pairs)
{
  map<string, vector<string> > pairs = strings::pairs("one=1,two=2", ",", "=");
  ASSERT_EQ(2, pairs.size());
  ASSERT_EQ(1, pairs.count("one"));
  ASSERT_EQ(1, pairs["one"].size());
  EXPECT_EQ("1", pairs["one"].front());
  ASSERT_EQ(1, pairs.count("two"));
  ASSERT_EQ(1, pairs["two"].size());
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
  foreach (const std::string& file, os::listdir(dir)) {
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
  //abstract away a proper platform independent /tmp dir.

  hashset<std::string> emptyListing;
  emptyListing.insert(".");
  emptyListing.insert("..");

  hashset<std::string> expectedListing = emptyListing;
  EXPECT_EQ(expectedListing, listfiles(tmpdir));

  os::mkdir(tmpdir + "/a/b/c");
  os::mkdir(tmpdir + "/a/b/d");
  os::mkdir(tmpdir + "/e/f");

  expectedListing = emptyListing;
  expectedListing.insert("a");
  expectedListing.insert("e");
  EXPECT_EQ(expectedListing, listfiles(tmpdir));

  expectedListing = emptyListing;
  expectedListing.insert("b");
  EXPECT_EQ(expectedListing, listfiles(tmpdir + "/a"));

  expectedListing = emptyListing;
  expectedListing.insert("c");
  expectedListing.insert("d");
  EXPECT_EQ(expectedListing, listfiles(tmpdir + "/a/b"));

  expectedListing = emptyListing;
  EXPECT_EQ(expectedListing, listfiles(tmpdir + "/a/b/c"));
  EXPECT_EQ(expectedListing, listfiles(tmpdir + "/a/b/d"));

  expectedListing.insert("f");
  EXPECT_EQ(expectedListing, listfiles(tmpdir + "/e"));

  expectedListing = emptyListing;
  EXPECT_EQ(expectedListing, listfiles(tmpdir + "/e/f"));
}


TEST_F(StoutUtilsTest, touch)
{
  const std::string& testfile  = tmpdir + "/" + UUID::random().toString();
  Try<bool> result = os::touch(testfile);

  ASSERT_TRUE(result.get());
  ASSERT_TRUE(os::exists(testfile));
}


TEST_F(StoutUtilsTest, readWriteString)
{
  const std::string& testfile  = tmpdir + "/" + UUID::random().toString();
  const std::string& teststr = "test";

  Try<bool> result = os::write(testfile, teststr);
  ASSERT_TRUE(result.get());

  Result<std::string> readstr = os::read(testfile);

  ASSERT_TRUE(readstr.isSome());
  EXPECT_EQ(teststr, readstr.get());
}


TEST_F(StoutUtilsTest, find)
{
  const std::string& testdir  = tmpdir + "/" + UUID::random().toString();
  const std::string& subdir = testdir + "/test1";
  ASSERT_TRUE(os::mkdir(subdir)); // Create the directories.

  // Now write some files.
  const std::string& file1 = testdir + "/file1.txt";
  const std::string& file2 = subdir + "/file2.txt";
  const std::string& file3 = subdir + "/file3.jpg";

  ASSERT_TRUE(os::touch(file1).get());
  ASSERT_TRUE(os::touch(file2).get());
  ASSERT_TRUE(os::touch(file3).get());

  // Find "*.txt" files.
  Try<std::list<std::string> > result = os::find(testdir, ".txt");
  ASSERT_TRUE(result.isSome());

  hashset<std::string> files;
  foreach (const std::string& file, result.get()) {
    files.insert(file);
  }

  ASSERT_EQ(2, files.size());
  ASSERT_TRUE(files.contains(file1));
  ASSERT_TRUE(files.contains(file2));
}


TEST_F(StoutUtilsTest, uname)
{
  Try<os::UTSInfo> info = os::uname();

  ASSERT_TRUE(info.isSome());
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

  ASSERT_TRUE(name.isSome());
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

  ASSERT_TRUE(info.isSome());
}


TEST_F(StoutUtilsTest, hashset)
{
  hashset<std::string> hs1;
  hs1.insert(std::string("HS1"));
  hs1.insert(std::string("HS3"));

  hashset<std::string> hs2;
  hs2.insert(std::string("HS2"));

  hs1 = hs2;
  ASSERT_TRUE(hs1.size() == 1);
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
  ASSERT_EQ(1, map.get("foo").size());

  map.put("foo", 1025);
  ASSERT_EQ(2, map.get("foo").size());

  ASSERT_EQ(2, map.size());

  map.put("bar", 1024);
  ASSERT_EQ(1, map.get("bar").size());

  map.put("bar", 1025);
  ASSERT_EQ(2, map.get("bar").size());

  ASSERT_EQ(4, map.size());
}


TEST(StoutMultihashmapTest, Remove)
{
  multihashmap<string, uint16_t> map;

  map.put("foo", 1024);
  map.remove("foo", 1024);
  ASSERT_EQ(0, map.get("foo").size());

  ASSERT_EQ(0, map.size());

  map.put("foo", 1024);
  map.put("foo", 1025);
  ASSERT_EQ(2, map.get("foo").size());

  ASSERT_EQ(2, map.size());

  map.remove("foo");
  ASSERT_EQ(0, map.get("foo").size());
  ASSERT_EQ(0, map.size());
}


TEST(StoutMultihashmapTest, Size)
{
  multihashmap<string, uint16_t> map;

  map.put("foo", 1024);
  map.put("foo", 1025);
  ASSERT_EQ(2, map.get("foo").size());
  ASSERT_TRUE(map.contains("foo", 1024));
  ASSERT_TRUE(map.contains("foo", 1025));
  ASSERT_EQ(2, map.size());

  map.put("bar", 1024);
  map.put("bar", 1025);
  ASSERT_EQ(2, map.get("bar").size());
  ASSERT_TRUE(map.contains("bar", 1024));
  ASSERT_TRUE(map.contains("bar", 1025));
  ASSERT_EQ(4, map.size());
}


TEST(StoutMultihashmapTest, Iterator)
{
  multihashmap<string, uint16_t> map;

  map.put("foo", 1024);
  map.put("foo", 1025);
  ASSERT_EQ(2, map.get("foo").size());
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
  ASSERT_EQ(1, map.get("foo").size());
  ASSERT_EQ(1, map.get("bar").size());
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
