#ifndef __TOKENIZE_HPP__
#define __TOKENIZE_HPP__

#include <map>
#include <stdexcept>
#include <string>
#include <vector>


namespace tokenize {

class TokenizeException : public std::runtime_error
{
public:
  TokenizeException(const std::string& what) : std::runtime_error(what) {}
};


/**
 * Utility function to tokenize a string based on some delimiters.
 */
std::vector<std::string> split(const std::string& s,
                               const std::string& delims);


std::map<std::string, std::vector<std::string> > pairs(const std::string& s,
                                                       char delim1, char delim2);

} // namespace tokenize {

#endif // __TOKENIZE_HPP__





