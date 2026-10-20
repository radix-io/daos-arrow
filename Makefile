ARROW=${HOME}/soft/arrow-cpp

CXX=g++
MPICXX=mpicxx
CXXFLAGS=-Wall -g --std=c++20

CPPFLAGS=$(shell pkg-config --cflags arrow parquet arrow-dataset arrow-compute)
LDLIBS=$(shell pkg-config --libs arrow parquet arrow-dataset arrow-compute)


parallel-write-with-mpi: parallel-write-with-mpi.cc
	$(MPICXX) $(CPPFLAGS) $(CXXFLAGS) -o $@  $< $(LDLIBS)
