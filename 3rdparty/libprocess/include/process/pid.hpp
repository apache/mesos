#ifndef __PROCESS_PID_HPP__
#define __PROCESS_PID_HPP__

#include <stdint.h>

#include <iostream>
#include <sstream>
#include <string>

#include <process/address.hpp>

namespace process {

// Forward declaration to break cyclic dependencies.
class ProcessBase;


struct UPID
{
  UPID() = default;

  UPID(const UPID& that)
    : id(that.id), address(that.address) {}

  UPID(const char* id_, uint32_t ip_, uint16_t port_)
    : id(id_), address(ip_, port_) {}

  UPID(const char* id_, const network::Address& address_)
    : id(id_), address(address_) {}

  UPID(const std::string& id_, uint32_t ip_, uint16_t port_)
    : id(id_), address(ip_, port_) {}

  UPID(const std::string& id_, const network::Address& address_)
    : id(id_), address(address_) {}

  /*implicit*/ UPID(const char* s);

  /*implicit*/ UPID(const std::string& s);

  /*implicit*/ UPID(const ProcessBase& process);

  operator std::string () const;

  operator bool () const
  {
    return id != "" && address.ip != 0 && address.port != 0;
  }

  bool operator ! () const // NOLINT(whitespace/operators)
  {
    return id == "" && address.ip == 0 && address.port == 0;
  }

  bool operator < (const UPID& that) const
  {
    if (address == that.address) {
      return id < that.id;
    } else {
      return address < that.address;
    }
  }

  bool operator == (const UPID& that) const
  {
    return (id == that.id && address == that.address);
  }

  bool operator != (const UPID& that) const
  {
    return !(*this == that);
  }

  std::string id;
  network::Address address;
};


template <typename T = ProcessBase>
struct PID : UPID
{
  PID() : UPID() {}

  /*implicit*/ PID(const T* t) : UPID(static_cast<const ProcessBase&>(*t)) {}
  /*implicit*/ PID(const T& t) : UPID(static_cast<const ProcessBase&>(t)) {}

  template <typename Base>
  operator PID<Base> () const
  {
    // Only allow upcasts!
    T* t = NULL;
    Base* base = t;
    (void)base;  // Eliminate unused base warning.
    PID<Base> pid;
    pid.id = id;
    pid.address = address;
    return pid;
  }
};


// Outputing UPIDs and generating UPIDs using streams.
std::ostream& operator << (std::ostream&, const UPID&);
std::istream& operator >> (std::istream&, UPID&);


// UPID hash value (for example, to use in Boost's unordered maps).
std::size_t hash_value(const UPID&);

}  // namespace process {

#endif // __PROCESS_PID_HPP__
