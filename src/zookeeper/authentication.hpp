#ifndef __ZOOKEEPER_AUTHENTICATION_HPP__
#define __ZOOKEEPER_AUTHENTICATION_HPP__

#include <zookeeper.h>

#include <string>

namespace zookeeper {

struct Authentication
{
  std::string scheme;
  std::string credentials;
};


// An ACL that ensures we're the only authenticated user to mutate our
// nodes - others are welcome to read.
extern const ACL_vector EVERYONE_READ_CREATOR_ALL;

} // namespace zookeeper {

#endif // __ZOOKEEPER_AUTHENTICATION_HPP__
