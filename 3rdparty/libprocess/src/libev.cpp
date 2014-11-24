#include <ev.h>

#include <queue>

#include <stout/lambda.hpp>

#include "libev.hpp"

namespace process {

// Defines the initial values for all of the declarations made in
// libev.hpp (since these need to live in the static data space).
struct ev_loop* loop = NULL;

ev_async async_watcher;

std::queue<ev_io*>* watchers = new std::queue<ev_io*>();

synchronizable(watchers);

std::queue<lambda::function<void(void)>>* functions =
  new std::queue<lambda::function<void(void)>>();

ThreadLocal<bool>* _in_event_loop_ = new ThreadLocal<bool>();

} // namespace process {
