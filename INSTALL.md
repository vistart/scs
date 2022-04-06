# Install

Make the makefile for building and installing:

```shell
cmake .
```

Build and install the libraries:

```shell
make
[sudo] make install
```

## MKL

> Only supported by Intel Chips.

```shell
cmake -DMKLROOT=<path to the root of MKL> .
```

# Test
```shell
cmake -DBUILD_TESTING=on .
```
test the solver using MKL:
```shell
cmake -DBUILD_TESTING=on -DMKLROOT=<path to the root of MKL> .
```