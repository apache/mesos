#ifndef __PROCESS_PID_HPP__
#define __PROCESS_PID_HPP__

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

  /*implicit*/ UPID(const char* s);

  /*implicit*/ UPID(const std::string& s);

  /*implicit*/ UPID(const ProcessBase& process);

  operator std::string () const;

  operator bool () const
  {
    return id != "" && ip != 0 && port != 0;
  }

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
    pid.ip = ip;
    pid.port = port;
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
