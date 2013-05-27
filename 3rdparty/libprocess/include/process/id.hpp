#ifndef __PROCESS_ID_HPP__
#define __PROCESS_ID_HPP__

#include <string>

namespace process {
namespace ID {

// Returns 'prefix(N)' where N represents the number of instances
// where this prefix has been used to generate an ID.
std::string generate(const std::string& prefix = "");

} // namespace ID {
} // namespace process {

#endif // __PROCESS_ID_HPP__
