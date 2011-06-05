#ifndef __PID_HPP__
#define __PID_HPP__

#include <stdint.h>

#include <iostream>
#include <sstream>
#include <string>


namespace process {

// Forward declaration to break cyclic dependencies.
class ProcessBase;


struct UPID
{
  UPID()
    : ip(0), port(0) {}

  UPID(const UPID& that)
    : id(that.id), ip(that.ip), port(that.port) {}

  UPID(const char* id_, uint32_t ip_, uint16_t port_)
    : id(id_), ip(ip_), port(port_) {}

  UPID(const std::string& id_, uint32_t ip_, uint16_t port_)
    : id(id_), ip(ip_), port(port_) {}

  UPID(const char* s);

  UPID(const std::string& s);

  UPID(const ProcessBase& process);

  operator std::string() const;

  bool operator ! () const
  {
    return id == "" && ip == 0 && port == 0;
  }

  bool operator < (const UPID& that) const
  {
    if (this != &that) {
      if (ip == that.ip && port == that.port)
        return id < that.id;
      else if (ip == that.ip && port != that.port)
        return port < that.port;
      else
        return ip < that.ip;
    }

    return false;
  }

  bool operator == (const UPID& that) const
  {
    if (this != &that) {
      return (id == that.id &&
              ip == that.ip &&
              port == that.port);
    }

    return true;
  }

  bool operator != (const UPID& that) const
  {
    return !(this->operator == (that));
  }

  std::string id;
  uint32_t ip;
  uint16_t port;
};


template <typename T = ProcessBase>
struct PID : UPID
{
  PID() : UPID() {}
  PID(const T& t) : UPID(static_cast<const ProcessBase&>(t)) {}
};

}  // namespace process {


// Outputing UPIDs and generating UPIDs using streams.
std::ostream& operator << (std::ostream&, const struct process::UPID&);
std::istream& operator >> (std::istream&, struct process::UPID&);


// UPID hash value (for example, to use in Boost's unordered maps).
std::size_t hash_value(const struct process::UPID&);

#endif // __PID_HPP__
