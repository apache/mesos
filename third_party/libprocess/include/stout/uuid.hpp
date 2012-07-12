#ifndef __STOUT_UUID_HPP__
#define __STOUT_UUID_HPP__

#include <assert.h>

#include <sstream>
#include <string>

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <boost/uuid/uuid_generators.hpp>

struct UUID : boost::uuids::uuid
{
public:
  static UUID random()
  {
    return UUID(boost::uuids::random_generator()());
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
