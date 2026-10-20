#include <arrow/api.h>
#include <arrow/io/api.h>
#include <parquet/arrow/writer.h>
#include <vector>
#include <string>
#include <iostream>
#include <arrow/filesystem/filesystem.h>
#include <arrow/filesystem/localfs.h>

#include <mpi.h>
/*
 * with RecordBatch writing in place we can tackle parallelism
 * consider an MPI job that writes its data to Parquet.  we can shard to one
 * file per MPI rank
 */

arrow::Status writeShard(int rank, std::shared_ptr<arrow::RecordBatch> batch, const std::string prefix) {

    std::string filename = prefix + "data/part-" + std::to_string(rank) + ".parquet";

    auto result = arrow::io::FileOutputStream::Open(filename);
    if (!result.ok() && result.status().IsIOError()) {
        // make directory and try again
        auto fs = std::make_shared<arrow::fs::LocalFileSystem>();
        ARROW_RETURN_NOT_OK(fs->CreateDir("data"));

        result = arrow::io::FileOutputStream::Open(filename);
    }
    auto outfile = std::move(result).ValueUnsafe();

    auto writer_props = parquet::WriterProperties::Builder().build();
    auto arrow_props =  parquet::ArrowWriterProperties::Builder().build();

    std::unique_ptr<parquet::arrow::FileWriter> writer;
    ARROW_ASSIGN_OR_RAISE(writer, parquet::arrow::FileWriter::Open(
                *batch->schema(), arrow::default_memory_pool(), outfile,
                writer_props, arrow_props));

    ARROW_RETURN_NOT_OK(writer->WriteRecordBatch(*batch));
    return writer->Close();
}
arrow::Status writeData(int rank, int nprocs, const std::string prefix) {
    // "builders":  consider id and name mappings

    arrow::Int64Builder id_builder;
    arrow::StringBuilder name_builder;

    // some data
    std::vector<int64_t> ids = {1, 2, 3, 4, 5, 6, 7, 8};
    std::vector<std::string> names = {"Gemini", "Aries", "Taurus",
        "Libra", "Scorpio", "Cancer",
        "Leo", "Sagittarius"};

    size_t start = rank*(ids.size()/nprocs);
    size_t count = ids.size()/nprocs;
    for (size_t i=start; (i-start)< count; i++) {
        ARROW_RETURN_NOT_OK(id_builder.Append(ids[i]));
        ARROW_RETURN_NOT_OK(name_builder.Append(names[i]));
    }

    // finalize the arrays.. prepare to write
    std::shared_ptr<arrow::Array> id_array;
    ARROW_RETURN_NOT_OK(id_builder.Finish(&id_array));

    std::shared_ptr<arrow::Array> name_array;
    ARROW_RETURN_NOT_OK(name_builder.Finish(&name_array));

    // define the schema
    auto schema = arrow::schema({
            arrow::field("ID", arrow::int64()),
            arrow::field("Username", arrow::utf8())
            });
    int64_t num_rows = id_array->length();
    // table can grow while RecordBatch allocates a single array
    auto batch = arrow::RecordBatch::Make(schema, num_rows, {id_array, name_array});

    return writeShard(rank, batch, prefix);

    // could also return arrow::Status::OK()
}
int main(int argc, char **argv)
{
    MPI_Init(&argc, &argv);
    int nprocs, rank;
    MPI_Comm_size(MPI_COMM_WORLD, &nprocs);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    auto status = writeData(rank, nprocs, argv[1]);
    if (!status.ok()) {
        std::cerr << "Error: "<< status.ToString() << std::endl;
        MPI_Abort(MPI_COMM_WORLD, 1);
    }
    MPI_Finalize();
    return 0;
}
