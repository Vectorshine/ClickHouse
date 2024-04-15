#pragma once

#include <Columns/IColumn.h>
#include <unordered_map>
#include <deque>

namespace DB
{

class ChunkInfo
{
public:
    virtual ~ChunkInfo() = default;
    ChunkInfo() = default;
};

using ChunkInfoPtr = std::shared_ptr<const ChunkInfo>;

/**
 * Chunk is a list of columns with the same length.
 * Chunk stores the number of rows in a separate field and supports invariant of equal column length.
 *
 * Chunk has move-only semantic. It's more lightweight than block cause doesn't store names, types and index_by_name.
 *
 * Chunk can have empty set of columns but non-zero number of rows. It helps when only the number of rows is needed.
 * Chunk can have columns with zero number of rows. It may happen, for example, if all rows were filtered.
 * Chunk is empty only if it has zero rows and empty list of columns.
 *
 * Any ChunkInfo may be attached to chunk.
 * It may be useful if additional info per chunk is needed. For example, bucket number for aggregated data.
**/

class Chunk
{
public:
    Chunk() = default;
    Chunk(const Chunk & other) = delete;
    Chunk(Chunk && other) noexcept
        : columns(std::move(other.columns))
        , num_rows(other.num_rows)
        , chunk_infos(std::move(other.chunk_infos))
    {
        other.num_rows = 0;
    }

    Chunk(Columns columns_, UInt64 num_rows_);
    Chunk(Columns columns_, UInt64 num_rows_, std::deque<ChunkInfoPtr> chunk_infos_);
    Chunk(MutableColumns columns_, UInt64 num_rows_);
    Chunk(MutableColumns columns_, UInt64 num_rows_, std::deque<ChunkInfoPtr> chunk_infos_);

    Chunk & operator=(const Chunk & other) = delete;
    Chunk & operator=(Chunk && other) noexcept
    {
        columns = std::move(other.columns);
        chunk_infos = std::move(other.chunk_infos);
        num_rows = other.num_rows;
        other.num_rows = 0;
        return *this;
    }

    Chunk clone() const;

    void swap(Chunk & other) noexcept
    {
        columns.swap(other.columns);
        chunk_infos.swap(other.chunk_infos);
        std::swap(num_rows, other.num_rows);
    }

    void clear()
    {
        num_rows = 0;
        columns.clear();
        chunk_infos.clear();
    }

    const Columns & getColumns() const { return columns; }
    void setColumns(Columns columns_, UInt64 num_rows_);
    void setColumns(MutableColumns columns_, UInt64 num_rows_);
    Columns detachColumns();
    MutableColumns mutateColumns();
    /** Get empty columns with the same types as in block. */
    MutableColumns cloneEmptyColumns() const;

    const std::deque<ChunkInfoPtr> & getChunkInfos() const { return chunk_infos; }
    void setChunkInfos(std::deque<ChunkInfoPtr> chunk_infos_) { chunk_infos = std::move(chunk_infos_); }
    size_t hasAnyChunkInfo() const { return !chunk_infos.empty(); }

    template <class T>
    std::shared_ptr<const T> getChunkInfo() const
    {
        static_assert(std::is_base_of_v<ChunkInfo, T>, "Template parameter must inherit ChunkInfo");
        for (const auto & it : chunk_infos)
        {
            auto cast = std::dynamic_pointer_cast<const T>(it);
            if (cast)
                return cast;
        }
        return nullptr;
    }

    template <class T>
    std::shared_ptr<T> extractChunkInfo()
    {
        static_assert(std::is_base_of_v<ChunkInfo, T>, "Template parameter must inherit ChunkInfo");
        for (auto it = chunk_infos.begin(); it != chunk_infos.end(); ++it)
        {
            auto cast = std::dynamic_pointer_cast<const T>(*it);
            if (cast)
            {
                it = chunk_infos.erase(it);
                return std::const_pointer_cast<T>(cast);
            }
        }
        return nullptr;
    }

    template <class T>
    void addChunkInfo(std::shared_ptr<T> info)
    {
        static_assert(std::is_base_of_v<ChunkInfo, T>, "Template parameter must inherit ChunkInfo");
        chassert(!getChunkInfo<T>());
        chunk_infos.emplace_back(std::move(info));
    }

    UInt64 getNumRows() const { return num_rows; }
    UInt64 getNumColumns() const { return columns.size(); }
    bool hasRows() const { return num_rows > 0; }
    bool hasColumns() const { return !columns.empty(); }
    bool empty() const { return !hasRows() && !hasColumns(); }
    explicit operator bool() const { return !empty(); }

    void addColumn(ColumnPtr column);
    void addColumn(size_t position, ColumnPtr column);
    void erase(size_t position);

    UInt64 bytes() const;
    UInt64 allocatedBytes() const;

    std::string dumpStructure() const;

    void append(const Chunk & chunk);
    void append(const Chunk & chunk, size_t from, size_t length); // append rows [from, from+length) of chunk

private:
    Columns columns;
    UInt64 num_rows = 0;
    std::deque<ChunkInfoPtr> chunk_infos;

    void checkNumRowsIsConsistent();
};

using Chunks = std::vector<Chunk>;

/// AsyncInsert needs two kinds of information:
/// - offsets of different sub-chunks
/// - tokens of different sub-chunks, which are assigned by setting `insert_deduplication_token`.
class AsyncInsertInfo : public ChunkInfo
{
public:
    AsyncInsertInfo() = default;
    explicit AsyncInsertInfo(const std::vector<size_t> & offsets_, const std::vector<String> & tokens_) : offsets(offsets_), tokens(tokens_) {}

    std::vector<size_t> offsets;
    std::vector<String> tokens;
};

using AsyncInsertInfoPtr = std::shared_ptr<AsyncInsertInfo>;

/// Extension to support delayed defaults. AddingDefaultsProcessor uses it to replace missing values with column defaults.
class ChunkMissingValues : public ChunkInfo
{
public:
    using RowsBitMask = std::vector<bool>; /// a bit per row for a column

    const RowsBitMask & getDefaultsBitmask(size_t column_idx) const;
    void setBit(size_t column_idx, size_t row_idx);
    bool empty() const { return rows_mask_by_column_id.empty(); }
    size_t size() const { return rows_mask_by_column_id.size(); }
    void clear() { rows_mask_by_column_id.clear(); }

private:
    using RowsMaskByColumnId = std::unordered_map<size_t, RowsBitMask>;

    /// If rows_mask_by_column_id[column_id][row_id] is true related value in Block should be replaced with column default.
    /// It could contain less columns and rows then related block.
    RowsMaskByColumnId rows_mask_by_column_id;
};

/// Converts all columns to full serialization in chunk.
/// It's needed, when you have to access to the internals of the column,
/// or when you need to perform operation with two columns
/// and their structure must be equal (e.g. compareAt).
void convertToFullIfConst(Chunk & chunk);
void convertToFullIfSparse(Chunk & chunk);

/// Creates a chunk with the same columns but makes them constants with a default value and a specified number of rows.
Chunk cloneConstWithDefault(const Chunk & chunk, size_t num_rows);

}
