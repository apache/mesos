#include <iostream>
#include <string>

#include "configurator.hpp"

using namespace mesos::internal;

using std::cout;
using std::string;


int main(int argc, char** argv)
{
  if (argc == 1) {
    cout << "Usage:  mesos-getconf OPTION [arg1 arg2 ... ]\n\n";
    cout << "OPTION\t\tis the name option whose value is to be extracted\n";
    cout << "arg1 ...\tOptional arguments passed to mesos, e.g. --quiet\n\n";
    cout << "This utility gives shell scripts access to Mesos parameters.\n\n";
    return 1;
  }

  Configurator configurator;
  configurator.load(argc, argv, true);

  string param(argv[1]);
  transform(param.begin(), param.end(), param.begin(), tolower);

  Configuration conf = configurator.getConfiguration();
  if (conf.contains(param)) {
    cout << conf[param] << "\n";
  }

  return 0;
}
