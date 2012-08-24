#ifndef __STOUT_TIME_HPP__
#define __STOUT_TIME_HPP__

#include <ctype.h> // For 'isdigit'.

#include <string>

#include "numify.hpp"
#include "try.hpp"

// Forward declarations.
struct hours;
struct minutes;
struct seconds;
struct milliseconds;
struct microseconds;
struct nanoseconds;


struct hours
{
  explicit hours(double _value) : value(_value) {}
  inline operator minutes () const;
  inline operator seconds () const;
  inline operator milliseconds () const;
  inline operator microseconds () const;
  inline operator nanoseconds () const;
  double mins() const { return value * 60.0; }
  double secs() const { return value * 3600.0; }
  double millis() const { return value * 3600.0 * 1000.0; }
  double micros() const { return value * 3600.0 * 1000000.0; }
  double nanos() const { return value * 3600.0 * 1000000000.0; }
  const double value;
};


struct minutes
{
  explicit minutes(double _value) : value(_value) {}
  inline operator hours () const;
  inline operator seconds () const;
  inline operator milliseconds () const;
  inline operator microseconds () const;
  inline operator nanoseconds () const;
  double hrs() const {return value / 60.0; }
  double secs() const { return value * 60.0; }
  double millis() const { return value * 60.0 * 1000.0; }
  double micros() const { return value * 60.0 * 1000000.0; }
  double nanos() const { return value * 60.0 * 1000000000.0; }
  const double value;
};


struct seconds
{
  explicit seconds(double _value) : value(_value) {}
  static inline Try<seconds> parse(const std::string& s);
  inline operator hours () const;
  inline operator minutes () const;
  inline operator milliseconds () const;
  inline operator microseconds () const;
  inline operator nanoseconds () const;
  double hrs() const { return value / 3600.0; }
  double mins() const { return value / 60.0; }
  double millis() const { return value * 1000.0; }
  double micros() const { return value * 1000000.0; }
  double nanos() const { return value * 1000000000.0; }
  bool operator < (const seconds& that) const { return value < that.value; }
  const double value;
};


struct milliseconds
{
  explicit milliseconds(double _value) : value(_value) {}
  inline operator hours () const;
  inline operator minutes () const;
  inline operator seconds () const;
  inline operator microseconds () const;
  inline operator nanoseconds () const;
  double hrs() const { return value / (3600.0 * 1000.0); }
  double mins() const { return value / (60.0 * 1000.0); }
  double secs() const { return value / 1000.0; }
  double micros() const { return value * 1000.0; }
  double nanos() const { return value * 1000000.0; }
  bool operator < (const milliseconds& that) const { return value < that.value; }
  const double value;
};


struct microseconds
{
  explicit microseconds(double _value) : value(_value) {}
  inline operator hours () const;
  inline operator minutes () const;
  inline operator seconds () const;
  inline operator milliseconds () const;
  inline operator nanoseconds () const;
  double hrs() const { return value / (3600.0 * 1000000.0); }
  double mins() const { return value / (60.0 * 1000000.0); }
  double secs() const { return value / 1000000.0; }
  double millis() const { return value / 1000.0; }
  double nanos() const { return value * 1000.0; }
  bool operator < (const microseconds& that) const { return value < that.value; }
  const double value;
};


struct nanoseconds
{
  explicit nanoseconds(double _value) : value(_value) {}
  inline operator hours () const;
  inline operator minutes () const;
  inline operator seconds () const;
  inline operator milliseconds () const;
  inline operator microseconds () const;
  double hrs() const { return value / (3600.0 * 1000000000.0); }
  double mins() const { return value / (60.0 * 1000000000.0); }
  double secs() const { return value / 1000000000.0; }
  double millis() const { return value / 1000000.0; }
  double micros() const { return value / 1000.0; }
  bool operator < (const nanoseconds& that) const { return value < that.value; }
  const double value;
};


inline hours::operator minutes () const
{
  return minutes(mins());
}


inline hours::operator seconds () const
{
  return seconds(secs());
}


inline hours::operator milliseconds () const
{
  return milliseconds(millis());
}


inline hours::operator microseconds () const
{
  return microseconds(micros());
}


inline hours::operator nanoseconds () const
{
  return nanoseconds(nanos());
}


inline minutes::operator hours () const
{
  return hours(hrs());
}


inline minutes::operator seconds () const
{
  return seconds(secs());
}


inline minutes::operator milliseconds () const
{
  return milliseconds(millis());
}


inline minutes::operator microseconds () const
{
  return microseconds(micros());
}


inline minutes::operator nanoseconds () const
{
  return nanoseconds(nanos());
}


inline Try<seconds> seconds::parse(const std::string& s)
{
  // TODO(benh): Support negative durations (i.e., starts with '-') as
  // well as values that use a decimal point.
  size_t index = 0;
  while (index < s.size()) {
    if (isdigit(s[index])) {
      index++;
      continue;
    }

    Try<double> value = numify<double>(s.substr(0, index));

    if (value.isError()) {
      return Try<seconds>::error(value.error());
    }

    const std::string& unit = s.substr(index);

    if (unit == "ns") {
      return seconds(nanoseconds(value.get()));
    } else if (unit == "us") {
      return seconds(microseconds(value.get()));
    } else if (unit == "ms") {
      return seconds(microseconds(value.get()));
    } else if (unit == "secs") {
      return seconds(value.get());
    } else if (unit == "mins") {
      return seconds(minutes(value.get()));
    } else if (unit == "hrs") {
      return seconds(hours(value.get()));
    } else {
      return Try<seconds>::error("Unknown duration unit '" + unit + "'");
    }
  }

  return Try<seconds>::error("Invalid duration string.");
}


inline seconds::operator hours () const
{
  return hours(hrs());
}


inline seconds::operator minutes () const
{
  return minutes(mins());
}


inline seconds::operator milliseconds () const
{
  return milliseconds(millis());
}


inline seconds::operator microseconds () const
{
  return microseconds(micros());
}


inline seconds::operator nanoseconds () const
{
  return nanoseconds(nanos());
}


inline milliseconds::operator hours () const
{
  return hours(hrs());
}


inline milliseconds::operator minutes () const
{
  return minutes(mins());
}


inline milliseconds::operator seconds () const
{
  return seconds(secs());
}


inline milliseconds::operator microseconds () const
{
  return microseconds(micros());
}


inline milliseconds::operator nanoseconds () const
{
  return nanoseconds(nanos());
}


inline microseconds::operator hours () const
{
  return hours(hrs());
}


inline microseconds::operator minutes () const
{
  return minutes(mins());
}


inline microseconds::operator seconds () const
{
  return seconds(secs());
}


inline microseconds::operator milliseconds () const
{
  return milliseconds(millis());
}


inline microseconds::operator nanoseconds () const
{
  return nanoseconds(nanos());
}


inline nanoseconds::operator hours () const
{
  return hours(hrs());
}


inline nanoseconds::operator minutes () const
{
  return minutes(mins());
}


inline nanoseconds::operator seconds () const
{
  return seconds(secs());
}


inline nanoseconds::operator milliseconds () const
{
  return milliseconds(millis());
}


inline nanoseconds::operator microseconds () const
{
  return microseconds(micros());
}

#endif // __STOUT_TIME_HPP__
