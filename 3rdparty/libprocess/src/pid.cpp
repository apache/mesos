// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License

#ifndef __WINDOWS__
#include <arpa/inet.h>
#include <netdb.h>
#endif // __WINDOWS__

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <glog/logging.h>

#include <iostream>
#include <sstream>
#include <string>

#include <process/pid.hpp>
#include <process/process.hpp>

#include <stout/net.hpp>
#include <stout/os.hpp>

#include "config.hpp"

using std::ios_base;
using std::istream;
using std::istringstream;
using std::ostream;
using std::ostringstream;
using std::string;

namespace process {

UPID::UPID(const char* s)
{
  istringstream in(s);
  in >> *this;
}


UPID::UPID(const string& s)
{
  istringstream in(s);
  in >> *this;
}


// TODO(benh): Make this inline-able (cyclic dependency issues).
UPID::UPID(const ProcessBase& process)
{
  id = process.self().id;
  address = process.self().address;
}


UPID::operator string() const
{
  ostringstream out;
  out << *this;
  return out.str();
}


ostream& operator<<(ostream& stream, const UPID& pid)
{
  stream << pid.id << "@" << pid.address;
  return stream;
}


istream& operator>>(istream& stream, UPID& pid)
{
  pid.id = "";
  pid.address.ip = net::IP(INADDR_ANY);
  pid.address.port = 0;

  string str;
  if (!(stream >> str)) {
    stream.setstate(ios_base::badbit);
    return stream;
  }

  VLOG(3) << "Attempting to parse '" << str << "' into a PID";

  if (str.size() == 0) {
    stream.setstate(ios_base::badbit);
    return stream;
  }

  string id;
  string host;
  network::inet::Address address = network::inet::Address::ANY_ANY();

  size_t index = str.find('@');

  if (index != string::npos) {
    id = str.substr(0, index);
  } else {
    stream.setstate(ios_base::badbit);
    return stream;
  }

  str = str.substr(index + 1);

  index = str.find(':');

  if (index != string::npos) {
    host = str.substr(0, index);
  } else {
    stream.setstate(ios_base::badbit);
    return stream;
  }

  // TODO(evelinad): Extend this to support IPv6.
  Try<net::IP> ip = net::getIP(host, AF_INET);

  if (ip.isError()) {
    VLOG(2) << ip.error();
    stream.setstate(ios_base::badbit);
    return stream;
  }

  address.ip = ip.get();

  str = str.substr(index + 1);

  if (sscanf(str.c_str(), "%hu", &address.port) != 1) {
    stream.setstate(ios_base::badbit);
    return stream;
  }

  pid.id = id;
  pid.address = address;

  return stream;
}

} // namespace process {
