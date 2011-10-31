#ifndef __SLAVE_HTTP_HPP__
#define __SLAVE_HTTP_HPP__

#include <process/future.hpp>
#include <process/http.hpp>


namespace mesos {
namespace internal {
namespace slave {

// Forward declaration (necessary to break circular dependency).
class Slave;

namespace http {

// Returns current vars in "key value\n" format (keys do not contain
// spaces, values may contain spaces but are ended by a newline).
process::Promise<process::HttpResponse> vars(
    const Slave& slave,
    const process::HttpRequest& request);


namespace json {

// Returns current statistics of the slave.
process::Promise<process::HttpResponse> stats(
    const Slave& slave,
    const process::HttpRequest& request);


// Returns current state of the cluster that the slave knows about.
process::Promise<process::HttpResponse> state(
    const Slave& slave,
    const process::HttpRequest& request);

} // namespace json {
} // namespace http {
} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __SLAVE_HTTP_HPP__
