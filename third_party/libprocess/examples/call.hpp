class Func
{
public:
  virtual void func() = 0;
};

class MyFunc : public Func
{
public:
  void func() { return; }
};

extern Func *f;
extern MyFunc *myf;

extern void func();
