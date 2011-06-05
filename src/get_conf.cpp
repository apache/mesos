#include <iostream>
#include "configurator.hpp"

using std::cout;
using namespace mesos::internal;

int main(int argc, char **argv) {
  if (argc == 1) {
    cout << "Usage:\nget_conf OPTION [arg1 arg2 ... ]\n";
    cout << "OPTION\t\tis the name option whose value is to be extracted\n";
    cout << "arg1 ...\tOptional arguments passed to mesos, e.g. --quiet\n\n";
    return 1;
  }
  char **argv2 = &argv[1];
  Configurator conf;
  conf.load(argc-1, argv2);
  string param(argv[1]);
  transform(param.begin(), param.end(), param.begin(), tolower);
  if (conf.getParams().contains(param)) {
    cout << conf.getParams()[param] << "\n";
  }
  return 0;
}
