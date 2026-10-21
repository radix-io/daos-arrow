# DAOS for Arrow

The C++ interface to Arrow already has support for writing Parquet data out to
different formats:  e.g. plain files, Azure blobs, or HDF5.  This projects
extends Arrow to support DAOS natively.

The DAOS interception library probably handles Arrow/Parquet just fine.  A
DAOS-native implementation can be a little more efficient resolving datasets,
for example, and of course anything that bypasses the FUSE interface will see
performance benefits.

It only took a couple hours and about 30 bucks worth of API calls to get Claude
to implement this native approach.   The code extends `arrow::fs::FileSystem`
, `arrow::io::RandomAccessFile`, and `arrow::io::OutputStream` and adds a
"daos" entity to the arrow::fs::FileSystemRegistrar .

I started this work thinking I'd stick an S3 emulation layer on top of DAOS and
just use exising Arrow/Parquet infrastructure, which is why you might find a
few stray references to 's3'.  Working directly with DAOS turned out to be much
more straightforward.

## Requirements

* DAOS development headers
* Arrow built with at least Parquet, Filesystem, Compute, and Dataset features.  Your Arrow cmake invocation should contain these flags (plus whatever else you want Arrow to do)

    cmake .. -DARROW_PARQUET=ON -DARROW_FILESYSTEM=ON -DARROW_COMPUTE=ON -DARROW_DATASET=ON -D-DARROW_ACERO=ON [...]

## Building

Right now, just builds an object (.o) file extending a few Arrow classes: link
that object into your Arrow/Parquet code and now your code will understand URIs
like `daos://pool/container/foo`

## testing

All the tests assume you have a daos pool `radix-io` and created a container
`daos-arrow-test` in that pool.  Making tests take arbitrary pool / containers
is on the todo list.

## Running

This project comes with two demonstration codes in the `examples` directory:

* parallel-write-with-mpi.cc uses Parquet's "dataset" feature to show one way to write out one slug of data per process
* unified-reading.cc shows how Parquet re-assembles those multiple slugs

## A note about AI

The code in examples/ came from a human as he was learning how to write code
using Arrow and Parquet.  A human also wrote this README.  Just about all the
code in src/ and tests/ came from Claude sonnet-5 implementing a plan drawn up
by Opus-5.  Brave new world...
