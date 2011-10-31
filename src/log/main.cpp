#include <iostream>
#include <list>
#include <string>

#include <process/dispatch.hpp>
#include <process/process.hpp>

#include "common/fatal.hpp"
#include "common/foreach.hpp"
#include "common/result.hpp"

#include "log/replica.hpp"

using namespace mesos;
using namespace mesos::internal;
using namespace mesos::internal::log;


int main(int argc, char** argv)
{
  if (argc < 2) {
    fatal("Usage: %s file <from> <to>", argv[0]);
  }

  std::string file = argv[1];

  process::initialize(true);

  Replica replica(file);

  uint64_t from, to;

  if (argc != 4) {
    process::Future<uint64_t> begin = replica.beginning();
    process::Future<uint64_t> end = replica.ending();

    begin.await();
    end.await();

    CHECK(begin.isReady());
    CHECK(end.isReady());

    from = begin.get();
    to = end.get();
  } else {
    from = atoi(argv[2]);
    to = atoi(argv[3]);
  }

  std::cout << std::endl << "Attempting to read the log from "
            << from << " to " << to << std::endl << std::endl;

  process::Future<std::list<Action> > actions = replica.read(from, to);

  actions.await();

  CHECK(!actions.isFailed()) << actions.failure();

  CHECK(actions.isReady());

  foreach (const Action& action, actions.get()) {
    std::cout << "----------------------------------------------" << std::endl;
    action.PrintDebugString();
  }

  return 0;
}
