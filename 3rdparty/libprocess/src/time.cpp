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

#include <time.h>

#include <glog/logging.h>

#include <process/time.hpp>
#include <stout/os.hpp>

namespace process {

std::ostream& operator<<(std::ostream& stream, const RFC1123& formatter)
{
  time_t secs = static_cast<time_t>(formatter.time.secs());

  tm timeInfo = {};
  if (os::gmtime_r(&secs, &timeInfo) == nullptr) {
    PLOG(ERROR)
      << "Failed to convert from 'time_t' to a 'tm' struct "
      << "using os::gmtime_r()";
    return stream;
  }

  static const char* WEEK_DAYS[] = {
      "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
    };

  static const char* MONTHS[] = {
      "Jan",
      "Feb",
      "Mar",
      "Apr",
      "May",
      "Jun",
      "Jul",
      "Aug",
      "Sep",
      "Oct",
      "Nov",
      "Dec"
    };

  char buffer[64] = {};

  // 'strftime' cannot be used since it depends on the locale, which
  // is not useful when using the RFC 1123 format in HTTP Headers.
  if (snprintf(
          buffer,
          sizeof(buffer),
          "%s, %02d %s %d %02d:%02d:%02d GMT",
          WEEK_DAYS[timeInfo.tm_wday],
          timeInfo.tm_mday,
          MONTHS[timeInfo.tm_mon],
          timeInfo.tm_year + 1900,
          timeInfo.tm_hour,
          timeInfo.tm_min,
          timeInfo.tm_sec) < 0) {
    LOG(ERROR)
      << "Failed to format the 'time' to a string using snprintf";
    return stream;
  }

  stream << buffer;

  return stream;
}


std::ostream& operator<<(std::ostream& stream, const RFC3339& formatter)
{
  // Round down the secs to use it with strftime and then append the
  // fraction part.
  time_t secs = static_cast<time_t>(formatter.time.secs());

  // The RFC 3339 Format.
  tm timeInfo = {};
  if (os::gmtime_r(&secs, &timeInfo) == nullptr) {
    PLOG(ERROR)
      << "Failed to convert from 'time_t' to a 'tm' struct "
      << "using os::gmtime_r()";
    return stream;
  }

  char buffer[64] = {};

  strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &timeInfo);
  stream << buffer;

  // Append the fraction part in nanoseconds.
  int64_t nanoSeconds = (formatter.time.duration() - Seconds(secs)).ns();
  if (nanoSeconds != 0) {
    char prev = stream.fill();

    // 9 digits for nanosecond level precision.
    stream << "." << std::setfill('0') << std::setw(9) << nanoSeconds;

    // Return the stream to original formatting state.
    stream.fill(prev);
  }

  stream << "+00:00";
  return stream;
}

} // namespace process {
