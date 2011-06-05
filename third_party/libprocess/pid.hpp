#ifndef PID_HPP
#define PID_HPP

#include <stdint.h>

#include <iostream>
#include <sstream>
#include <string>


struct PID;


/* Outputing PIDs and generating PIDs using streamds. */
std::ostream & operator << (std::ostream &, const PID &);
std::istream & operator >> (std::istream &, PID &);


/* PID hash value (for example, to use in Boost's unordered maps). */
std::size_t hash_value(const PID &);


struct PID
{
  PID() : ip(0), port(0) {}

  PID(const char *id_, uint32_t ip_, uint16_t port_)
    : id(id_), ip(ip_), port(port_) {}

  PID(const std::string& id_, uint32_t ip_, uint16_t port_)
    : id(id_), ip(ip_), port(port_) {}

  PID(const char *s) 
  {
    std::istringstream in(s);
    in >> *this;
  }

  PID(const std::string &s)
  {
    std::istringstream in(s);
    in >> *this;
  }

  operator std::string() const
  {
    std::ostringstream out;
    out << *this;
    return out.str();
  }

  bool operator ! () const
  {
    return id == "" && ip == 0 && port == 0;
  }

  bool operator < (const PID &that) const
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

  bool operator == (const PID &that) const
  {
    if (this != &that) {
      return (id == that.id &&
              ip == that.ip &&
              port == that.port);
    }

    return true;
  }

  bool operator != (const PID &that) const
  {
    return !(this->operator == (that));
  }


  std::string id;
  uint32_t ip;
  uint16_t port;
};


#endif /* PID_HPP */
