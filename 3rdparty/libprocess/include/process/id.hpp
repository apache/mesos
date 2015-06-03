#ifndef __PROCESS_ID_HPP__
#define __PROCESS_ID_HPP__

/** @file */

#include <string>

namespace process {
namespace ID {

/**
 * Returns 'prefix(N)' where N represents the number of instances
 * where the same prefix (wrt. string value equality) has been used
 * to generate an ID.
 *
 * @param prefix The prefix to base the result.
 * @return An "ID" in the shape 'prefix(N)'.
 */
std::string generate(const std::string& prefix = "");

} // namespace ID {
} // namespace process {

#endif // __PROCESS_ID_HPP__
