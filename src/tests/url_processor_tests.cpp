#include <algorithm>

#include <fstream>

#include <gtest/gtest.h>

#include <string>

#include "common/utils.hpp"
#include "common/uuid.hpp"
#include "detector/url_processor.hpp"
#include "tests/utils.hpp"

using namespace mesos::internal;
using namespace mesos::internal::test;

TEST(UrlProcessorTest, Zoo)
{
  std::pair<UrlProcessor::URLType, std::string> results =
      UrlProcessor::process("zoo://jake:1/foo");
  EXPECT_EQ(UrlProcessor::ZOO, results.first);
  EXPECT_EQ("jake:1/foo", results.second);

  results = UrlProcessor::process("zoo://jake:1,bob:2/foo");
  EXPECT_EQ(UrlProcessor::ZOO, results.first);
  EXPECT_EQ("jake:1,bob:2/foo", results.second);
}


TEST(UrlProcessorTest, ZooAuth)
{
  std::pair<UrlProcessor::URLType, std::string> results =
      UrlProcessor::process("zoo://fred:bob@jake:1/foo");
  EXPECT_EQ(UrlProcessor::ZOO, results.first);
  EXPECT_EQ("fred:bob@jake:1/foo", results.second);

  results = UrlProcessor::process("zoo://fred:bob@jake:1,bob:2/foo");
  EXPECT_EQ(UrlProcessor::ZOO, results.first);
  EXPECT_EQ("fred:bob@jake:1,bob:2/foo", results.second);
}


TEST_WITH_WORKDIR(UrlProcessorTest, ZooFile)
{
  std::string filename = UUID::random().toString();
  std::ofstream file(filename.c_str());
  file << "host1:8080" << std::endl;
  file << "host2:8081" << std::endl;
  file.close();

  std::pair<UrlProcessor::URLType, std::string> results =
      UrlProcessor::process("zoofile://" + filename);
  EXPECT_EQ(UrlProcessor::ZOO, results.first);
  EXPECT_EQ("host1:8080,host2:8081", results.second);
}


TEST_WITH_WORKDIR(UrlProcessorTest, ZooFileZnode)
{
  std::string filename = UUID::random().toString();
  std::ofstream file(filename.c_str());
  file << "host:8080" << std::endl;
  file << "[znode] /jake/bob" << std::endl;
  file.close();

  std::pair<UrlProcessor::URLType, std::string> results =
      UrlProcessor::process("zoofile://" + filename);
  EXPECT_EQ(UrlProcessor::ZOO, results.first);
  EXPECT_EQ("host:8080/jake/bob", results.second);
}


TEST_WITH_WORKDIR(UrlProcessorTest, ZooFileAuth)
{
  std::string filename = UUID::random().toString();
  std::ofstream file(filename.c_str());
  file << "[auth] jake:bob" << std::endl;
  file << "host:8080" << std::endl;
  file.close();

  std::pair<UrlProcessor::URLType, std::string> results =
      UrlProcessor::process("zoofile://" + filename);
  EXPECT_EQ(UrlProcessor::ZOO, results.first);
  EXPECT_EQ("jake:bob@host:8080", results.second);
}


TEST_WITH_WORKDIR(UrlProcessorTest, ZooFileAll)
{
  std::string filename = UUID::random().toString();
  std::ofstream file(filename.c_str());
  file << "[auth] jake:bob" << std::endl;
  file << "[znode] /jake/bob" << std::endl;
  file << "host1:8080" << std::endl;
  file << "host2:8081" << std::endl;
  file.close();

  std::pair<UrlProcessor::URLType, std::string> results =
      UrlProcessor::process("zoofile://" + filename);
  EXPECT_EQ(UrlProcessor::ZOO, results.first);
  EXPECT_EQ("jake:bob@host1:8080,host2:8081/jake/bob", results.second);
}


TEST(UrlProcessorTest, Mesos)
{
  std::pair<UrlProcessor::URLType, std::string> results =
      UrlProcessor::process("mesos://master@jake:1");
  EXPECT_EQ(UrlProcessor::MESOS, results.first);
  EXPECT_EQ("master@jake:1", results.second);
}

TEST(UrlProcessorTest, Unknown)
{
  std::pair<UrlProcessor::URLType, std::string> results =
      UrlProcessor::process("ftp://jake:1");
  EXPECT_EQ(UrlProcessor::UNKNOWN, results.first);
  EXPECT_EQ("ftp://jake:1", results.second);
}
