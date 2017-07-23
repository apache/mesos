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


## Setting up your configuration

In order to use this tool, you will need to create a
configuration file in your home directory under
`~/.mesos/config.toml`. A template for this config can be
seen below:

```
# The `plugins` is an array listing the absolute paths of the
# plugins you want to add to the CLI.
plugins = [
  "</absolute/path/to/plugin-1/directory>",
  "</absolute/path/to/plugin-2/directory>"
]
```

You can override the location of this configuration file using
the environment variable `MESOS_CLI_CONFIG`.
