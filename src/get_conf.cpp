#include <iostream>
#include "configurator.hpp"

using std::cout;
using namespace mesos::internal;

int main(int argc, char **argv) {
  if (argc == 1) {
    cout << "Usage:  mesos-getconf OPTION [arg1 arg2 ... ]\n\n";
    cout << "OPTION\t\tis the name option whose value is to be extracted\n";
    cout << "arg1 ...\tOptional arguments passed to mesos, e.g. --quiet\n\n";
    cout << "This utility gives shell scripts access Mesos parameters.\n\n";
    return 1;
  }
  Configurator conf;
  conf.load(argc, argv, true);
  string param(argv[1]);
  transform(param.begin(), param.end(), param.begin(), tolower);
  if (conf.getParams().contains(param)) {
    cout << conf.getParams()[param] << "\n";
  }
  return 0;
}
