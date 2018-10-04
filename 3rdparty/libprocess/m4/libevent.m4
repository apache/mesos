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

#
# SYNOPSIS
#
#   MESOS_HAVE_LIBEVENT()
#
# DESCRIPTION
#
#   Validates if we have a usable libevent.
#

#
# SYNOPSIS
#
#   MESOS_HAVE_LIBEVENT_SSL()
#
# DESCRIPTION
#
#   Validates if we have a usable libevent_openssl.
#

AC_DEFUN([MESOS_HAVE_LIBEVENT],[
  AC_CHECK_HEADERS([event2/event.h], [], [
    AC_MSG_ERROR([cannot find libevent headers
-------------------------------------------------------------------
libevent version 2+ headers are required for libprocess to build.
-------------------------------------------------------------------
  ])])

  AC_CHECK_LIB([event_core], [event_base_new], [], [
    AC_MSG_ERROR([cannot find libevent
-------------------------------------------------------------------
libevent_core version 2+ is required for libprocess to build.
-------------------------------------------------------------------
  ])])

  AC_CHECK_HEADERS([event2/thread.h], [], [
    AC_MSG_ERROR([cannot find libevent_pthreads headers
-------------------------------------------------------------------
libevent_pthreads version 2+ headers are required for libprocess to build.
-------------------------------------------------------------------
  ])])

  AC_CHECK_LIB([event_pthreads], [evthread_use_pthreads], [], [
    AC_MSG_ERROR([cannot find libevent_pthreads
-------------------------------------------------------------------
libevent_pthreads version 2+ is required for libprocess to build.
-------------------------------------------------------------------
  ])])
])

AC_DEFUN([MESOS_HAVE_LIBEVENT_SSL],[
  AC_CHECK_HEADERS([event2/bufferevent_ssl.h],
                   [AC_CHECK_LIB([event_openssl],
                                 [bufferevent_openssl_get_ssl],
                                 [],
                                 [AC_MSG_ERROR([cannot find libevent_openssl
-------------------------------------------------------------------
libevent_openssl version 2+ is required for an SSL-enabled build.
-------------------------------------------------------------------
                                 ])])],
                   [AC_MSG_ERROR([cannot find libevent_openssl headers
-------------------------------------------------------------------
libevent_openssl version 2+ headers are required for an SSL-enabled build.
-------------------------------------------------------------------
  ])])
])
