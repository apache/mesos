#ifndef __TOKENIZE_HPP__
#define __TOKENIZE_HPP__

#include <string>
#include <vector>


namespace mesos { namespace internal {

/**
 * Utility function to tokenize a string based on some delimiters.
 */
std::vector<std::string> tokenize(const std::string& s, const std::string& delims);

}} /* namespace mesos { namespace internal { */

#endif // __TOKENIZE_HPP__





