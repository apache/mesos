# Mesos CLI

## Prerequisites

Make sure you have the following prerequisites
installed on your system before you begin.

```
python 2.6 or 2.7
virtualenv
```

## Getting Started

Once you you have the prerequisites installed, simply
run the `bootstrap` script from this directory to set
up a python virtual environment and start running the tool.

```
$ ./bootstrap

...

Setup complete!

To begin working, simply activate your virtual environment,
run the CLI, and then deactivate the virtual environment
when you are done.

    $ source activate
    $ mesos <command> [<args>...]
    $ source deactivate
```

**NOTE:** The virtual environment will also setup bash
autocomplete for all `mesos` commands.


## Running tests

To run the unit tests developed for the Mesos CLI, use
`mesos-cli-tests`:

```
$ ./bootstrap

...

Setup complete!

To begin working, simply activate your virtual environment,
run the CLI, and then deactivate the virtual environment
when you are done.

    $ source activate
    $ mesos-cli-tests

Running the Mesos CLI unit tests

...

OK
```
