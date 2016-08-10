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

#ifndef __STOUT_OS_SYSCTL_HPP__
#define __STOUT_OS_SYSCTL_HPP__

// Only provide sysctl support for OS X and FreeBSD.
#if !defined(__APPLE__) && !defined(__FreeBSD__)
#error "stout/os/sysctl.hpp is only available on OS X and FreeBSD."
#endif

#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/types.h>

#include <string>
#include <vector>

#include <stout/error.hpp>
#include <stout/none.hpp>
#include <stout/option.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>

namespace os {

// Provides an abstraction for getting system information via the
// underlying 'sysctl' system call. You describe the sysctl
// "Management Information Base" (MIB) name via the constructor, for
// example, to describe "maximum number of processes allowed in the
// system" you would do:
//
//   os::sysctl(CTL_KERN, KERN_MAXPROC)
//
// To _retrieve_ the value you need to use one of the 'integer',
// 'string', 'table', or 'time' methods to indicate the type of the
// value being retrieved. For example:
//
//   Try<int> maxproc = os::sysctl(CTL_KERN, KERN_MAXPROC).integer();
//
// Note that the 'table' method requires specifying a length. If you
// would like the length to be looked up dynamically you can just pass
// None. Here's an example using 'table' that builds on above:
//
//   Try<vector<kinfo_proc>> processes =
//     os::sysctl(CTL_KERN, KERN_PROC, KERN_PROC_ALL).table(maxprox.get());
//
// TODO(benh): Provide an 'integer(i)', 'string(s)', and 'table(t)' to
// enable setting system information.
struct sysctl
{
  // Note that we create a constructor for each number of levels
  // because we can't pick a suitable default for unused levels (in
  // order to distinguish no value from some value) and while Option
  // would solve that it could also cause people to use None which
  // we'd need to later handle as an error.
  explicit sysctl(int level1);
  sysctl(int level1, int level2);
  sysctl(int level1, int level2, int level3);
  sysctl(int level1, int level2, int level3, int level4);
  sysctl(int level1, int level2, int level3, int level4, int level5);
  ~sysctl();

  // Get system information as an integer.
private: struct Integer; // Forward declaration.
public:
  Integer integer() const;

  // Get system information as a string.
  Try<std::string> string() const;

  // Get system information as a timeval.
  Try<timeval> time() const;

  // Get system information as a table, optionally specifying a
  // length. Note that this function is lazy and will not actually
  // perform the syscall until you cast (implicitly or explicitly) a
  // 'Table' to a std::vector<T>. For example, to get the first 10
  // processes in the process table you can do:
  //
  //     Try<std::vector<kinfo_proc>> processes =
  //       os::sysctl(CTL_KERN, KERN_PROC, KERN_PROC_ALL).table(10);
  //
private: struct Table; // Forward declaration.
public:
  Table table(const Option<size_t>& length = None()) const;

private:
  struct Integer
  {
    Integer(int _levels, int* _name);

    template <typename T>
    operator Try<T>();

    const int levels;
    int* name;
  };

  struct Table
  {
    Table(int _levels, int* _name, const Option<size_t>& _length);

    template <typename T>
    operator Try<std::vector<T>>();

    const int levels;
    int* name;
    Option<size_t> length;
  };

  const int levels;
  int* name;
};


inline sysctl::sysctl(int level1)
  : levels(1), name(new int[levels])
{
  name[0] = level1;
}


inline sysctl::sysctl(int level1, int level2)
  : levels(2), name(new int[levels])
{
  name[0] = level1;
  name[1] = level2;
}


inline sysctl::sysctl(int level1, int level2, int level3)
  : levels(3), name(new int[levels])
{
  name[0] = level1;
  name[1] = level2;
  name[2] = level3;
}


inline sysctl::sysctl(int level1, int level2, int level3, int level4)
  : levels(4), name(new int[levels])
{
  name[0] = level1;
  name[1] = level2;
  name[2] = level3;
  name[3] = level4;
}


inline sysctl::sysctl(
    int level1,
    int level2,
    int level3,
    int level4,
    int level5)
  : levels(5), name(new int[levels])
{
  name[0] = level1;
  name[1] = level2;
  name[2] = level3;
  name[3] = level4;
  name[4] = level5;
}


inline sysctl::~sysctl()
{
  delete[] name;
}


inline sysctl::Integer sysctl::integer() const
{
  return Integer(levels, name);
}


inline Try<std::string> sysctl::string() const
{
  // First determine the size of the string.
  size_t size = 0;
  if (::sysctl(name, levels, nullptr, &size, nullptr, 0) == -1) {
    return ErrnoError();
  }

  // Now read it.
  size_t length = size / sizeof(char);
  char* temp = new char[length];
  if (::sysctl(name, levels, temp, &size, nullptr, 0) == -1) {
    Error error = ErrnoError();
    delete[] temp;
    return error;
  }

  // TODO(benh): It's possible that the value has changed since we
  // determined it's length above. We should really check that we
  // get back the same length and if not throw an error.

  // The "string" in 'temp' might include null bytes, so to get all of
  // the data we need to create a string with 'size' (but we exclude
  // the last null byte via 'size - 1').
  std::string result(temp, size - 1);
  delete[] temp;
  return result;
}


inline Try<timeval> sysctl::time() const
{
  timeval result;
  size_t size = sizeof(result);
  if (::sysctl(name, levels, &result, &size, nullptr, 0) == -1) {
    return ErrnoError();
  }
  return result;
}


inline sysctl::Table sysctl::table(const Option<size_t>& length) const
{
  return Table(levels, name, length);
}


inline sysctl::Integer::Integer(
    int _levels,
    int* _name)
  : levels(_levels),
    name(_name)
{}


template <typename T>
sysctl::Integer::operator Try<T>()
{
  T i;
  size_t size = sizeof(i);
  if (::sysctl(name, levels, &i, &size, nullptr, 0) == -1) {
    return ErrnoError();
  }
  return i;
}


inline sysctl::Table::Table(
    int _levels,
    int* _name,
    const Option<size_t>& _length)
  : levels(_levels),
    name(_name),
    length(_length)
{}


template <typename T>
sysctl::Table::operator Try<std::vector<T>>()
{
  size_t size = 0;
  if (length.isNone()) {
    if (::sysctl(name, levels, nullptr, &size, nullptr, 0) == -1) {
      return ErrnoError();
    }
    if (size % sizeof(T) != 0) {
      return Error("Failed to determine the length of result, "
                   "amount of available data is not a multiple "
                   "of the table type");
    }
    length = Option<size_t>(size / sizeof(T));
  }

  T* ts = new T[length.get()];
  size = length.get() * sizeof(T);
  if (::sysctl(name, levels, ts, &size, nullptr, 0) == -1) {
    Error error = ErrnoError();
    delete[] ts;
    return error;
  }

  // TODO(benh): It's possible that the value has changed since we
  // determined it's length above (or from what was specified). We
  // should really check that we get back the same length and if not
  // throw an error.

  length = size / sizeof(T);

  std::vector<T> results;
  for (size_t i = 0; i < length.get(); i++) {
    results.push_back(ts[i]);
  }
  delete[] ts;
  return results;
}

} // namespace os {

#endif // __STOUT_OS_SYSCTL_HPP__
