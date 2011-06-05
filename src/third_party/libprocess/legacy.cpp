#include <process.hpp>

#include <iostream>
#include <string>

#include <tr1/functional>

using std::cout;
using std::endl;
using std::string;

using std::tr1::bind;

template <typename T>
typename std::tr1::result_of<std::tr1::function<T (void)>(void)>::type
invoke2(const std::tr1::function<T (void)> &thunk)
{
  cout << "non-void" << endl;
  const T &t = thunk();
  return t;
}

// void invoke2(const std::tr1::function<void (void)> &thunk)
// {
//   cout << "void" << endl;
//   thunk();
// }


void func(const string &first)
{
  cout << first;
}


string * func2(const string &first)
{
  cout << first;
  return new string("42");
}


class Test : public Process
{
protected:
  void operator () ()
  {
    string hello("hello");
    invoke(bind(func, hello));
    cout << " world " << endl;
    string * i = invoke2<string *>(bind(func2, hello));
    i = invoke2(bind(func2, hello));
    cout << *i << endl;
    invoke2(bind(func2, hello));
  }

public:
  Test() {}
};


int main(int argc, char **argv)
{
  Process::wait(Process::spawn(new Test()));
  return 0;
}


// Callback class                                                               
class Callback                                                                  
{                                                                               
public:                                                                         
  Callback() {}                                                                 
  virtual ~Callback() {}                                                        
  virtual void run() = 0;                                                       
};                                                                              
                                                                                
                                                                                
// A Callback that calls operator() on an object of type T; used for wrapping   
// objects returned by boost::bind to give them a common interface.             
template<typename T>                                                            
class CallbackImpl : public Callback                                            
{                                                                               
public:                                                                         
  T t;                                                                          
  CallbackImpl(const T &_t): t(_t) {}                                           
  virtual ~CallbackImpl() {}                                                    
  virtual void run() { t(); }                                                   
};                                                                              
                                                                                
                                                                                
// Utility function to make a new CallbackImpl without having to write down     
// its type (the compiler infers it from the parameter to the function).        
template<typename T>                                                            
Callback* makeCallback(const T& t)                                              
{                                                                               
  return new CallbackImpl<T>(t);                                                
}
