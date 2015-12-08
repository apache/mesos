# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License

# Makefile for stout.

AUTOMAKE_OPTIONS = foreign

# NOTE: The stout headers were moved to the Makefile in "include"
# subdirectory to make it easy to preserve the directory structure of
# the header files during installation.
SUBDIRS = . include

# Tests.
EXTRA_DIST =					\
  tests/base64_tests.cpp			\
  tests/bits_tests.cpp				\
  tests/bytes_tests.cpp				\
  tests/cache_tests.cpp				\
  tests/duration_tests.cpp			\
  tests/dynamiclibrary_tests.cpp		\
  tests/error_tests.cpp				\
  tests/flags_tests.cpp				\
  tests/gzip_tests.cpp				\
  tests/hashmap_tests.cpp			\
  tests/hashset_tests.cpp			\
  tests/interval_tests.cpp			\
  tests/ip_tests.cpp                            \
  tests/json_tests.cpp				\
  tests/jsonify_tests.cpp			\
  tests/linkedhashmap_tests.cpp			\
  tests/mac_tests.cpp                           \
  tests/main.cpp				\
  tests/multimap_tests.cpp			\
  tests/none_tests.cpp				\
  tests/option_tests.cpp			\
  tests/os_tests.cpp				\
  tests/os/sendfile_tests.cpp			\
  tests/os/signals_tests.cpp			\
  tests/path_tests.cpp				\
  tests/proc_tests.cpp				\
  tests/protobuf_tests.cpp			\
  tests/protobuf_tests.pb.cc			\
  tests/protobuf_tests.pb.h			\
  tests/protobuf_tests.proto			\
  tests/result_tests.cpp			\
  tests/set_tests.cpp				\
  tests/some_tests.cpp				\
  tests/strings_tests.cpp			\
  tests/subcommand_tests.cpp			\
  tests/svn_tests.cpp				\
  tests/uuid_tests.cpp				\
  tests/version_tests.cpp
