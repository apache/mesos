#include <process.hpp>

/*
 * We would like to allow a single process to "speak" multiple
 * protocols.  Therefore, any additions to the base Process class
 * should be constructed as "mixins". In order to avoid using multiple
 * inheritance for mixins we require that all mixin classes are
 * written in a specific way. We demonstrate this below:
 *
 * First lets create a mixin that allows a process to "speak" Avro. We
 * use a fairly standard mixin model (i.e. create a subclass that
 * doesn't know about its parent!) to do this.
 */

struct AvroSchema { MSGID id; };
struct AvroMessage {};


template <typename P>
class Avro : public P
{
protected:
  void send(const PID &pid, const AvroSchema &schema, const AvroMessage &msg)
  {
    Process::send(pid, schema.id, std::make_pair("hello world", 12));
  }
};


/*
 * We would also like processes to be able "speak" tuple.
 */

#include <tuple.hpp>

using namespace process::tuple;

enum { MSG = PROCESS_MSGID };

namespace process { namespace tuple {

TUPLE(MSG, (int));

}}

/*
 * Now we can imagine an implementation of this that uses multiple
 * inheritance to accomplish the job:
 */


class FirstTupleAndAvroProcess : public Tuple<Process>, public Avro<Process>
{
protected:
  void operator () ()
  {
    PID pid;

    Tuple<Process>::send(pid, pack<MSG>(42));

    AvroSchema schema;
    AvroMessage msg;
    Avro<Process>::send(pid, schema, msg);
  }
};


/*
 * But this will be a non-starter for many people, because they won't
 * want to use multiple inheritance. Given the way we constructed our
 * mixins, we can avoid multiple inheritance and get (roughly) the
 * same result:
 */



class SecondTupleAndAvroProcess : public Tuple< Avro<Process> >
{
private:
  typedef Tuple< Avro<Process> > T;
  typedef Avro<Process> A;

protected:
  void operator () ()
  {
    PID pid;

    Tuple<Avro<Process> >::send(pid, Tuple< Avro<Process> >::pack<MSG>(42));
    T::send(pid, T::pack<MSG>(42));
    send(pid, pack<MSG>(42));

    AvroSchema schema;
    AvroMessage msg;
    Avro<Process>::send(pid, schema, msg);
    A::send(pid, schema, msg);
  }
};


/*
 * This isn't too bad. It lets you use the outermost mixin's
 * functionality without any extra syntax, and with a few typedef's
 * you can get at the other mixin's without too much trouble. The nice
 * thing, of course, about this is that we still don't have multiple
 * inheritance and yet Tuple and Avro were created completely
 * independent of one another. Okay, so we can clean this up a little
 * bit with a few more helpful templates.
 */


template <template <typename P> class Mixin0,
	  template <typename P> class Mixin1>
struct ProcessWithMixins : public Mixin0<Mixin1<Process> >
{
  typedef Mixin0<Mixin1<Process> > first;
  typedef Mixin1<Process> second;
};


class ThirdTupleAndAvroProcess : public ProcessWithMixins<Tuple, Avro>
{
private:
  typedef ProcessWithMixins<Tuple, Avro>::first T;
  typedef ProcessWithMixins<Tuple, Avro>::second A;

protected:
  void operator () ()
  {
    PID pid;

    T::send(pid, T::pack<MSG>(42));
    send(pid, pack<MSG>(42));

    AvroSchema schema;
    AvroMessage msg;
    A::send(pid, schema, msg);
  }
};


int main(int argc, char **argv)
{
  return 0;
}
