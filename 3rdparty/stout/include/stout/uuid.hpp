// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//  http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef __STOUT_UUID_HPP__
#define __STOUT_UUID_HPP__

#include <assert.h>

#include <stdexcept>
#include <string>

#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/string_generator.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_io.hpp>

#include <stout/error.hpp>
#include <stout/try.hpp>

#ifdef __WINDOWS__
#include <stout/windows.hpp>
#endif // __WINDOWS__

namespace id {

struct UUID : boost::uuids::uuid
{
public:
  static UUID random()
  {
    static thread_local boost::uuids::random_generator* generator = nullptr;

    if (generator == nullptr) {
      generator = new boost::uuids::random_generator();
    }

    return UUID((*generator)());
  }

  static Try<UUID> fromBytes(const std::string& s)
  {
    const std::string error = "Not a valid UUID";

    if (s.size() != UUID::static_size()) {
      return Error(error);
    }

    boost::uuids::uuid uuid;
    memcpy(&uuid, s.data(), s.size());

    if (uuid.version() == UUID::version_unknown) {
      return Error(error);
    }

    return UUID(uuid);
  }

  static Try<UUID> fromString(const std::string& s)
  {
    try {
      // NOTE: We don't use `thread_local` for the `string_generator`
      // (unlike for the `random_generator` above), because it is cheap
      // to construct one each time.
      boost::uuids::string_generator gen;
      boost::uuids::uuid uuid = gen(s);
      return UUID(uuid);
    } catch (const std::runtime_error& e) {
      return Error(e.what());
    }
  }

  std::string toBytes() const
  {
    assert(sizeof(data) == size());
    return std::string(reinterpret_cast<const char*>(data), sizeof(data));
  }

  std::string toString() const
  {
    return to_string(*this);
  }

private:
  explicit UUID(const boost::uuids::uuid& uuid)
    : boost::uuids::uuid(uuid) {}
};

} // namespace id {

namespace std {

template <>
struct hash<id::UUID>
{
  typedef size_t result_type;

  typedef id::UUID argument_type;

  result_type operator()(const argument_type& uuid) const
  {
    return boost::uuids::hash_value(uuid);
  }
};

} // namespace std {

#endif // __STOUT_UUID_HPP__
