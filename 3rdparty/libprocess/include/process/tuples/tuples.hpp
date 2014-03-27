#ifndef __PROCESS_TUPLES_HPP__
#define __PROCESS_TUPLES_HPP__

#include <stdint.h>
#include <stdlib.h>

#include <arpa/inet.h>

#include <sstream>
#include <string>
#include <utility>

#include <boost/tuple/tuple.hpp>


namespace process { namespace tuples {

// TODO(benh): Check stream errors! Report errors! Ahhhh!

struct serializer
{
  std::ostringstream& stream;

  explicit serializer(std::ostringstream& s) : stream(s) {}

  void operator & (const int32_t & i)
  {
    uint32_t netInt = htonl((uint32_t) i);
    stream.write((char *) &netInt, sizeof(netInt));
  }

  void operator & (const int64_t & i)
  {
    uint32_t hiInt = htonl((uint32_t) (i >> 32));
    uint32_t loInt = htonl((uint32_t) (i & 0xFFFFFFFF));
    stream.write((char *) &hiInt, sizeof(hiInt));
    stream.write((char *) &loInt, sizeof(loInt));
  }

#ifdef __APPLE__
  void operator & (const intptr_t &i)
  {
    if (sizeof(intptr_t) == sizeof(int32_t))
      *this & ((int32_t &) i);
    else if (sizeof(intptr_t) == sizeof(int64_t))
      *this & ((int64_t &) i);
    else
      abort();
  }
#endif

  void operator & (const size_t &i)
  {
    if (sizeof(size_t) == sizeof(int32_t))
      *this & ((int32_t &) i);
    else if (sizeof(size_t) == sizeof(int64_t))
      *this & ((int64_t &) i);
    else
      abort();
  }

  void operator & (const double &d)
  {
    // TODO(*): Deal with endian issues?
    stream.write((char *) &d, sizeof(d));
  }

  void operator & (const std::string &s)
  {
    size_t size = s.size();
    *this & (size);
    stream.write(s.data(), size);
  }

  void operator & (const PID &pid)
  {
    *this & ((int32_t) pid.pipe);
    *this & ((int32_t) pid.ip);
    *this & ((int32_t) pid.port);
  }
};


struct deserializer
{
  std::istringstream &stream;

  explicit deserializer(std::istringstream &s) : stream(s) {}

  void operator & (int32_t &i)
  {
    uint32_t netInt;
    stream.read((char *) &netInt, sizeof(netInt));
    i = ntohl(netInt);
  }

  void operator & (int64_t &i)
  {
    uint32_t hiInt, loInt;
    stream.read((char *) &hiInt, sizeof(hiInt));
    stream.read((char *) &loInt, sizeof(loInt));
    int64_t hi64 = ntohl(hiInt);
    int64_t lo64 = ntohl(loInt);
    i = (hi64 << 32) | lo64;
  }

#ifdef __APPLE__
  void operator & (intptr_t &i)
  {
    if (sizeof(intptr_t) == sizeof(int32_t))
      *this & ((int32_t &) i);
    else if (sizeof(intptr_t) == sizeof(int64_t))
      *this & ((int64_t &) i);
    else
      abort();
  }
#endif

  void operator & (size_t &i)
  {
    if (sizeof(size_t) == sizeof(int32_t))
      *this & ((int32_t &) i);
    else if (sizeof(size_t) == sizeof(int64_t))
      *this & ((int64_t &) i);
    else
      abort();
  }

  void operator & (double &d)
  {
    // TODO(*): Deal with endian issues?
    stream.read((char *) &d, sizeof(d));
  }

  void operator & (std::string &s)
  {
    size_t size;
    *this & (size);
    s.resize(size);
    stream.read((char *) s.data(), size);
  }

  void operator & (PID &pid)
  {
    *this & ((int32_t &) pid.pipe);
    *this & ((int32_t &) pid.ip);
    *this & ((int32_t &) pid.port);
  }
};


}} // namespace process { namespace tuples {


#endif // __PROCESS_TUPLES_HPP__
