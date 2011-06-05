#ifndef __RELIABLE_HPP__
#define __RELIABLE_HPP__

#include <process.hpp>


class ReliableSender;


class ReliableProcess : public Process
{
public:
  Reliable();
  ~Reliable();

protected:
  /**
   * Sequence number of current message.
   */
  virtual bool seq() const;

  /**
   * Whether or not current message has been seen before.
   */
  virtual bool duplicate() const;

  /**
   * Update PID 'old' to 'cur' so that unsent messages will be
   * redirected appropriately.
   * @param old the existing PID
   * @param cur the new PID
   */
  virtual void redirect(const PID &old, const PID &cur);

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

  /**
   * Relay a _reliable_ message. The intermediate destination does not
   * send an ack.
   * @param via intermediate destination
   * @param to destination
   * @param id message id
   */
  virtual void relay(const PID &via, const PID &to, MSGID id);

  /**
   * Relay a _reliable_ message with data. The intermediate
   * destination does not send an ack.
   * @param via intermediatedestination
   * @param to destination
   * @param id message id
   * @param data payload
   * @param length payload length
   */
  virtual void relay(const PID &via, const PID &to, MSGID id, const char *data, size_t length);

  /**
   * Forward the current _reliable_ message to PID. The destination will
   * send an ack to the sender.
   * @param to destination
   */
  virtual void forward(const PID &to);

  /* Blocks for message at most specified seconds. */
  virtual MSGID receive(time_t);
  
private:
  int seq;
  std::map<PID, ReliableSender *> senders;
};


inline void ReliableProcess::rsend(const PID &to, MSGID id)
{
  rsend(to, id, NULL, 0);
}


inline void ReliableProcess::relay(const PID &via, const PID &to, MSGID id)
{
  relay(via, to, id, NULL, 0);
}


#endif /* __RELIABLE_HPP__ */
