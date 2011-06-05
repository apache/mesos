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
  PID() : pipe(0), ip(0), port(0) {}


  PID(const char *s) 
  {
    std::istringstream iss(s);
    iss >> *this;
  }


  PID(const std::string &s)
  {
    std::istringstream iss(s);
    iss >> *this;
  }


  operator std::string() const
  {
    std::ostringstream oss;
    oss << *this;
    return oss.str();
  }


  bool operator ! () const
  {
    return !pipe && !ip && !port;
  }


  bool operator < (const PID &that) const
  {
    if (this != &that) {
      if (ip == that.ip && port == that.port)
        return pipe < that.pipe;
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
      return (pipe == that.pipe &&
              ip == that.ip &&
              port == that.port);
    }

    return true;
  }


  bool operator != (const PID &that) const
  {
    return !(this->operator == (that));
  }


  uint32_t pipe;
  uint32_t ip;
  uint16_t port;
};


#endif /* PID_HPP */
