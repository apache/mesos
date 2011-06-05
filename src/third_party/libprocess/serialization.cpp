#ifdef __APPLE__
#define _XOPEN_SOURCE
#endif /* __APPLE__ */

#include <stdlib.h>

#include <arpa/inet.h>

#include "process.hpp"
#include "serialization.hpp"

namespace process { namespace serialization {

/* TODO(*): Check stream errors! Report errors! Ahhhh! */

void serializer::operator & (const int32_t &i)
{
  uint32_t netInt = htonl((uint32_t) i);
  stream.write((char *) &netInt, sizeof(netInt));
}


void serializer::operator & (const int64_t &i)
{
  uint32_t hiInt = htonl((uint32_t) (i >> 32));
  uint32_t loInt = htonl((uint32_t) (i & 0xFFFFFFFF));
  stream.write((char *) &hiInt, sizeof(hiInt));
  stream.write((char *) &loInt, sizeof(loInt));
}


void serializer::operator & (const size_t &i)
{
  if (sizeof(size_t) == sizeof(int32_t))
    this->operator & ((int32_t &) i);
  else if (sizeof(size_t) == sizeof(int64_t))
    this->operator & ((int64_t &) i);
  else
    abort();
}


void serializer::operator & (const std::string &str)
{
  size_t size = str.size();
  this->operator & (size);
  stream.write(str.data(), size);
}


void serializer::operator & (const PID &pid)
{
  this->operator & ((int32_t) pid.pipe);
  this->operator & ((int32_t) pid.ip);
  this->operator & ((int32_t) pid.port);
}


void deserializer::operator & (int32_t &i)
{
  uint32_t netInt;
  stream.read((char *) &netInt, sizeof(netInt));
  i = ntohl(netInt);
}

void deserializer::operator & (int64_t &i)
{
  uint32_t hiInt, loInt;
  stream.read((char *) &hiInt, sizeof(hiInt));
  stream.read((char *) &loInt, sizeof(loInt));
  int64_t hi64 = ntohl(hiInt);
  int64_t lo64 = ntohl(loInt);
  i = (hi64 << 32) | lo64;
}

void deserializer::operator & (size_t &i)
{
  if (sizeof(size_t) == sizeof(int32_t))
    this->operator & ((int32_t &) i);
  else if (sizeof(size_t) == sizeof(int64_t))
    this->operator & ((int64_t &) i);
  else
    abort();
}  

void deserializer::operator & (std::string &str)
{
  size_t size;
  this->operator & (size);
  str.resize(size);
  stream.read((char *) str.data(), size);
}

void deserializer::operator & (PID &pid)
{
  this->operator & ((int32_t &) pid.pipe);
  this->operator & ((int32_t &) pid.ip);
  this->operator & ((int32_t &) pid.port);
}


}}
