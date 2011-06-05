#ifndef __UTILS_HPP__
#define __UTILS_HPP__

#include <unistd.h>


// Useful common macros.
#define VA_NUM_ARGS_IMPL(_1, _2, _3, _4, _5, N, ...) N
#define VA_NUM_ARGS(...) VA_NUM_ARGS_IMPL(__VA_ARGS__, 5, 4, 3, 2, 1)

#define CONCAT_IMPL(A, B) A ## B
#define CONCAT(A, B) CONCAT_IMPL(A, B)


namespace mesos { namespace internal { namespace utils {

inline std::string getcwd()
{
  size_t size = 100;
     
  while (true) {
    char* temp = new char[size];
    if (::getcwd(temp, size) == temp) {
      std::string result(temp);
      delete[] temp;
      return result;
    } else {
      delete[] temp;
      if (errno != ERANGE) {
        return std::string();
      }
      size *= 2;
    }
  }

  return std::string();
}

}}} // namespace mesos { namespace internal { namespace utils {

#endif // __UTILS_HPP__
