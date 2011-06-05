#include <netdb.h>

#include <arpa/inet.h>

#include <iostream>
#include <string>

#include <boost/unordered_map.hpp>

#include "config.hpp"
#include "pid.hpp"

using std::istream;
using std::ostream;
using std::size_t;
using std::string;


ostream & operator << (ostream &stream, const PID &pid)
{
  // Call inet_ntop since inet_ntoa is not thread-safe!
  char ip[INET_ADDRSTRLEN];
  if (inet_ntop(AF_INET, (in_addr *) &pid.ip, ip, INET_ADDRSTRLEN) == NULL)
    memset(ip, 0, INET_ADDRSTRLEN);

  stream << pid.pipe << "@" << ip << ":" << pid.port;
  return stream;
}


istream & operator >> (istream &stream, PID &pid)
{
  pid.pipe = 0;
  pid.ip = 0;
  pid.port = 0;

  string str;
  if (!(stream >> str)) {
    stream.setstate(std::ios_base::badbit);
    return stream;
  }

  if (str.size() > 500) {
    stream.setstate(std::ios_base::badbit);
    return stream;
  }

  char host[512];
  int id;
  unsigned short port;
  if (sscanf(str.c_str(), "%d@%[^:]:%hu", &id, host, &port) != 3) {
    stream.setstate(std::ios_base::badbit);
    return stream;
  }

  hostent *he = gethostbyname2(host, AF_INET);
  if (!he) {
    stream.setstate(std::ios_base::badbit);
    return stream;
  }

  pid.pipe = id;
  pid.ip = *((uint32_t *) he->h_addr);
  pid.port = port;
  return stream;
}


size_t hash_value(const PID& pid)
{
  size_t seed = 0;
  boost::hash_combine(seed, pid.pipe);
  boost::hash_combine(seed, pid.ip);
  boost::hash_combine(seed, pid.port);
  return seed;
}
