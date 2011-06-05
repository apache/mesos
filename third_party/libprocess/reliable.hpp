#ifndef __RELIABLE_HPP__
#define __RELIABLE_HPP__

#include <process.hpp>

#include <functional>
#include <map>

#define RELIABLE_TIMEOUT 10


enum {
  RELIABLE_MSG = PROCESS_MSGID,
  RELIABLE_ACK,
  RELIABLE_REDIRECT_VIA,
  RELIABLE_REDIRECT_TO,
  RELIABLE_MSGID
};


struct rmsg; 
class ReliableSender;


class ReliableProcess : public Process
{
public:
  ReliableProcess();
  virtual ~ReliableProcess();

protected:
  /**
   * @return sequence number of current _message, or -1 if current
   * message is not a _reliable_ message.
   */
  virtual int seq() const;

  /**
   * @return true if current message has been seen before, otherwise
   * false (because current message has not been seen before or is not
   * a _reliable_ message).
   */
  virtual bool duplicate() const;

  /**
   * @return origin of current message (if current message is not
   * reliable this returns Process::from()).
   */
  virtual PID origin() const;

  /**
   * @return destination of current message (if current message is not
   * reliable this returns Process::self()).
   */
  virtual PID destination() const;

  /**
   * Acknowledges the current message by sending an 'ack' back to the
   * origin, or does nothing if the current message is not _reliable_.
   */
  virtual void ack();

  /**
   * Forward current message (provided it is _reliable_).
   * @param via hop (or possibly destination)
   * @return false if the current message is not _reliable_, true
   * otherwise.
   */
  virtual bool forward(const PID &via);

  /**
   * Sends a _reliable_ message to PID.
   * @param to destination
   * @param id message id
   * @return sequence number of message
   */
  virtual int rsend(const PID &to, MSGID id);

  /**
   * Sends a _reliable_ message via another PID (meant to be
   * forwarded).
   * @param via hop
   * @param to destination
   * @param id message id
   * @return sequence number of message
   */
  virtual int rsend(const PID &via, const PID &to, MSGID id);

  /**
   * Sends a _reliable_ message with data to PID.
   * @param to destination
   * @param id message id
   * @param data payload
   * @param length payload length
   * @return sequence number of message
   */
  virtual int rsend(const PID &to, MSGID id, const char *data, size_t length);

  /**
   * Sends a _reliable_ message with data via another process (meant
   * to be forwarded).
   * @param via hop
   * @param to destination
   * @param id message id
   * @param data payload
   * @param length payload length
   * @return sequence number of message
   */
  virtual int rsend(const PID &via, const PID &to, MSGID id, const char *data, size_t length);


  /* Blocks for message indefinitely. */
  virtual MSGID receive();

  /* Blocks for message at most specified seconds. */
  virtual MSGID receive(double secs);

  /**
   * Redirect unacknolwedged messages to be sent to a different PID.
   * @param existing the current PID
   * @param updated the new PID
   */
  virtual void redirect(const PID &existing, const PID &updated);

  /**
   * Cancel trying to reliably send the message with the specified
   * sequence number.
   * @param seq sequence number of message to cancel
   */
  virtual void cancel(int seq);
  
private:
  struct rmsg *current;
  std::map<PID, int> sentSeqs;
  std::map<std::pair<PID, PID>, int> recvSeqs;
  std::map<PID, ReliableSender *> senders;
};


inline int ReliableProcess::rsend(const PID &to, MSGID id)
{
  return rsend(to, id, NULL, 0);
}


inline int ReliableProcess::rsend(const PID &via, const PID &to, MSGID id)
{
  return rsend(via, to, id, NULL, 0);
}


inline MSGID ReliableProcess::receive()
{
  return receive(0);
}

#endif /* __RELIABLE_HPP__ */
