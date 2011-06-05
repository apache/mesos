#include <assert.h>

#include "fatal.hpp"
#include "foreach.hpp"
#include "reliable.hpp"

using std::map;

#define malloc(bytes)                                               \
  ({ void *tmp;                                                     \
     if ((tmp = malloc(bytes)) == NULL)                             \
       fatalerror("malloc"); tmp;                                   \
   })

#define realloc(address, bytes)                                     \
  ({ void *tmp;                                                     \
     if ((tmp = realloc(address, bytes)) == NULL)                   \
       fatalerror("realloc"); tmp;                                  \
   })


/*
 * TODO(benh): Don't send structs around, this is horribly
 * scary. Instead, either use what ever hotness we get from Avro or
 * ProtocolBuffers, or something of the sort.
 */
struct rmsg
{
  int seq;
  struct msg msg;
};


class ReliableSender : public Process
{
public:
  struct rmsg *rmsg;

  ReliableSender(struct rmsg *_rmsg)
    : rmsg(_rmsg) {}

  ~ReliableSender()
  {
    if (rmsg != NULL) {
      free(rmsg);
      rmsg = NULL;
    }
  }

protected:
  void operator () ()
  {
    do {
      send(rmsg->msg.to, RELIABLE_MSG, (char *) rmsg,
	   sizeof(struct rmsg) + rmsg->msg.len);

      switch (receive(RELIABLE_TIMEOUT)) {
	case RELIABLE_ACK: {
	  // All done!
	  return;
	}
        case RELIABLE_REDIRECT: {
	  rmsg->msg.to = *reinterpret_cast<const PID *>(body(NULL));
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


ReliableProcess::ReliableProcess()
  : current(NULL), nextSeq(0) {}


ReliableProcess::~ReliableProcess()
{
  if (current != NULL) {
    free(current);
    current = NULL;
  }

  foreachpair (const PID &pid, ReliableSender *sender, senders) {
    assert(pid == sender->getPID());
    // Shut it down by sending it an ack.
    send(pid, RELIABLE_ACK);
    wait(pid);
    delete sender;
  }
}


int ReliableProcess::seq() const
{
  if (current != NULL)
    return current->seq;

  return -1;
}


bool ReliableProcess::duplicate() const
{
  // TODO(benh): Since we ignore out-of-order messages right now, a
  // duplicate message is just one whose sequence identifier is
  // greater than the last one we saw. Note that we don't add the
  // sequence identifier for the current message until the next
  // 'receive' invocation (see below).
  if (current != NULL) {
    map<PID, int>::const_iterator it = recvSeqs.find(current->msg.from);
    if (it != recvSeqs.end()) {
      int last = it->second;
      if (current->seq <= last)
	return true;
    }
  }

  return false;
}


PID ReliableProcess::origin() const
{
  if (current != NULL)
    return current->msg.from;

  return PID();
}


void ReliableProcess::ack()
{
  if (current != NULL)
    send(current->msg.from, RELIABLE_ACK, (char *) current,
	 sizeof(struct rmsg) + current->msg.len);
}


bool ReliableProcess::forward(const PID &to)
{
  if (current != NULL) {
    send(to, RELIABLE_MSG, (char *) current,
	 sizeof(struct rmsg) + current->msg.len);
    return true;
  }

  return false;
}


int ReliableProcess::rsend(const PID &to, MSGID id, const char *data, size_t length)
{
  // Allocate/Initialize outgoing message.
  struct rmsg *rmsg = (struct rmsg *) malloc(sizeof(struct rmsg) + length);

  int seq = nextSeq++;

  rmsg->seq = seq;

  rmsg->msg.from.pipe = self().pipe;
  rmsg->msg.from.ip = self().ip;
  rmsg->msg.from.port = self().port;
  rmsg->msg.to.pipe = to.pipe;
  rmsg->msg.to.ip = to.ip;
  rmsg->msg.to.port = to.port;
  rmsg->msg.id = id;
  rmsg->msg.len = length;

  if (length > 0)
    memcpy((char *) rmsg + sizeof(struct rmsg), data, length);

  ReliableSender *sender = new ReliableSender(rmsg);
  PID pid = link(spawn(sender));
  senders[pid] = sender;

  return seq;
}


MSGID ReliableProcess::receive(double secs)
{
  // Record sequence number for current (now old) _reliable_ message
  // and also free the message.
  if (current != NULL) {
    // TODO(benh): Since we ignore out-of-order messages right now, we
    // can be sure that the current message is the next in the
    // sequence (unless it's the first message or a duplicate).
    if (!duplicate()) {
      assert((recvSeqs.find(current->msg.from) == recvSeqs.end()) ||
	     (recvSeqs[current->msg.from] + 1 == current->seq));
      recvSeqs[current->msg.from] = current->seq;
    }
    free(current);
    current = NULL;
  }

  do {
    MSGID id = Process::receive(secs);
    switch (id) {
      // TODO(benh): Better validation of messages!
      case RELIABLE_ACK: {
	size_t length;
	const char *data = body(&length);
	assert(length > 0);
	struct rmsg *rmsg = (struct rmsg *) data;

	// TODO(benh): Is this really the way we want to do acks?
	foreachpair (const PID &pid, ReliableSender *sender, senders) {
	  assert(pid == sender->getPID());
	  // TODO(benh): Don't look into sender's class like this ... HACK!
	  if (rmsg->seq == sender->rmsg->seq &&
	      rmsg->msg.to == sender->rmsg->msg.to) {
	    send(pid, RELIABLE_ACK);
	  }
	}
	continue;
      }
      case RELIABLE_MSG: {
	size_t length;
	const char *data = body(&length);
	assert(length > 0);
	current = (struct rmsg *) malloc(length);
	memcpy((char *) current, data, length);

	// TODO(benh): Don't ignore out-of-order messages!
	if (recvSeqs.find(current->msg.from) != recvSeqs.end())
	  if (recvSeqs[current->msg.from] + 1 < current->seq)
	    continue;

	// Note that we don't record the sequence number here so that
	// our logic in 'duplicate' (see above) is correct. We might
	// want to consider a more complicated mechanism for
	// determining duplicates.

	inject(current->msg.from, current->msg.id,
	       data + sizeof(struct rmsg), current->msg.len);

	// Avoid recursively invoking ourselves via receive(), use receive(0)!
	return Process::receive(0);
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


void ReliableProcess::redirect(const PID &existing, const PID &updated)
{
  // Send a redirect to all running senders and update internal mapping.
  foreachpair (const PID &pid, ReliableSender *sender, senders) {
    assert(pid == sender->getPID());
    // TODO(benh): Don't look into sender's class like this ... HACK!
    if (existing == sender->rmsg->msg.to)
      send(pid, RELIABLE_REDIRECT, (char *) &updated, sizeof(PID));
  }
}


void ReliableProcess::cancel(int seq)
{
  foreachpair (const PID &pid, ReliableSender *sender, senders) {
    assert(pid == sender->getPID());
    // Shut it down by sending it an ack. It will get cleaned up via
    // the PROCESS_EXIT above.
    // TODO(benh): Don't look into sender's class like this ... HACK!
    if (seq == sender->rmsg->seq) {
      send(pid, RELIABLE_ACK);
      break;
    }
  }
}
