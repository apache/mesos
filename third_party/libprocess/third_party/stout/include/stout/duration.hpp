#ifndef __STOUT_DURATION_HPP__
#define __STOUT_DURATION_HPP__

#include <ctype.h> // For 'isdigit'.

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

  Duration() : seconds(0.0) {}

  double ns() const    { return seconds / NANOSECONDS; }
  double us() const    { return seconds / MICROSECONDS; }
  double ms() const    { return seconds / MILLISECONDS; }
  double secs() const  { return seconds / SECONDS; }
  double mins() const  { return seconds / MINUTES; }
  double hrs() const   { return seconds / HOURS; }
  double days() const  { return seconds / DAYS; }
  double weeks() const { return seconds / WEEKS; }

  bool operator <  (const Duration& d) const { return seconds <  d.seconds; }
  bool operator <= (const Duration& d) const { return seconds <= d.seconds; }
  bool operator >  (const Duration& d) const { return seconds >  d.seconds; }
  bool operator >= (const Duration& d) const { return seconds >= d.seconds; }
  bool operator == (const Duration& d) const { return seconds == d.seconds; }
  bool operator != (const Duration& d) const { return seconds != d.seconds; }

  // TODO(vinod): Overload arithmetic operators.

protected:
  static const double NANOSECONDS =  0.000000001;
  static const double MICROSECONDS = 0.000001;
  static const double MILLISECONDS = 0.001;
  static const uint64_t SECONDS    = 1;
  static const uint64_t MINUTES    = 60 * SECONDS;
  static const uint64_t HOURS      = 60 * MINUTES;
  static const uint64_t DAYS       = 24 * HOURS;
  static const uint64_t WEEKS      = 7 * DAYS;

  Duration(double value, double unit)
    : seconds(value * unit) {}

private:
  double seconds;
};


class Nanoseconds : public Duration
{
public:
  explicit Nanoseconds(double nanoseconds)
    : Duration(nanoseconds, NANOSECONDS) {}
};


class Microseconds : public Duration
{
public:
  explicit Microseconds(double microseconds)
    : Duration(microseconds, MICROSECONDS) {}
};


class Milliseconds : public Duration
{
public:
  explicit Milliseconds(double milliseconds)
    : Duration(milliseconds, MILLISECONDS) {}
};


class Seconds : public Duration
{
public:
  explicit Seconds(double seconds)
    : Duration(seconds, SECONDS) {}
};


class Minutes : public Duration
{
public:
  explicit Minutes(double minutes)
    : Duration(minutes, MINUTES) {}
};


class Hours : public Duration
{
public:
  explicit Hours(double hours)
    : Duration(hours, HOURS) {}
};


class Days : public Duration
{
public:
  explicit Days(double days)
    : Duration(days, DAYS) {}
};


class Weeks : public Duration
{
public:
  explicit Weeks(double value) : Duration(value, WEEKS) {}
};


inline std::ostream& operator << (
    std::ostream& stream,
    const Duration& duration)
{
  // Fix the number digits after the decimal point.
  std::ios_base::fmtflags flags = stream.flags();
  long precision = stream.precision();

  stream.setf(std::ios::fixed, std::ios::floatfield);
  stream.precision(2);

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
  stream.unsetf(std::ios::floatfield);
  stream.setf(flags);
  stream.precision(precision);

  return stream;
}

#endif // __STOUT_DURATION_HPP__
