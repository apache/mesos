#include <errno.h>
#include <netdb.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <arpa/inet.h>

#include <glog/logging.h>

#include <iostream>
#include <string>

#include <boost/unordered_map.hpp>

#include <process/pid.hpp>
#include <process/process.hpp>

#include <stout/os.hpp>

#include "config.hpp"


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

  VLOG(2) << "Attempting to parse '" << str << "' into a PID";

  if (str.size() == 0) {
    stream.setstate(std::ios_base::badbit);
    return stream;
  }

  string id;
  string host;
  uint32_t ip;
  uint16_t port;

  size_t index = str.find('@');

  if (index != string::npos) {
    id = str.substr(0, index);
  } else {
    stream.setstate(std::ios_base::badbit);
    return stream;
  }

  str = str.substr(index + 1);

  index = str.find(':');

  if (index != string::npos) {
    host = str.substr(0, index);
  } else {
    stream.setstate(std::ios_base::badbit);
    return stream;
  }

  hostent he, *hep;
  char* temp;
  size_t length;
  int result;
  int herrno;

  // Allocate temporary buffer for gethostbyname2_r.
  length = 1024;
  temp = new char[length];

  while ((result = gethostbyname2_r(
      host.c_str(), AF_INET, &he, temp, length, &hep, &herrno)) == ERANGE) {
    // Enlarge the buffer.
    delete[] temp;
    length *= 2;
    temp = new char[length];
  }

  if (result != 0 || hep == NULL) {
    VLOG(2) << "Failed to parse host '" << host
            << "' because " << hstrerror(herrno);
    delete[] temp;
    stream.setstate(std::ios_base::badbit);
    return stream;
  }

  if (hep->h_addr_list[0] == NULL) {
    VLOG(2) << "Got no addresses for '" << host << "'";
    delete[] temp;
    stream.setstate(std::ios_base::badbit);
    return stream;
  }

  ip = *((uint32_t*) hep->h_addr_list[0]);

  delete[] temp;

  str = str.substr(index + 1);

  if (sscanf(str.c_str(), "%hu", &port) != 1) {
    stream.setstate(std::ios_base::badbit);
    return stream;
  }

  pid.id = id;
  pid.ip = ip;
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
