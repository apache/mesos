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

#ifndef __PROCESS_TIME_HPP__
#define __PROCESS_TIME_HPP__

#include <iomanip>

#include <stout/duration.hpp>

namespace process {

// Represents an instant in time.
class Time
{
public:
  // Constructs a time at the Epoch. It is needed because collections
  // (e.g., std::map) require a default constructor to construct
  // empty values.
  Time() : sinceEpoch(Duration::zero()) {}

  static Time epoch();
  static Time max();

  static Try<Time> create(double seconds);

  Duration duration() const { return sinceEpoch; }

  double secs() const { return sinceEpoch.secs(); }

  bool operator<(const Time& t) const { return sinceEpoch < t.sinceEpoch; }
  bool operator<=(const Time& t) const { return sinceEpoch <= t.sinceEpoch; }
  bool operator>(const Time& t) const { return sinceEpoch > t.sinceEpoch; }
  bool operator>=(const Time& t) const { return sinceEpoch >= t.sinceEpoch; }
  bool operator==(const Time& t) const { return sinceEpoch == t.sinceEpoch; }
  bool operator!=(const Time& t) const { return sinceEpoch != t.sinceEpoch; }

  Time& operator+=(const Duration& d)
  {
    sinceEpoch += d;
    return *this;
  }

  Time& operator-=(const Duration& d)
  {
    sinceEpoch -= d;
    return *this;
  }

  Duration operator-(const Time& that) const
  {
    return sinceEpoch - that.sinceEpoch;
  }

  Time operator+(const Duration& duration) const
  {
    Time new_ = *this;
    new_ += duration;
    return new_;
  }

  Time operator-(const Duration& duration) const
  {
    Time new_ = *this;
    new_ -= duration;
    return new_;
  }

private:
  Duration sinceEpoch;

  // Made it private to avoid the confusion between Time and Duration.
  // Users should explicitly use Clock::now() and Time::create() to
  // create a new time instance.
  explicit Time(const Duration& _sinceEpoch) : sinceEpoch(_sinceEpoch) {}
};

inline Time Time::epoch() { return Time(Duration::zero()); }
inline Time Time::max() { return Time(Duration::max()); }


// Stream manipulator class which serializes Time objects in RFC 1123
// format (Also known as HTTP Date format).
// The serialization is independent from the locale and ready to be
// used in HTTP Headers.
// Example: Wed, 15 Nov 1995 04:58:08 GMT
// See http://www.w3.org/Protocols/rfc2616/rfc2616-sec14.html
// section 14.18.
// See https://www.ietf.org/rfc/rfc1123.txt section 5.2.14
class RFC1123
{
public:
  explicit RFC1123(const Time& _time) : time(_time) {}

private:
  friend std::ostream& operator<<(std::ostream& out, const RFC1123& format);

  const Time time;
};


std::ostream& operator<<(std::ostream& out, const RFC1123& formatter);


// Stream manipulator class which serializes Time objects in RFC 3339
// format.
// Example: 1996-12-19T16:39:57-08:00,234
class RFC3339
{
public:
  explicit RFC3339(const Time& _time) : time(_time) {}

private:
  friend std::ostream& operator<<(std::ostream& out, const RFC3339& format);

  const Time time;
};


std::ostream& operator<<(std::ostream& out, const RFC3339& formatter);


// Outputs the time in RFC 3339 Format.
inline std::ostream& operator<<(std::ostream& stream, const Time& time)
{
  stream << RFC3339(time);
  return stream;
}

} // namespace process {

#endif // __PROCESS_TIME_HPP__
