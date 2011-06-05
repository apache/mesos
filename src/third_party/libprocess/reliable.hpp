#ifndef __RELIABLE_HPP__
#define __RELIABLE_HPP__

#include <process.hpp>

#include <map>


enum {
  RELIABLE_MSG = PROCESS_MSGID,
  RELIABLE_ACK,
  RELIABLE_REDIRECT,
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
   * @return origin of current message, or equal to @see Process::from().
   */
  virtual PID origin() const;

  /**
   * Acknowledges the current message by sending an 'ack' back to the
   * origin, or does nothing if the current message is not _reliable_.
   */
  virtual void ack();

  /**
   * Forward current message (provided it is _reliable_).
   * @param to destination
   * @return false if the current message is not _reliable_, true
   * otherwise.
   */
  virtual bool forward(const PID &to);

  /**
   * Sends a _reliable_ message to PID.
   * @param to destination
   * @param id message id
   */
  virtual void rsend(const PID &to, MSGID id);

  /**
   * Sends a _reliable_ message with data to PID.
   * @param to destination
   * @param id message id
   * @param data payload
   * @param length payload length
   */
  virtual void rsend(const PID &to, MSGID id, const char *data, size_t length);

  /* Blocks for message at most specified seconds. */
  virtual MSGID receive(double secs);

  /**
   * Redirect unacknolwedged messages to be sent to a different PID.
   * @param existing the current PID
   * @param updated the new PID
   */
  virtual void redirect(const PID &existing, const PID &updated);
  
private:
  struct rmsg *current;
  std::map<PID, int> sentSeqs;
  std::map<PID, int> recvSeqs;
  std::map<PID, ReliableSender *> senders;
};


inline void ReliableProcess::rsend(const PID &to, MSGID id)
{
  rsend(to, id, NULL, 0);
}


#endif /* __RELIABLE_HPP__ */
