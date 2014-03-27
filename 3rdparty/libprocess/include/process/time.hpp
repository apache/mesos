#ifndef __PROCESS_TIME_HPP__
#define __PROCESS_TIME_HPP__

#include <iomanip>

#include <glog/logging.h>

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

  bool operator <  (const Time& t) const { return sinceEpoch <  t.sinceEpoch; }
  bool operator <= (const Time& t) const { return sinceEpoch <= t.sinceEpoch; }
  bool operator >  (const Time& t) const { return sinceEpoch >  t.sinceEpoch; }
  bool operator >= (const Time& t) const { return sinceEpoch >= t.sinceEpoch; }
  bool operator == (const Time& t) const { return sinceEpoch == t.sinceEpoch; }
  bool operator != (const Time& t) const { return sinceEpoch != t.sinceEpoch; }

  Time& operator += (const Duration& d)
  {
    sinceEpoch += d;
    return *this;
  }

  Time& operator -= (const Duration& d)
  {
    sinceEpoch -= d;
    return *this;
  }

  Duration operator - (const Time& that) const
  {
    return sinceEpoch - that.sinceEpoch;
  }

  Time operator + (const Duration& duration) const
  {
    Time new_ = *this;
    new_ += duration;
    return new_;
  }

  Time operator - (const Duration& duration) const
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


// Outputs the time in RFC 3339 Format.
inline std::ostream& operator << (std::ostream& stream, const Time& time)
{
  // Round down the secs to use it with strftime and then append the
  // fraction part.
  long secs = static_cast<long>(time.secs());
  char date[64];

  // The RFC 3339 Format.
  tm* tm_ = gmtime(&secs);
  if (tm_ == NULL) {
    LOG(ERROR) << "Cannot convert the 'time' to a tm struct using gmtime(): "
               << errno;
    return stream;
  }

  strftime(date, 64, "%Y-%m-%d %H:%M:%S", tm_);
  stream << date;

  // Append the fraction part in nanoseconds.
  int64_t nsecs = (time.duration() - Seconds(secs)).ns();

  if (nsecs != 0) {
    char prev = stream.fill();

    // 9 digits for nanosecond level precision.
    stream << "." << std::setfill('0') << std::setw(9) << nsecs;

    // Return the stream to original formatting state.
    stream.fill(prev);
  }

  stream << "+00:00";
  return stream;
}

} // namespace process {

#endif // __PROCESS_TIME_HPP__
