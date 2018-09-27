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
#   MESOS_HAVE_LIBEVENT(MINIMAL_VERSION, action-if-true, action-if-false)
#
# DESCRIPTION
#
#   Validates if we have a usable libevent.
#

AC_DEFUN([MESOS_HAVE_LIBEVENT],[
  AC_CHECK_HEADERS([event2/event.h], [], [
    AC_MSG_ERROR([cannot find libevent headers
-------------------------------------------------------------------
libevent version 2+ headers are required for libprocess to build.
-------------------------------------------------------------------
  ])])

  AC_CHECK_LIB([event], [event_base_new], [], [
    AC_MSG_ERROR([cannot find libevent
-------------------------------------------------------------------
libevent version 2+ is required for libprocess to build.
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

  AC_MSG_CHECKING([for libevent version])

  # Save our flags.
  saved_CFLAGS="$CFLAGS"
  saved_LIBS="$LIBS"

  # Required flags for libevent.
  LIBS="-levent"
  CFLAGS=""

  # Compile and run C program that gets the libevent version numnber into
  # "conftest.out" which gets cleaned up by AC_RUN_IFELSE itself.
  AC_LANG_PUSH([C])
  AC_RUN_IFELSE([
    AC_LANG_SOURCE([[
      #include <event.h>
      #include <stdio.h>

      int main(int argc, char** argv)
      {
        FILE* fp = fopen("conftest.out", "w");
        fprintf(fp, "%s", event_get_version());
        fclose(fp);
        return 0;
      }
    ]])],
    [libevent_version=`cat conftest.out`])
  AC_LANG_POP([C])

  # Restore flags.
  CFLAGS="$saved_CFLAGS"
  LIBS="$saved_LIBS"

  AC_MSG_RESULT([$libevent_version])

  AS_IF([test "x${libevent_version}" = "x"], [:], [
    AX_COMPARE_VERSION([$libevent_version],
                       [le], [$1],
                       [is_libevent_usable=yes])
  ])

  AS_IF([test "x$is_libevent_usable" = "xyes"], [
    $2
    :
  ], [
    $3
    :
  ])
])
