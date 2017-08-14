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

#include <gtest/gtest.h>

#include <gmock/gmock.h>

#include <stout/duration.hpp>
#include <stout/gtest.hpp>
#include <stout/stringify.hpp>
#include <stout/try.hpp>


TEST(DurationTest, Comparison)
{
  EXPECT_EQ(Duration::zero(), Seconds(0));
  EXPECT_EQ(Minutes(180), Hours(3));
  EXPECT_EQ(Seconds(10800), Hours(3));
  EXPECT_EQ(Milliseconds(10800000), Hours(3));

  EXPECT_EQ(Milliseconds(1), Microseconds(1000));
  EXPECT_EQ(Milliseconds(1000), Seconds(1));

  EXPECT_GT(Weeks(1), Days(6));

  EXPECT_LT(Hours(23), Days(1));

  EXPECT_LE(Hours(24), Days(1));
  EXPECT_GE(Hours(24), Days(1));

  EXPECT_NE(Minutes(59), Hours(1));

  // Maintains precision for a 100 year duration.
  EXPECT_GT(Weeks(5217) + Nanoseconds(1), Weeks(5217));
  EXPECT_LT(Weeks(5217) - Nanoseconds(1), Weeks(5217));
}


TEST(DurationTest, ParseAndTry)
{
  EXPECT_SOME_EQ(Hours(3), Duration::parse("3hrs"));
  EXPECT_SOME_EQ(Hours(3) + Minutes(30), Duration::parse("3.5hrs"));

  EXPECT_ERROR(
      Duration::parse(
          stringify(std::numeric_limits<int64_t>::max()) + "seconds"));

  EXPECT_SOME_EQ(Nanoseconds(3141592653), Duration::create(3.141592653));

  // Duration can hold only +/-9.22337e9 seconds.
  EXPECT_ERROR(Duration::create(10 * 1e9));
  EXPECT_ERROR(Duration::create(-10 * 1e9));
}


TEST(DurationTest, Arithmetic)
{
  Duration d = Seconds(11);
  d += Seconds(9);
  EXPECT_EQ(Seconds(20), d);

  d = Seconds(11);
  d -= Seconds(21);
  EXPECT_EQ(Seconds(-10), d);

  d = Seconds(10);
  d *= 2;
  EXPECT_EQ(Seconds(20), d);

  d = Seconds(10);
  d /= 2.5;
  EXPECT_EQ(Seconds(4), d);

  EXPECT_EQ(Seconds(20), Seconds(11) + Seconds(9));
  EXPECT_EQ(Seconds(-10), Seconds(11) - Seconds(21));
  EXPECT_EQ(Duration::create(2.5).get(), Seconds(10) * 0.25);
  EXPECT_EQ(Duration::create(1.25).get(), Seconds(10) / 8);

  EXPECT_EQ(Duration::create(Days(11).secs() + 9).get(), Days(11) + Seconds(9));
}


TEST(DurationTest, OutputFormat)
{
  EXPECT_EQ("1ns", stringify(Nanoseconds(1)));
  EXPECT_EQ("2ns", stringify(Nanoseconds(2)));

  // Truncated. Seconds in 15 digits of precision, max of double
  // type's precise digits.
  EXPECT_EQ("3.141592653secs",
            stringify(Duration::create(3.14159265358979).get()));
  EXPECT_EQ("3140ms", stringify(Duration::create(3.14).get()));
  EXPECT_EQ("10hrs", stringify(Hours(10)));
  EXPECT_EQ("-10hrs", stringify(Hours(-10)));

  // "10days" reads better than "1.42857142857143weeks" so it is
  // printed out in the lower unit.
  EXPECT_EQ("10days", stringify(Days(10)));
  // We go one-level down and it is still not a whole number so we
  // print it out using the higher unit.
  EXPECT_EQ("1.1875days", stringify(Days(1) + Hours(4) + Minutes(30)));
  // "2weeks" reads better than "14days" so we use the higher unit
  // here.
  EXPECT_EQ("2weeks", stringify(Days(14)));

  // Boundary cases.
  EXPECT_EQ("0ns", stringify(Duration::zero()));
  EXPECT_EQ("15250.2844524715weeks", stringify(Duration::max()));
  EXPECT_EQ("-15250.2844524715weeks", stringify(Duration::min()));
}


TEST(DurationTest, Timeval)
{
  EXPECT_EQ(Duration(timeval{10, 0}), Seconds(10));
  EXPECT_EQ(Duration(timeval{0, 7}), Microseconds(7));
  EXPECT_EQ(Duration(timeval{2, 123}), Seconds(2) + Microseconds(123));

  timeval t{2, 123};
  Duration d(t);
  EXPECT_EQ(d.timeval().tv_sec, t.tv_sec);
  EXPECT_EQ(d.timeval().tv_usec, t.tv_usec);

  t.tv_usec = 0;
  d = Duration(t);
  EXPECT_EQ(d.timeval().tv_sec, t.tv_sec);
  EXPECT_EQ(d.timeval().tv_usec, t.tv_usec);

  // Negative times.
  t.tv_sec = 0;
  t.tv_usec = -1;
  d = Duration(t);
  EXPECT_EQ(d.timeval().tv_sec, t.tv_sec);
  EXPECT_EQ(d.timeval().tv_usec, t.tv_usec);

  d = Microseconds(-1);
  EXPECT_EQ(d.timeval().tv_sec, t.tv_sec);
  EXPECT_EQ(d.timeval().tv_usec, t.tv_usec);

  t.tv_sec = -1;
  t.tv_usec = -30;
  d = Duration(t);
  EXPECT_EQ(d.timeval().tv_sec, t.tv_sec);
  EXPECT_EQ(d.timeval().tv_usec, t.tv_usec);
}
