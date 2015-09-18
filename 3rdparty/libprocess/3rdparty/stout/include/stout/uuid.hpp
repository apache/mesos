/**
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#ifndef __STOUT_UUID_HPP__
#define __STOUT_UUID_HPP__

#include <assert.h>

#include <sstream>
#include <string>

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>

#include <stout/thread_local.hpp>

struct UUID : boost::uuids::uuid
{
public:
  static UUID random()
  {
    static THREAD_LOCAL boost::uuids::random_generator* generator = NULL;

    if (generator == NULL) {
      generator = new boost::uuids::random_generator();
    }

    return UUID((*generator)());
  }

  static UUID fromBytes(const std::string& s)
  {
    boost::uuids::uuid uuid;
    memcpy(&uuid, s.data(), s.size());
    return UUID(uuid);
  }

  static UUID fromString(const std::string& s)
  {
    boost::uuids::uuid uuid;
    std::istringstream in(s);
    in >> uuid;
    return UUID(uuid);
  }

  std::string toBytes() const
  {
    assert(sizeof(data) == size());
    return std::string(reinterpret_cast<const char*>(data), sizeof(data));
  }

  std::string toString() const
  {
    std::ostringstream out;
    out << *this;
    return out.str();
  }

private:
  explicit UUID(const boost::uuids::uuid& uuid)
    : boost::uuids::uuid(uuid) {}
};

#endif // __STOUT_UUID_HPP__
