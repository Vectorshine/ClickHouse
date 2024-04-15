#pragma once

#include <Processors/Chunk.h>

namespace DB
{

/// To carry part level if chunk is produced by a merge tree source
class MergeTreePartLevelInfo : public ChunkInfo
{
public:
    MergeTreePartLevelInfo() = delete;
    explicit MergeTreePartLevelInfo(ssize_t part_level) : origin_merge_tree_part_level(part_level) { }
    size_t origin_merge_tree_part_level = 0;
};

inline size_t getPartLevelFromChunk(const Chunk & chunk)
{
    const auto part_level_info = chunk.getChunkInfo<MergeTreePartLevelInfo>();
    if (part_level_info)
        return part_level_info->origin_merge_tree_part_level;
    return 0;
}

}
