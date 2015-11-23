// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <ostream>
#include <string>

#include <mesos/uri/uri.hpp>

#include "uri/utils.hpp"

using std::ostream;
using std::string;

namespace mesos {

ostream& operator<<(ostream& stream, const URI& uri)
{
  stream << uri.scheme() << ":";

  // The 'authority' part.
  if (uri.has_host()) {
    stream << "//";

    if (uri.has_user()) {
      stream << uri.user();

      if (uri.has_password()) {
        stream << ":" << uri.password();
      }

      stream << "@";
    }

    stream << uri.host();

    if (uri.has_port()) {
      stream << ":" << uri.port();
    }
  }

  // The 'path' part.
  stream << uri.path();

  // The 'query' part.
  if (uri.has_query()) {
    stream << "?" << uri.query();
  }

  // The 'fragment' part.
  if (uri.has_fragment()) {
    stream << "#" << uri.fragment();
  }

  return stream;
}

namespace uri {

URI construct(
    const string& scheme,
    const string& path,
    const Option<string>& host,
    const Option<int>& port,
    const Option<string>& query,
    const Option<string>& fragment,
    const Option<string>& user,
    const Option<string>& password)
{
  URI uri;

  uri.set_scheme(scheme);
  uri.set_path(path);

  if (host.isSome()) { uri.set_host(host.get()); }
  if (port.isSome()) { uri.set_port(port.get()); }
  if (query.isSome()) { uri.set_query(query.get()); }
  if (fragment.isSome()) { uri.set_fragment(fragment.get()); }
  if (user.isSome()) { uri.set_user(user.get()); }
  if (password.isSome()) { uri.set_password(password.get()); }

  return uri;
}

} // namespace uri {
} // namespace mesos {
