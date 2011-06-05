#include <iostream>
#include <map>
#include <string>

#include "foreach.hpp"

#define foreachpair BOOST_FOREACH_PAIR

using std::string;

int main(int argc, char **argv)
{
  std::map<int, string> m;
  m[0] = "zero";
  m[1] = "one";
  m[2] = "two";
  foreachpair (int key, string value, m) {
    std::cout << key << ": " << value << std::endl;
  }
  return 0;
}
