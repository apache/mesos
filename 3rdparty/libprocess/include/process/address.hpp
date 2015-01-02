#ifndef __PROCESS_ADDRESS_HPP__
#define __PROCESS_ADDRESS_HPP__

#include <stdint.h>
#include <unistd.h>

#include <arpa/inet.h>

#include <glog/logging.h>

#include <sstream>

#include <boost/functional/hash.hpp>

namespace process {
namespace network {

// Represents a network "address", subsuming the struct addrinfo and
// struct sockaddr* that typically is used to encapsulate IP and port.
//
// TODO(benh): Create a Family enumeration to replace sa_family_t.
class Address
{
public:
  Address() : ip(0), port(0) {}

  Address(uint32_t _ip, uint16_t _port) : ip(_ip), port(_port) {}

  bool operator < (const Address& that) const
  {
    if (ip == that.ip) {
      return port < that.port;
    } else {
      return ip < that.ip;
    }
  }

  bool operator > (const Address& that) const
  {
    if (ip == that.ip) {
      return port > that.port;
    } else {
      return ip > that.ip;
    }
  }

  bool operator == (const Address& that) const
  {
    return (ip == that.ip && port == that.port);
  }

  bool operator != (const Address& that) const
  {
    return !(*this == that);
  }

  sa_family_t family() const
  {
    return AF_INET;
  }

  uint32_t ip;
  uint16_t port;
};


inline std::ostream& operator << (std::ostream& stream, const Address& address)
{
  char ip[INET_ADDRSTRLEN];
  if (inet_ntop(AF_INET, (in_addr*) &address.ip, ip, INET_ADDRSTRLEN) == NULL) {
    PLOG(FATAL) << "Failed to get human-readable IP address for '"
                << address.ip << "'";
  }
  stream << ip << ":" << address.port;
  return stream;
}


// Address hash value (for example, to use in Boost's unordered maps).
inline std::size_t hash_value(const Address& address)
{
  size_t seed = 0;
  boost::hash_combine(seed, address.ip);
  boost::hash_combine(seed, address.port);
  return seed;
}

} // namespace network {
} // namespace process {

#endif // __PROCESS_ADDRESS_HPP__
