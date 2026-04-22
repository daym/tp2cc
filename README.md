# Pascal compiler

This is a small compiler for the Pascal language, written in C++.

The aim is to bootstrap fpc 0.99.14 and fpc 1.0.6 with it.

Note that those do not support amd64.
They do support the following targets:
* i386
* m68k
* alpha
* powerpc

Bootstrap the FPC 0.99.14 compiler sources under `../fpc-1.0.6/source` with:

`./bootstrap-fpc-0.sh`

Or bootstrap the FPC 1.0.6 compiler sources under `../fpc-1.0.6/source` with:

`./bootstrap-fpc-1.sh`

Compile and run the checked-in example programs with:

`./tools/run_examples.sh`

Compile and run a couple of real `rpm` utilities as smoke tests with:

`./tools/run_rpm_smoke.sh`

For license see file COPYING in this directory.

## Limitations

Since we don't want to special-case all the things, non-packed records are stored in a C compatible way (alignment of each field is a natural multiple of its offset).
These kind of records basically didn't exist in Turbo Pascal (all records were packed). Free Pascal has a default alignment of 2 Byte for those.

When user is specifying "packed" records, we make a best effort to actually pack them.  If you see GCC warnings about packed being ignored you know where the limits are.

We use C++ references to model "var" parameters--so that means that those assume natural alignment for the type of the var parameter (!!!).  They CAN be untyped (since we added a special case to the compiler).

C++ casts actually cause undefined behavior if you cast objects with virtual method tables to unrelated types.  We don't work around this since we find it silly for Pascal to ignore the static type like this.  Use UBSAN to find spots where your Pascal program relies on this (at runtime).
