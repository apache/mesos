#include <sstream>
#include <string>

#include <gtest/gtest.h>

#include "master.hpp"

using namespace nexus;
using namespace nexus::internal;
using namespace nexus::internal::master;


TEST(ResourcesTest, InitializedWithZero)
{
  Resources r;
  EXPECT_EQ(0, r.cpus);
  EXPECT_EQ(0, r.mem);
}


TEST(ResourcesTest, Addition)
{
  Resources r1(1, 5);
  Resources r2(2, 10);
  Resources sum = r1 + r2;
  EXPECT_EQ(3, sum.cpus);
  EXPECT_EQ(15, sum.mem);
  Resources r;
  r += r1;
  EXPECT_EQ(1, r.cpus);
  EXPECT_EQ(5, r.mem);
  r += r2;
  EXPECT_EQ(3, r.cpus);
  EXPECT_EQ(15, r.mem);
}


TEST(ResourcesTest, Subtraction)
{
  Resources r1(1, 5);
  Resources r2(2, 10);
  Resources dif = r1 - r2;
  EXPECT_EQ(-1, dif.cpus);
  EXPECT_EQ(-5, dif.mem);
  Resources r;
  r -= r1;
  EXPECT_EQ(-1, r.cpus);
  EXPECT_EQ(-5, r.mem);
  r -= r2;
  EXPECT_EQ(-3, r.cpus);
  EXPECT_EQ(-15, r.mem);
}


TEST(ResourcesTest, PrettyPrinting)
{
  Resources r(3, 100100100100100LL);
  ostringstream oss;
  oss << r;
  EXPECT_EQ("<3 CPUs, 100100100100100 MEM>", oss.str());
}
