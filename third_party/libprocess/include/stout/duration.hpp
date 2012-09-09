#ifndef __STOUT_DURATION_HPP__
#define __STOUT_DURATION_HPP__

#include <ctype.h> // For 'isdigit'.

#include <string>

#include "numify.hpp"
#include "try.hpp"


class Duration
{
public:
  double nanos() const { return value; }
  double micros() const { return value / MICROSECONDS; }
  double millis() const { return value / MILLISECONDS; }
  double secs() const { return value / SECONDS; }
  double mins() const { return value / MINUTES; }
  double days() const { return value / DAYS; }
  double weeks() const { return value / WEEKS; }

  bool operator < (const Duration& that) const { return value < that.value; }

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
        return Try<Duration>::error(value.error());
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
        return Try<Duration>::error("Unknown duration unit '" + unit + "'");
      }
    }
    return Try<Duration>::error("Invalid duration '" + s + "'");
  }

protected:
  static const uint64_t NANOSECONDS = 1;
  static const uint64_t MICROSECONDS = 1000 * NANOSECONDS;
  static const uint64_t MILLISECONDS = 1000 * MICROSECONDS;
  static const uint64_t SECONDS = 1000 * MILLISECONDS;
  static const uint64_t MINUTES = 60 * SECONDS;
  static const uint64_t HOURS = 60 * MINUTES;
  static const uint64_t DAYS = 24 * HOURS;
  static const uint64_t WEEKS = 7 * DAYS;

  Duration(double _value, uint64_t _unit)
  {
    // Convert to the lowest unit (nanoseconds).
    value = _value * _unit;
  }

private:
  double value;
};


class Nanoseconds : public Duration
{
public:
  explicit Nanoseconds(double value)
    : Duration(value, NANOSECONDS) {}
};


class Microseconds : public Duration
{
public:
  explicit Microseconds(double value)
    : Duration(value, MICROSECONDS) {}
};


class Milliseconds : public Duration
{
public:
  explicit Milliseconds(double value)
    : Duration(value, MILLISECONDS) {}
};


class Seconds : public Duration
{
public:
  explicit Seconds(double value)
    : Duration(value, SECONDS) {}
};


class Minutes : public Duration
{
public:
  explicit Minutes(double value)
    : Duration(value, MINUTES) {}
};


class Hours : public Duration
{
public:
  explicit Hours(double value)
    : Duration(value, HOURS) {}
};


class Days : public Duration
{
public:
  explicit Days(double value)
    : Duration(value, DAYS) {}
};


class Weeks : public Duration
{
public:
  explicit Weeks(double value)
    : Duration(value, WEEKS) {}
};

#endif // __STOUT_DURATION_HPP__
