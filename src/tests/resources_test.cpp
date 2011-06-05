#include <sstream>
#include <string>

#include <gtest/gtest.h>

#include <master/master.hpp>

using namespace mesos;
using namespace mesos::internal;
using namespace mesos::internal::master;

using std::ostringstream;


TEST(ResourcesTest, InitializedWithZero)
{
  Resources r;
  EXPECT_EQ(0, r.cpus());
  EXPECT_EQ(0, r.mem());
}


TEST(ResourcesTest, Addition)
{
  Resources r1;
  r1.set_cpus(1);
  r1.set_mem(5);
  Resources r2;
  r2.set_cpus(2);
  r2.set_mem(10);
  Resources sum = r1 + r2;
  EXPECT_EQ(3, sum.cpus());
  EXPECT_EQ(15, sum.mem());
  Resources r;
  r += r1;
  EXPECT_EQ(1, r.cpus());
  EXPECT_EQ(5, r.mem());
  r += r2;
  EXPECT_EQ(3, r.cpus());
  EXPECT_EQ(15, r.mem());
}


TEST(ResourcesTest, Subtraction)
{
  Resources r1;
  r1.set_cpus(1);
  r1.set_mem(5);
  Resources r2;
  r2.set_cpus(2);
  r2.set_mem(10);
  Resources dif = r1 - r2;
  EXPECT_EQ(-1, dif.cpus());
  EXPECT_EQ(-5, dif.mem());
  Resources r;
  r -= r1;
  EXPECT_EQ(-1, r.cpus());
  EXPECT_EQ(-5, r.mem());
  r -= r2;
  EXPECT_EQ(-3, r.cpus());
  EXPECT_EQ(-15, r.mem());
}


TEST(ResourcesTest, PrettyPrinting)
{
  Resources r;
  r.set_cpus(3);
  r.set_mem(1001001001);
  ostringstream oss;
  oss << r;
  EXPECT_EQ("<3 CPUs, 1001001001 MEM>", oss.str());
}
