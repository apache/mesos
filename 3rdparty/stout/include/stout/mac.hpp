// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//  http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef __STOUT_MAC_HPP__
#define __STOUT_MAC_HPP__

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef __WINDOWS__
#include <arpa/inet.h>
#endif // __WINDOWS__

#ifdef __linux__
#include <linux/if.h>
#include <linux/if_packet.h>
#endif

#ifndef __WINDOWS__
#include <net/ethernet.h>
#endif // __WINDOWS__

#ifdef __APPLE__
#include <net/if.h>
#include <net/if_dl.h>
#include <net/if_types.h>
#endif

#ifndef __WINDOWS__
#include <sys/socket.h>
#endif // __WINDOWS__
#include <sys/types.h>

#include <iostream>
#include <string>

#include <stout/abort.hpp>
#include <stout/error.hpp>
#include <stout/none.hpp>
#include <stout/result.hpp>
#include <stout/stringify.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>


// Network utilities.
namespace net {

// Represents a MAC address. A MAC address is a 48-bit unique
// identifier assigned to a network interface for communications on
// the physical network segment. We use a byte array (in transmission
// order) to represent a MAC address. For example, for a MAC address
// 01:23:34:67:89:ab, the format is shown as follows:
//
//    MSB                                          LSB
//     |                                            |
//     v                                            v
// +--------+--------+--------+--------+--------+--------+
// |bytes[0]|bytes[1]|bytes[2]|bytes[3]|bytes[4]|bytes[5]|
// +--------+--------+--------+--------+--------+--------+
//
//     01   :   23   :   45   :   67   :   89   :   ab
// NOLINT(readability/ending_punctuation)
class MAC
{
public:
  // Parse a MAC address (e.g., 01:23:34:67:89:ab).
  static Try<MAC> parse(const std::string& s)
  {
    std::vector<std::string> tokens = strings::split(s, ":");
    if (tokens.size() != 6) {
      return Error("Invalid format. Expecting xx:xx:xx:xx:xx:xx");
    }

    auto isValidHexDigit = [](char c) {
      return (c >= '0' && c <= '9') ||
          (c >= 'a' && c <= 'f') ||
          (c >= 'A' && c <= 'F');
    };

    uint8_t bytes[6];
    for (size_t i = 0; i < 6; i++) {
      if (tokens[i].size() != 2) {
        return Error("Not a two digit hex number");
      }

      if (!isValidHexDigit(tokens[i][0]) ||
          !isValidHexDigit(tokens[i][1])) {
        return Error("Not a valid hex number");
      }

      const char* str = tokens[i].c_str();
      char *endptr = nullptr;
      unsigned long value = strtoul(str, &endptr, 16);

      assert(endptr == str + 2);
      assert(value < 256);

      bytes[i] = static_cast<uint8_t>(value);
    }

    return MAC(bytes);
  }

  // Constructs a MAC address from a byte array.
  explicit MAC(const uint8_t (&_bytes)[6])
  {
    for (size_t i = 0; i < 6; i++) {
      bytes[i] = _bytes[i];
    }
  }

  // Returns the byte at the given index. For example, for a MAC
  // address 01:23:45:67:89:ab, mac[0] = 01, mac[1] = 23 and etc.
  uint8_t operator[](size_t index) const
  {
    if (index >= 6) {
      ABORT("Invalid index specified in MAC::operator[]\n");
    }

    return bytes[index];
  }

  bool operator==(const MAC& that) const
  {
    for (size_t i = 0; i < 6; i++) {
      if (bytes[i] != that.bytes[i]) {
        return false;
      }
    }
    return true;
  }

  bool operator!=(const MAC& that) const
  {
    return !(*this == that);
  }

private:
  // Byte array of this MAC address (in transmission order).
  uint8_t bytes[6];
};


// Returns the standard string format (IEEE 802) of the given MAC
// address, which contains six groups of two hexadecimal digits,
// separated by colons, in transmission order (e.g.,
// 01:23:45:67:89:ab).
inline std::ostream& operator<<(std::ostream& stream, const MAC& mac)
{
  char buffer[18];

  sprintf(
      buffer,
      "%02hhx:%02hhx:%02hhx:%02hhx:%02hhx:%02hhx",
      mac[0],
      mac[1],
      mac[2],
      mac[3],
      mac[4],
      mac[5]);

  return stream << buffer;
}

} // namespace net {


// NOTE: These headers are placed here because the platform specific code
// requires classes defined in this file.
#ifdef __WINDOWS__
#include <stout/windows/mac.hpp>
#else
#include <stout/posix/mac.hpp>
#endif // __WINDOWS__

#endif // __STOUT_MAC_HPP__
