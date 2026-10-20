ARROW=${HOME}/soft/arrow-cpp

CXX=g++
MPICXX=mpicxx
CXXFLAGS=-Wall -g --std=c++20

CPPFLAGS=$(shell pkg-config --cflags arrow parquet arrow-dataset arrow-compute)
LDLIBS=$(shell pkg-config --libs arrow parquet arrow-dataset arrow-compute)

# DAOS: no pkg-config file shipped, so hand-specify against system paths.
DAOS_LDLIBS=-ldfs -lduns -ldaos -luuid

SRC=src
TESTS=tests

INC=-I$(SRC)

all: parallel-write-with-mpi unified-reading
# NB: named daos_arrow_fs.{h,cc}, NOT daos_fs.{h,cc} -- the system DAOS
# headers already ship /usr/include/daos_fs.h, and with -I$(SRC) in the
# build, a local daos_fs.h would shadow it (confirmed: g++ -H showed the
# local stub satisfying #include <daos_fs.h> inside daos-cpp-dfs.h, which
# then fails to find any dfs_* declarations at all).
$(SRC)/daos_arrow_fs.o: $(SRC)/daos_arrow_fs.cc $(SRC)/daos_arrow_fs.h $(SRC)/daos-cpp-dfs.h
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c -o $@ $<

parallel-write-with-mpi: examples//parallel-write-with-mpi.cc $(SRC)/daos_arrow_fs.o
	$(MPICXX) $(CPPFLAGS) $(CXXFLAGS) $(INC) -o $@ $< $(SRC)/daos_arrow_fs.o $(LDLIBS) $(DAOS_LDLIBS)

unified-reading: examples/unified-reading.cc $(SRC)/daos_arrow_fs.o
	$(MPICXX) $(CPPFLAGS) $(CXXFLAGS) $(INC) -o $@ $< $(SRC)/daos_arrow_fs.o $(LDLIBS) $(DAOS_LDLIBS)

# only the parallel write demo uses MPI.  everything else can run without MPI
$(TESTS)/test-dfs-sanity: $(TESTS)/test-dfs-sanity.cc
	$(CXX) $(CXXFLAGS) $(INC) -o $@ $< $(DAOS_LDLIBS)

$(TESTS)/test-dfs-roundtrip: $(TESTS)/test-dfs-roundtrip.cc
	$(CXX) $(CXXFLAGS) $(INC) -o $@ $< $(DAOS_LDLIBS)

$(TESTS)/test-daos-filesystem: $(TESTS)/test-daos-filesystem.cc $(SRC)/daos_arrow_fs.o
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(INC) -o $@ $< $(SRC)/daos_arrow_fs.o $(LDLIBS) $(DAOS_LDLIBS)

$(TESTS)/test-daos-filesystem-recursive: $(TESTS)/test-daos-filesystem-recursive.cc $(SRC)/daos_arrow_fs.o
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(INC) -o $@ $< $(SRC)/daos_arrow_fs.o $(LDLIBS) $(DAOS_LDLIBS)

.PHONY: tests
tests: $(TESTS)/test-dfs-sanity $(TESTS)/test-dfs-roundtrip $(TESTS)/test-daos-filesystem $(TESTS)/test-daos-filesystem-recursive

.PHONY: clean
clean:
	rm -f $(SRC)/daos_arrow_fs.o parallel-write-with-mpi unified-reading \
	      $(TESTS)/test-dfs-sanity $(TESTS)/test-dfs-roundtrip \
	      $(TESTS)/test-daos-filesystem $(TESTS)/test-daos-filesystem-recursive
