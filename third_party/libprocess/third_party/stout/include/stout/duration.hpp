#ifndef __STOUT_DURATION_HPP__
#define __STOUT_DURATION_HPP__

#include <ctype.h> // For 'isdigit'.
#include <limits.h> // For 'LLONG_(MAX|MIN)'

#include <iomanip>
#include <iostream>
#include <string>

#include "error.hpp"
#include "numify.hpp"
#include "try.hpp"

class Duration
{
public:
  static Try<Duration> parse(const std::string& s)
  {
    // TODO(benh): Support negative durations (i.e., starts with '-').
    size_t index = 0;
    while (index < s.size()) {
      if (isdigit(s[index]) || s[index] == '.') {
        index++;
        continue;
      }

      Try<double> value = numify<double>(s.substr(0, index));

      if (value.isError()) {
        return Error(value.error());
      }

      const std::string& unit = s.substr(index);

      if (unit == "ns") {
        return Duration(value.get(), NANOSECONDS);
      } else if (unit == "us") {
        return Duration(value.get(), MICROSECONDS);
      } else if (unit == "ms") {
        return Duration(value.get(), MILLISECONDS);
      } else if (unit == "secs") {
        return Duration(value.get(), SECONDS);
      } else if (unit == "mins") {
        return Duration(value.get(), MINUTES);
      } else if (unit == "hrs") {
        return Duration(value.get(), HOURS);
      } else if (unit == "days") {
        return Duration(value.get(), DAYS);
      } else if (unit == "weeks") {
        return Duration(value.get(), WEEKS);
      } else {
        return Error("Unknown duration unit '" + unit + "'");
      }
    }
    return Error("Invalid duration '" + s + "'");
  }

  static Try<Duration> create(double seconds);

  Duration() : nanos(0) {}

  int64_t ns() const   { return nanos; }
  double us() const    { return static_cast<double>(nanos) / MICROSECONDS; }
  double ms() const    { return static_cast<double>(nanos) / MILLISECONDS; }
  double secs() const  { return static_cast<double>(nanos) / SECONDS; }
  double mins() const  { return static_cast<double>(nanos) / MINUTES; }
  double hrs() const   { return static_cast<double>(nanos) / HOURS; }
  double days() const  { return static_cast<double>(nanos) / DAYS; }
  double weeks() const { return static_cast<double>(nanos) / WEEKS; }

  bool operator <  (const Duration& d) const { return nanos <  d.nanos; }
  bool operator <= (const Duration& d) const { return nanos <= d.nanos; }
  bool operator >  (const Duration& d) const { return nanos >  d.nanos; }
  bool operator >= (const Duration& d) const { return nanos >= d.nanos; }
  bool operator == (const Duration& d) const { return nanos == d.nanos; }
  bool operator != (const Duration& d) const { return nanos != d.nanos; }

  Duration& operator += (const Duration& that)
  {
    nanos += that.nanos;
    return *this;
  }

  Duration& operator -= (const Duration& that)
  {
    nanos -= that.nanos;
    return *this;
  }

  Duration& operator *= (double multiplier)
  {
    nanos = static_cast<int64_t>(nanos * multiplier);
    return *this;
  }

  Duration& operator /= (double divisor)
  {
    nanos = static_cast<int64_t>(nanos / divisor);
    return *this;
  }

  Duration operator + (const Duration& that) const
  {
    Duration sum = *this;
    sum += that;
    return sum;
  }

  Duration operator - (const Duration& that) const
  {
    Duration diff = *this;
    diff -= that;
    return diff;
  }

  Duration operator * (double multiplier) const
  {
    Duration product = *this;
    product *= multiplier;
    return product;
  }

  Duration operator / (double divisor) const
  {
    Duration quotient = *this;
    quotient /= divisor;
    return quotient;
  }

  // TODO(xujyan): Use constexpr for the following variables after
  // switching to C++11.
  // A constant holding the maximum value a Duration can have.
  static Duration max();
  // A constant holding the minimum (negative) value a Duration can
  // have.
  static Duration min();
  // A constant holding a Duration of a "zero" value.
  static Duration zero() { return Duration(); }

protected:
  static const int64_t NANOSECONDS  = 1;
  static const int64_t MICROSECONDS = 1000 * NANOSECONDS;
  static const int64_t MILLISECONDS = 1000 * MICROSECONDS;
  static const int64_t SECONDS      = 1000 * MILLISECONDS;
  static const int64_t MINUTES      = 60 * SECONDS;
  static const int64_t HOURS        = 60 * MINUTES;
  static const int64_t DAYS         = 24 * HOURS;
  static const int64_t WEEKS        = 7 * DAYS;

  // For the Seconds, Minutes, Hours, Days & Weeks constructor.
  Duration(int32_t value, int64_t unit)
    : nanos(value * unit) {}

  // For the Nanoseconds, Microseconds, Milliseconds constructor.
  Duration(int64_t value, int64_t unit)
    : nanos(value * unit) {}

private:
  // Used only by "parse".
  Duration(double value, int64_t unit)
    : nanos(static_cast<int64_t>(value * unit)) {}

  int64_t nanos;
};


class Nanoseconds : public Duration
{
public:
  explicit Nanoseconds(int64_t nanoseconds)
    : Duration(nanoseconds, NANOSECONDS) {}

  Nanoseconds(const Duration& d) : Duration(d) {}
};


class Microseconds : public Duration
{
public:
  explicit Microseconds(int64_t microseconds)
    : Duration(microseconds, MICROSECONDS) {}

  Microseconds(const Duration& d) : Duration(d) {}
};


class Milliseconds : public Duration
{
public:
  explicit Milliseconds(int64_t milliseconds)
    : Duration(milliseconds, MILLISECONDS) {}

  Milliseconds(const Duration& d) : Duration(d) {}
};


class Seconds : public Duration
{
public:
  explicit Seconds(int64_t seconds)
    : Duration(seconds, SECONDS) {}

  Seconds(const Duration& d) : Duration(d) {}
};


class Minutes : public Duration
{
public:
  explicit Minutes(int32_t minutes)
    : Duration(minutes, MINUTES) {}

  Minutes(const Duration& d) : Duration(d) {}
};


class Hours : public Duration
{
public:
  explicit Hours(int32_t hours)
    : Duration(hours, HOURS) {}

  Hours(const Duration& d) : Duration(d) {}
};


class Days : public Duration
{
public:
  explicit Days(int32_t days)
    : Duration(days, DAYS) {}

  Days(const Duration& d) : Duration(d) {}
};


class Weeks : public Duration
{
public:
  explicit Weeks(int32_t value) : Duration(value, WEEKS) {}

  Weeks(const Duration& d) : Duration(d) {}
};


inline std::ostream& operator << (
    std::ostream& stream,
    const Duration& duration)
{
  long precision = stream.precision();

  // Output the duration in full double precision.
  stream.precision(std::numeric_limits<double>::digits10);

  if (duration < Microseconds(1)) {
    stream << duration.ns() << "ns";
  } else if (duration < Milliseconds(1)) {
    stream << duration.us() << "us";
  } else if (duration < Seconds(1)) {
    stream << duration.ms() << "ms";
  } else if (duration < Minutes(1)) {
    stream << duration.secs() << "secs";
  } else if (duration < Hours(1)) {
    stream << duration.mins() << "mins";
  } else if (duration < Days(1)) {
    stream << duration.hrs() << "hrs";
  } else if (duration < Weeks(1)) {
    stream << duration.days() << "days";
  } else {
    stream << duration.weeks() << "weeks";
  }

  // Return the stream to original formatting state.
  stream.precision(precision);

  return stream;
}


inline Try<Duration> Duration::create(double seconds)
{
  if (seconds * SECONDS > LLONG_MAX) {
    return Error("Argument larger than the maximum number of seconds that "
                 "a Duration can represent due to int64_t's size limit.");
  }

  return Nanoseconds(static_cast<int64_t>(seconds * SECONDS));
}


inline Duration Duration::max() { return Nanoseconds(LLONG_MAX); }


inline Duration Duration::min() { return Nanoseconds(LLONG_MIN); }

#endif // __STOUT_DURATION_HPP__
