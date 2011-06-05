#include <sstream>
#include <string>

std::string
operator + (const std::string &s, int i)
{
  std::stringstream out;
  out << i;
  return s + out.str();
}
