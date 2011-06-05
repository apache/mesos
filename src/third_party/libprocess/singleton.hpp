#ifndef SINGLETON_HPP
#define SINGLETON_HPP
 
template<class T>
class Singleton
{
private:
  static T *singleton;
  static bool instantiated;

public:
  static T * instance ()
  {
    static volatile bool instantiating = true;
    if (!instantiated) {
      if (__sync_bool_compare_and_swap(&instantiated, false, true)) {
	singleton = new T();
	instantiating = false;
      }
    }

    while (instantiating);

    return singleton;
  }
};
 
#endif // SINGLETON_HPP
