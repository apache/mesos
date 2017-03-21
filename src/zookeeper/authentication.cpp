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
// limitations under the License

#include <mesos/zookeeper/authentication.hpp>

#ifdef __WINDOWS__
// NOTE: We need to redefine this macro, normally defined in
// zookeeper/src/c/include/winconfig.h.  Headers that include
// zookeeper.h typically need to undefine this macro to prevent
// it from bleeding into other sources.  However, this file
// needs to use the Zookeeper ACL struct, rather than Mesos ACLs.
#define ACL ZKACL
#endif // __WINDOWS__

namespace zookeeper {

ACL _EVERYONE_READ_CREATOR_ALL_ACL[] = {
  { ZOO_PERM_READ, ZOO_ANYONE_ID_UNSAFE },
  { ZOO_PERM_ALL, ZOO_AUTH_IDS }
};


const ACL_vector EVERYONE_READ_CREATOR_ALL = {
    2, _EVERYONE_READ_CREATOR_ALL_ACL
};


ACL _EVERYONE_CREATE_AND_READ_CREATOR_ALL_ACL[] = {
  { ZOO_PERM_CREATE, ZOO_ANYONE_ID_UNSAFE },
  { ZOO_PERM_READ, ZOO_ANYONE_ID_UNSAFE },
  { ZOO_PERM_ALL, ZOO_AUTH_IDS }
};


const ACL_vector EVERYONE_CREATE_AND_READ_CREATOR_ALL = {
    3, _EVERYONE_CREATE_AND_READ_CREATOR_ALL_ACL
};

} // namespace zookeeper {
