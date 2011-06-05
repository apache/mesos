#include <assert.h>

#include <foreach.hpp>
#include <io.hpp>
#include <tuple.hpp>

#include <map>
#include <string>
#include <queue>
#include <vector>

using std::map;
using std::string;
using std::queue;
using std::vector;

using namespace process::tuple;


enum { OKAY = PROCESS_MSGID, ERROR, SIT, SEAT, LEAVE, PICKUP, PUTDOWN };

namespace process { namespace tuple {
TUPLE(::OKAY, ());
TUPLE(::ERROR, (int));
TUPLE(::SIT, ());
TUPLE(::SEAT, (int, PID, PID));
TUPLE(::LEAVE, ());
TUPLE(::PICKUP, ());
TUPLE(::PUTDOWN, ());
}}


class Utensil : public Tuple<Process>
{
private:
  int id;
  bool available;

protected:
  void operator () ()
  {
    Out::println("utensil %d is at %s", id, string(getPID()).c_str());

    queue<PID> waiters;
    PID diner;

    for (;;) {
      switch (receive()) {
      case PICKUP: {
	if (available) {
	  available = false;
	  diner = from();
	  send<OKAY>(diner);
	} else {
	  waiters.push(from());
	}
	break;
      }
      case PUTDOWN: {
	if (available || !(diner == from()))
	  break;
	send<OKAY>(diner);
	if (!waiters.empty()) {
	  diner = waiters.front();
	  waiters.pop();
	  send<OKAY>(diner);
	} else {
	  available = true;
	}
	break;
      }
      }
    }
  }

public:
  Utensil(int _id) : id(_id), available(true) {}
};


class Table : public Tuple<Process>
{
private:
  int capacity;
  queue<int> seats;
  map<PID, int> diners;
  map<int, PID> utensils;

protected:
  void operator () ()
  {
    Out::println("table is at %s", string(getPID()).c_str());

    // Create the seats and the utensils.
    for (int i = 0; i < capacity; i++) {
      seats.push(i);
      utensils[i] = Process::spawn(new Utensil(i));
    }

    for (;;) {
      switch (receive()) {
      case SIT: {
	if (seats.size() > 0) {
	  int seat = seats.front();
	  seats.pop();
	  diners[from()] = seat;
	  PID first, second;
	  if (seat < (seat + 1) % capacity) {
	    first = utensils[seat];
	    second = utensils[(seat + 1) % capacity];
	  } else {
	    first = utensils[(seat + 1) % capacity];
	    second = utensils[seat];
	  }
	  send<SEAT>(from(), seat, first, second);
	} else {
	  send<ERROR>(from(), 0);
	}
	break;
      }
      case LEAVE: {
	assert(diners.find(from()) != diners.end());
	seats.push(diners[from()]);
	diners.erase(from());
	send<OKAY>(from());
	break;
      }
      default:
	Out::println("table received unexpected message");
	break;
      }
    }
  }

public:
  Table(int _capacity) : capacity(_capacity) {}
};


class Philosopher : public Tuple<Process>
{
private:
  string name;
  Table *table;

protected:
  void operator () ()
  {
    Out::println("%s is at %s", name.c_str(), string(getPID()).c_str());

    send<SIT>(table->getPID());
    receive();
    assert(msgid() == SEAT);
    int seat;
    PID first;
    PID second;
    unpack<SEAT>(seat, first, second);

    Out::println("%s is sitting at %d", name.c_str(), seat);

    for (int i = 0; i < 5; i++) {
      Out::println("%s is hungry", name.c_str());
      send<PICKUP>(first);
      receive();
      assert(msgid() == OKAY);
      send<PICKUP>(second);
      receive();
      assert(msgid() == OKAY);
      Out::println("%s is eating", name.c_str());
      pause(1);//rand() % 3 + 1);
      send<PUTDOWN>(second);
      receive();
      assert(msgid() == OKAY);
      send<PUTDOWN>(first);
      receive();
      assert(msgid() == OKAY);
      Out::println("%s is thinking", name.c_str());
      pause(1);//rand() % 3 + 1);
    }
  }

public:
  Philosopher(const char *_name, Table *_table) :
    name(_name), table(_table) {}
};


int main(int argc, char **argv)
{
  // Create a table that can dine some philosophers.
  Table table(3);

  Process::spawn(&table);

  // Create the philosophers.
  vector<Philosopher *> philosophers(3);
  philosophers[0] = new Philosopher("Socrates", &table);
  philosophers[1] = new Philosopher("Plato", &table);
  philosophers[2] = new Philosopher("Diogenes", &table);

  foreach (Philosopher *philosopher, philosophers)
    Process::spawn(philosopher);

  foreach (Philosopher *philosopher, philosophers)
    Process::wait(philosopher);

  return 0;
}
