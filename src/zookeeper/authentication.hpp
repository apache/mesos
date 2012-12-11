#ifndef __ZOOKEEPER_AUTHENTICATION_HPP__
#define __ZOOKEEPER_AUTHENTICATION_HPP__

#include <zookeeper.h>

#include <string>

namespace zookeeper {

struct Authentication
{
  Authentication(
      const std::string& _scheme,
      const std::string& _credentials)
    : scheme(_scheme),
      credentials(_credentials) {}

  const std::string scheme;
  const std::string credentials;
};


// An ACL that ensures we're the only authenticated user to mutate our
// nodes - others are welcome to read.
extern const ACL_vector EVERYONE_READ_CREATOR_ALL;

// An ACL that allows others to create child nodes and read nodes, but
// we're the the only authenticated user to mutate our nodes.
extern const ACL_vector EVERYONE_CREATE_AND_READ_CREATOR_ALL;

} // namespace zookeeper {

#endif // __ZOOKEEPER_AUTHENTICATION_HPP__
