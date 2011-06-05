#include <netdb.h>
#include <stdio.h>
#include <string.h>

#include <arpa/inet.h>

#include <iostream>
#include <string>

#include <boost/unordered_map.hpp>

#include "config.hpp"
#include "pid.hpp"
#include "process.hpp"


using std::istream;
using std::ostream;
using std::size_t;
using std::string;


namespace process {

UPID::UPID(const char* s)
{
  std::istringstream in(s);
  in >> *this;
}


UPID::UPID(const std::string& s)
{
  std::istringstream in(s);
  in >> *this;
}


// TODO(benh): Make this inline-able (cyclic dependency issues).
UPID::UPID(const ProcessBase& process)
{
  id = process.self().id;
  ip = process.self().ip;
  port = process.self().port;
}


UPID::operator std::string() const
{
  std::ostringstream out;
  out << *this;
  return out.str();
}


ostream& operator << (ostream& stream, const UPID& pid)
{
  // Call inet_ntop since inet_ntoa is not thread-safe!
  char ip[INET_ADDRSTRLEN];
  if (inet_ntop(AF_INET, (in_addr *) &pid.ip, ip, INET_ADDRSTRLEN) == NULL)
    memset(ip, 0, INET_ADDRSTRLEN);

  stream << pid.id << "@" << ip << ":" << pid.port;
  return stream;
}


istream& operator >> (istream& stream, UPID& pid)
{
  pid.id = "";
  pid.ip = 0;
  pid.port = 0;

  string str;
  if (!(stream >> str)) {
    stream.setstate(std::ios_base::badbit);
    return stream;
  }

  if (str.size() > 512) {
    stream.setstate(std::ios_base::badbit);
    return stream;
  }

  string id;
  string host;
  unsigned short port;

  size_t index = str.find('@');

  if (index != string::npos) {
    id = str.substr(0, index);
  } else {
    stream.setstate(std::ios_base::badbit);
    return stream;
  }

  str = str.substr(index + 1, str.size() - index);

  index = str.find(':');

  if (index != string::npos) {
    host = str.substr(0, index);
  } else {
    stream.setstate(std::ios_base::badbit);
    return stream;
  }

  hostent *he = gethostbyname2(host.c_str(), AF_INET);
  if (!he) {
    stream.setstate(std::ios_base::badbit);
    return stream;
  }

  str = str.substr(index + 1, str.size() - index);

  if (sscanf(str.c_str(), "%hu", &port) != 1) {
    stream.setstate(std::ios_base::badbit);
    return stream;
  }

  pid.id = id;
  pid.ip = *((uint32_t *) he->h_addr);
  pid.port = port;

  return stream;
}


size_t hash_value(const UPID& pid)
{
  size_t seed = 0;
  boost::hash_combine(seed, pid.id);
  boost::hash_combine(seed, pid.ip);
  boost::hash_combine(seed, pid.port);
  return seed;
}

} // namespace process {
