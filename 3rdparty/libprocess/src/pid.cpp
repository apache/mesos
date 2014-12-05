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

#include <stout/net.hpp>
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
  node = process.self().node;
}


UPID::operator std::string() const
{
  std::ostringstream out;
  out << *this;
  return out.str();
}


ostream& operator << (ostream& stream, const UPID& pid)
{
  stream << pid.id << "@" << pid.node;
  return stream;
}


istream& operator >> (istream& stream, UPID& pid)
{
  pid.id = "";
  pid.node.ip = 0;
  pid.node.port = 0;

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
  Node node;

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

  //TODO(evelinad): Extend this to support IPv6
  Try<uint32_t> ip = net::getIP(host, AF_INET);

  if (ip.isError()) {
    VLOG(2) << ip.error();
    stream.setstate(std::ios_base::badbit);
    return stream;
  }

  node.ip = ip.get();

  str = str.substr(index + 1);

  if (sscanf(str.c_str(), "%hu", &node.port) != 1) {
    stream.setstate(std::ios_base::badbit);
    return stream;
  }

  pid.id = id;
  pid.node = node;

  return stream;
}


size_t hash_value(const UPID& pid)
{
  size_t seed = 0;
  boost::hash_combine(seed, pid.id);
  boost::hash_combine(seed, pid.node.ip);
  boost::hash_combine(seed, pid.node.port);
  return seed;
}

} // namespace process {
