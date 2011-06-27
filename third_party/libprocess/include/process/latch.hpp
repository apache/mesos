#ifndef __PROCESS_LATCH_HPP__
#define __PROCESS_LATCH_HPP__


namespace process {

class LatchProcess;


class Latch
{
public:
  Latch();
  virtual ~Latch();

  void trigger();
  bool await(double secs = 0);

private:
  Latch(const Latch& that);
  Latch& operator = (const Latch& that);

  bool triggered;
  LatchProcess* latch;
};

}  // namespace process {

#endif // __PROCESS_LATCH_HPP__
