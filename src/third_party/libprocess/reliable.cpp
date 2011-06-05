#include "reliable.hpp"

#define TIMEOUT 10


class ReliableSender : public Process
{
public:
  int seq;
  struct msg *msg;
  PID via;

  ReliableSender(int _seq, struct msg *_msg, const PID& _via = PID())
    : seq(_seq), msg(_msg), via(_via) {}

protected:
  void operator () ()
  {
    do {
      if (via == PID()) {
	fatal("unimplemented");
	//string data = pack<RELIABLE_MSG>(seq, msg);
	send(msg->to, RELIABLE_MSG, data.data(), data.size());
      } else {
	fatal("unimplemented");
	//string data = pack<RELIABLE_RELAY>(seq, msg);
	send(via, RELIABLE_RELAY, data.data(), data.size());
      }

      switch (receive(TIMEOUT)) {
	case RELIABLE_ACK: {
	  // All done!
	  return;
	}
        case RELIABLE_REDIRECT: {
	  const PID &pid = *static_cast<const PID *>(body());
	  msg->to = pid;
	  break;
	}
        case PROCESS_TIMEOUT: {
	  // Retry!
	  break;
	}
      }
    } while (true);
  }
};


ReliableProcess::ReliableProcess() : seq(0) {}


ReliableProcess::~ReliableProcess() {}


void ReliableProcess::redirect(const PID &old, const PID &cur)
{
  // Send a redirect to all running senders and update internal mapping.
  foreachpair (const PID &pid, ReliableSender *sender, senders) {
    // TODO(benh): Don't look into sender's class like this ... HACK!
    if (old == sender->msg->to) {
      assert(pid, sender->getPID());
      send(pid, RELIABLE_REDIRECT, &cur, sizeof(PID));
    }
  }
}


void ReliableProcess::rsend(const PID &to, MSGID id, const char *data, size_t length)
{
  // Allocate/Initialize outgoing message.
  struct msg *msg = (struct msg *) malloc(sizeof(struct msg) + length);

  msg->from.pipe = self().pipe;
  msg->from.ip = self().ip;
  msg->from.port = self().port;
  msg->to.pipe = to.pipe;
  msg->to.ip = to.ip;
  msg->to.port = to.port;
  msg->id = id;
  msg->len = length;

  if (length > 0)
    memcpy((char *) msg + sizeof(struct msg), data, length);

  ReliableSender *sender = new ReliableSender(seq++, msg);
  PID pid = link(spawn(sender));
  senders[pid] = sender;
}


void ReliableProcess::relay(const PID &via, const PID &to, MSGID id, const char *data, size_t length)
{
  // Allocate/Initialize outgoing message.
  struct msg *msg = (struct msg *) malloc(sizeof(struct msg) + length);

  msg->from.pipe = self().pipe;
  msg->from.ip = self().ip;
  msg->from.port = self().port;
  msg->to.pipe = to.pipe;
  msg->to.ip = to.ip;
  msg->to.port = to.port;
  msg->id = id;
  msg->len = length;

  if (length > 0)
    memcpy((char *) msg + sizeof(struct msg), data, length);

  ReliableSender *sender = new ReliableSender(seq++, msg, via);
  PID pid = link(spawn(sender));
  senders[pid] = sender;
}


MSGID ReliableProcess::receive(time_t secs)
{
  do {
    MSGID id = Process::receive(secs);
    switch (id) {
      case RELIABLE_MSG: {
	send(from(), RELIABLE_ACK);
	const struct msg *msg = static_cast<const struct msg *>(body());
	inject(msg->from, msg->id, body() + sizeof(struct msg), msg->len);
	return Process::receive();
      }
      case RELIABLE_RELAY: {
	const struct msg *msg = static_cast<const struct msg *>(body());
	inject(msg->from, msg->id, body() + sizeof(struct msg), msg->len);
	return Process::receive();
      }
      case PROCESS_EXIT: {
	if (senders.find(from()) != senders.end()) {
	  ReliableSender *sender = senders[from()];
	  senders.erase(from());
	  delete sender;
	  continue;
	}
	break;
      }
    }
    return id;
  } while (true);
}
