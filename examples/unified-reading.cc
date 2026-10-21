#include <arrow/api.h>
#include <arrow/io/api.h>
#include <parquet/arrow/writer.h>
#include <vector>
#include <string>
#include <iostream>
#include <arrow/filesystem/filesystem.h>
#include <arrow/filesystem/localfs.h>

#include <arrow/dataset/api.h>

#include <arrow/compute/api.h>

// Pulls in daos_arrow::DaosFileSystem and self-registers it with Arrow's
// FileSystem registry ("daos" scheme) as a side effect of linking it in --
// see the FileSystemRegistrar at the bottom of daos_arrow_fs.cc.
#include "daos_arrow_fs.h"

#include <mpi.h>

/*
 * Parquet can abstract multiple files under a "dataset" abstraction
 *
 * `prefix` is a daos://pool/container/[path] URI, matching what
 * parallel-write-with-mpi.cc wrote to. A bare local path (no
 * "daos://" scheme) falls back to LocalFileSystem via
 * FileSystemFromUriOrPath, so this can still be exercised without DAOS.
 */

arrow::Status readData(int rank, int nprocs, const std::string& prefix)
{
    // interesting:  will get an error about "no function registered with name:
    // make_struct" unless we initialize arrow::compute
    ARROW_RETURN_NOT_OK(arrow::compute::Initialize());

    std::string path;
    ARROW_ASSIGN_OR_RAISE(auto fs, arrow::fs::FileSystemFromUriOrPath(prefix, &path));

    auto format = std::make_shared<arrow::dataset::ParquetFileFormat>();

    arrow::dataset::FileSystemFactoryOptions options;
    // Could pass a single file like this as the selector parameter:  {"data/"}
    // but it won't recurse into a directory.  use fs::FileSelector instead
    arrow::fs::FileSelector selector;
    selector.base_dir = path.empty() ? "data" : path + "/data";
    selector.recursive = true;
    ARROW_ASSIGN_OR_RAISE(auto factory,
            arrow::dataset::FileSystemDatasetFactory::Make(fs, selector,
                format, options));

    // obtains a dataset object -- just the tools and methods to operate on
    // data; the data still lives on disk
    ARROW_ASSIGN_OR_RAISE(auto read_dataset, factory->Finish());

    std::cout << "Schema: \n" << read_dataset->schema()->ToString() << std::endl;

    ARROW_ASSIGN_OR_RAISE(auto scanner_builder, read_dataset->NewScan());
    ARROW_RETURN_NOT_OK(scanner_builder->Project({"ID", "Username"}));
    ARROW_ASSIGN_OR_RAISE(auto scanner, scanner_builder->Finish());
    ARROW_ASSIGN_OR_RAISE(auto table, scanner->ToTable());

    std::cout << "Read " << table->num_rows() << " rows from all shards." << std::endl;

    return arrow::Status::OK();

}

int main(int argc, char **argv)
{
    MPI_Init(&argc, &argv);
    int nprocs, rank;
    MPI_Comm_size(MPI_COMM_WORLD, &nprocs);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    auto status = readData(rank, nprocs, argv[1]);
    if (!status.ok()) {
        std::cerr << "Error: "<< status.ToString() << std::endl;
        MPI_Abort(MPI_COMM_WORLD, 1);
    }
    MPI_Finalize();
    return 0;
}
