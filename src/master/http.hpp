#ifndef __MASTER_HTTP_HPP__
#define __MASTER_HTTP_HPP__

#include <process/future.hpp>
#include <process/http.hpp>

namespace mesos {
namespace internal {
namespace master {

// Forward declaration (necessary to break circular dependency).
class Master;

namespace http {

// Returns current vars in "key value\n" format (keys do not contain
// spaces, values may contain spaces but are ended by a newline).
process::Promise<process::HttpResponse> vars(
    const Master& master,
    const process::HttpRequest& request);


namespace json {

// Returns current statistics of the master.
process::Promise<process::HttpResponse> stats(
    const Master& master,
    const process::HttpRequest& request);


// Returns current state of the cluster that the master knows about.
process::Promise<process::HttpResponse> state(
    const Master& master,
    const process::HttpRequest& request);

} // namespace json {
} // namespace http {
} // namespace master {
} // namespace internal {
} // namespace mesos {

#endif // __MASTER_HTTP_HPP__
