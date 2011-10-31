#include "zookeeper/authentication.hpp"

namespace zookeeper {

ACL _EVERYONE_READ_CREATOR_ALL_ACL[] = {
  { ZOO_PERM_READ, ZOO_ANYONE_ID_UNSAFE },
  { ZOO_PERM_ALL, ZOO_AUTH_IDS }
};


const ACL_vector EVERYONE_READ_CREATOR_ALL = {
    2, _EVERYONE_READ_CREATOR_ALL_ACL
};

} // namespace zookeeper {
