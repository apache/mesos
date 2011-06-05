#include <sstream>
#include <string>

#include <gtest/gtest.h>

#include <master/master.hpp>

using namespace mesos;
using namespace mesos::internal;
using namespace mesos::internal::master;

using std::ostringstream;
using std::string;


TEST(ResourcesTest, Parsing)
{
  Resource cpus = Resources::parse("cpus", "45.55");
  ASSERT_EQ(Resource::SCALAR, cpus.type());
  EXPECT_EQ(45.55, cpus.scalar().value());

  Resource ports = Resources::parse("ports", "[10000-20000, 30000-50000]");
  ASSERT_EQ(Resource::RANGES, ports.type());
  EXPECT_EQ(2, ports.ranges().range_size());

  Resource disks = Resources::parse("disks", "{sda1}");
  ASSERT_EQ(Resource::SET, disks.type());
  ASSERT_EQ(1, disks.set().item_size());
  EXPECT_EQ("sda1", disks.set().item(0));

  Resources r1 = Resources::parse("cpus:45.55;"
                                  "ports:[10000-20000, 30000-50000];"
                                  "disks:{sda1}");

  Resources r2;
  r2 += cpus;
  r2 += ports;
  r2 += disks;

  EXPECT_EQ(r1, r2);
}


TEST(ResourcesTest, Printing)
{
  Resources r = Resources::parse("cpus:45.55;"
                                 "ports:[10000-20000, 30000-50000];"
                                 "disks:{sda1}");

  string output = "cpus=45.55; ports=[10000-20000, 30000-50000]; disks={sda1}";

  ostringstream oss;
  oss << r;

  // TODO(benh): This test is a bit strict because it implies the
  // ordering of things (e.g., the ordering of resources and the
  // ordering of ranges). We should really just be checking for the
  // existance of certain substrings in the output.

  EXPECT_EQ(output, oss.str());
}


TEST(ResourcesTest, InitializedIsEmpty)
{
  Resources r;
  EXPECT_EQ(0, r.size());
}


TEST(ResourcesTest, BadResourcesNotAllocatable)
{
  Resource cpus;
  cpus.set_type(Resource::SCALAR);
  cpus.mutable_scalar()->set_value(1);
  Resources r;
  r += cpus;
  EXPECT_EQ(0, r.allocatable().size());
  cpus.set_name("cpus");
  cpus.mutable_scalar()->set_value(0);
  r += cpus;
  EXPECT_EQ(0, r.allocatable().size());
}


TEST(ResourcesTest, ScalarEquals)
{
  Resource cpus = Resources::parse("cpus", "3");
  Resource mem =  Resources::parse("mem", "3072");

  Resources r1;
  r1 += cpus;
  r1 += mem;

  Resources r2;
  r2 += cpus;
  r2 += mem;

  EXPECT_EQ(2, r1.size());
  EXPECT_EQ(2, r2.size());
  EXPECT_EQ(r1, r2);
}


TEST(ResourcesTest, ScalarSubset)
{
  Resource cpus1 = Resources::parse("cpus", "1");
  Resource mem1 =  Resources::parse("mem", "3072");

  Resource cpus2 = Resources::parse("cpus", "1");
  Resource mem2 =  Resources::parse("mem", "4096");

  Resources r1;
  r1 += cpus1;
  r1 += mem1;

  Resources r2;
  r2 += cpus2;
  r2 += mem2;

  EXPECT_TRUE(r1 <= r2);
  EXPECT_FALSE(r2 <= r1);
}


TEST(ResourcesTest, ScalarAddition)
{
  Resource cpus1 = Resources::parse("cpus", "1");
  Resource mem1 = Resources::parse("mem", "5");

  Resource cpus2 = Resources::parse("cpus", "2");
  Resource mem2 = Resources::parse("mem", "10");

  Resources r1;
  r1 += cpus1;
  r1 += mem1;

  Resources r2;
  r2 += cpus2;
  r2 += mem2;

  Resources sum = r1 + r2;
  EXPECT_EQ(2, sum.size());
  EXPECT_EQ(3, sum.getScalar("cpus", Resource::Scalar()).value());
  EXPECT_EQ(15, sum.getScalar("mem", Resource::Scalar()).value());

  Resources r = r1;
  r += r2;
  EXPECT_EQ(2, r.size());
  EXPECT_EQ(3, r.getScalar("cpus", Resource::Scalar()).value());
  EXPECT_EQ(15, r.getScalar("mem", Resource::Scalar()).value());
}


TEST(ResourcesTest, ScalarSubtraction)
{
  Resource cpus1 = Resources::parse("cpus", "50");
  Resource mem1 = Resources::parse("mem", "4096");

  Resource cpus2 = Resources::parse("cpus", "0.5");
  Resource mem2 = Resources::parse("mem", "1024");

  Resources r1;
  r1 += cpus1;
  r1 += mem1;

  Resources r2;
  r2 += cpus2;
  r2 += mem2;

  Resources diff = r1 - r2;
  EXPECT_EQ(2, diff.size());
  EXPECT_EQ(49.5, diff.getScalar("cpus", Resource::Scalar()).value());
  EXPECT_EQ(3072, diff.getScalar("mem", Resource::Scalar()).value());

  Resources r = r1;
  r -= r2;
  EXPECT_EQ(49.5, diff.getScalar("cpus", Resource::Scalar()).value());
  EXPECT_EQ(3072, diff.getScalar("mem", Resource::Scalar()).value());

  r = r1;
  r -= r1;
  EXPECT_EQ(0, r.allocatable().size());
}


TEST(ResourcesTest, RangesEquals)
{
  Resource ports = Resources::parse("ports", "[20000-40000]");

  Resources r1;
  r1 += ports;

  Resources r2;
  r2 += ports;

  EXPECT_EQ(r1, r2);
}


TEST(ResourcesTest, RangesSubset)
{
  Resource ports1 = Resources::parse("ports", "[20000-40000]");
  Resource ports2 = Resources::parse("ports", "[20000-40000, 50000-60000]");

  Resources r1;
  r1 += ports1;

  Resources r2;
  r2 += ports2;

  EXPECT_EQ(1, r1.size());
  EXPECT_EQ(1, r2.size());
  EXPECT_TRUE(r1 <= r2);
  EXPECT_FALSE(r2 <= r1);
}


TEST(ResourcesTest, RangesAddition)
{
  Resource ports1 = Resources::parse("ports", "[20000-40000, 21000-38000]");
  Resource ports2 = Resources::parse("ports", "[30000-50000, 10000-20000]");

  Resources r;
  r += ports1;
  r += ports2;

  EXPECT_EQ(1, r.size());

  const Resource::Ranges& ranges = r.getRanges("ports", Resource::Ranges());

  EXPECT_EQ(1, ranges.range_size());
  EXPECT_EQ(10000, ranges.range(0).begin());
  EXPECT_EQ(50000, ranges.range(0).end());
}


TEST(ResourcesTest, RangesSubtraction)
{
  Resource ports1 = Resources::parse("ports", "[20000-40000]");
  Resource ports2 = Resources::parse("ports", "[10000-20000, 30000-50000]");

  Resources r;
  r += ports1;
  r -= ports2;

  EXPECT_EQ(1, r.size());

  const Resource::Ranges& ranges = r.getRanges("ports", Resource::Ranges());

  EXPECT_EQ(1, ranges.range_size());
  EXPECT_EQ(20001, ranges.range(0).begin());
  EXPECT_EQ(29999, ranges.range(0).end());
}


TEST(ResourcesTest, SetEquals)
{
  Resource disks = Resources::parse("disks", "{sda1}");

  Resources r1;
  r1 += disks;

  Resources r2;
  r2 += disks;

  EXPECT_EQ(r1, r2);
}


TEST(ResourcesTest, SetSubset)
{
  Resource disks1 = Resources::parse("disks", "{sda1,sda2}");
  Resource disks2 = Resources::parse("disks", "{sda1,sda2,sda3,sda4}");

  Resources r1;
  r1 += disks1;

  Resources r2;
  r2 += disks2;

  EXPECT_EQ(1, r1.size());
  EXPECT_EQ(1, r2.size());
  EXPECT_TRUE(r1 <= r2);
  EXPECT_FALSE(r2 <= r1);
}


TEST(ResourcesTest, SetAddition)
{
  Resource disks1 = Resources::parse("disks", "{sda1,sda2,sda3}");
  Resource disks2 = Resources::parse("disks", "{sda1,sda2,sda3,sda4}");

  Resources r;
  r += disks1;
  r += disks2;

  EXPECT_EQ(1, r.size());

  const Resource::Set& set = r.getSet("disks", Resource::Set());

  EXPECT_EQ(4, set.item_size());
}


TEST(ResourcesTest, SetSubtraction)
{
  Resource disks1 = Resources::parse("disks", "{sda1,sda2,sda3,sda4}");
  Resource disks2 = Resources::parse("disks", "{sda2,sda3,sda4}");

  Resources r;
  r += disks1;
  r -= disks2;

  EXPECT_EQ(1, r.size());

  const Resource::Set& set = r.getSet("disks", Resource::Set());

  EXPECT_EQ(1, set.item_size());
  EXPECT_EQ("sda1", set.item(0));
}
