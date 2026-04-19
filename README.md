# Pascal compiler

This is a small compiler for the Pascal language, written in C++.

The aim is to bootstrap fpc 0.99.14 and fpc 1.0.6 with it.

Note that those do not support amd64.
They do support the following targets:
* i386
* m68k
* alpha
* powerpc

Build the translated compiler with:

`./build-fpc.sh`

Bootstrap the FPC 1.0.6 compiler sources under `../fpc-1.0.6/source` with:

`./bootstrap-fpc.sh`

Compile and run the checked-in example programs with:

`./tools/run_examples.sh`

Compile and run a couple of real `rpm` utilities as smoke tests with:

`./tools/run_rpm_smoke.sh`

For license see file COPYING in this directory.
