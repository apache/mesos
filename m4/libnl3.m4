# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

#
# SYNOPSIS
#
#   MESOS_MSG_LIBNL3_ERROR(message)
#
AC_DEFUN([MESOS_MSG_LIBNL3_ERROR], [

AC_MSG_ERROR([$1
-------------------------------------------------------------------
Please install libnl3 (version 3.2.26 or higher):
https://github.com/thom311/libnl/releases
-------------------------------------------------------------------
])

])

#
# SYNOPSIS
#
#   MESOS_HAVE_LIBNL3(action-if-true, action-if-false)
#
AC_DEFUN([MESOS_HAVE_LIBNL3], [

AC_ARG_WITH([nl],
  AS_HELP_STRING([--with-nl=@<:@DIR@:>@],
                 [specify where to locate the libnl3 library [default: /usr]])
)

# Make sure we only manipulate the compiler flags the first time
# this macro is used.
AS_IF([test x$ac_mesos_have_libnl3 = x], [
  AS_IF([test -n "$with_nl"], [
    CPPFLAGS="-I${with_nl}/include/libnl3 $CPPFLAGS"
    LDFLAGS="-L${with_nl}/lib $LDFLAGS"
  ], [
    CPPFLAGS="-I/usr/include/libnl3 $CPPFLAGS"
  ])
])

AC_CHECK_HEADERS(
  [netlink/netlink.h netlink/route/link/veth.h],
  [], [ac_mesos_have_libnl3=no]
)

# Check for libnl (both headers and libraries).
AC_CHECK_LIB(
  [nl-3], [nl_has_capability], [],
  [ac_mesos_have_libnl3=no]
)

# Check for libnl-route (both headers and libraries).
AC_CHECK_LIB(
  [nl-route-3], [rtnl_u32_get_classid], [],
  [ac_mesos_have_libnl3=no]
)

# Check for libnl-idiag-3 (both headers and libraries).
AC_CHECK_LIB(
  [nl-idiag-3], [idiagnl_msg_alloc_cache], [],
  [ac_mesos_have_libnl3=no]
)

AS_IF([test x$ac_mesos_have_libnl3 = xno], [
  $2
], [
  ac_mesos_have_libnl3=yes
  $1
])

])
