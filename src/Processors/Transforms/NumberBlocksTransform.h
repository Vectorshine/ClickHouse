#pragma once

#include <memory>
#include <Processors/Chunk.h>
#include <Processors/ISimpleTransform.h>
#include <Common/Exception.h>

namespace ErrorCodes
{
    extern const int LOGICAL_ERROR;
}

namespace DB
{
    struct SerialBlockNumberInfo : public ChunkInfo
    {
        explicit SerialBlockNumberInfo(size_t block_number_)
            : block_number(block_number_)
        {
        }

        size_t block_number = 0;
    };

    class NumberBlocksTransform : public ISimpleTransform
    {
    public:
        explicit NumberBlocksTransform(const Block & header)
            : ISimpleTransform(header, header, true)
        {
        }

        String getName() const override { return "NumberBlocksTransform"; }

        void transform(Chunk & chunk) override
        {
            chunk.addChunkInfo(std::make_shared<SerialBlockNumberInfo>(++block_number));
        }

    private:
        size_t block_number = 0;
    };

    class DedupTokenInfo : public ChunkInfo
    {
    public:
        explicit DedupTokenInfo(String first_part)
        {
            addTokenPart(std::move(first_part));
        }

        String getToken() const
        {
            String result;
            result.reserve(getTotalSize());

            for (const auto & part : token_parts)
                result.append(part);

            return result;
        }

        void addTokenPart(String part)
        {
            token_parts.push_back(std::move(part));
        }

    private:
        size_t getTotalSize() const
        {
            size_t size = 0;
            for (const auto & part : token_parts)
                size += part.size();
            return size;
        }

        std::vector<String> token_parts = {};
    };

    class AddUserDeduplicationTokenTransform : public ISimpleTransform
    {
    public:
        AddUserDeduplicationTokenTransform(String token_, const Block & header_)
            : ISimpleTransform(header_, header_, true)
            , token(token_)
        {
        }

        String getName() const override { return "AddUserDeduplicationTokenTransform"; }

        void transform(Chunk & chunk) override
        {
            chunk.addChunkInfo(std::make_shared<DedupTokenInfo>(token));
        }

    private:
        String token = {};
    };

    class CheckInsertDeduplicationTokenTransform : public ISimpleTransform
    {
    public:
        CheckInsertDeduplicationTokenTransform(String debug_, bool must_be_present_, const Block & header_)
            : ISimpleTransform(header_, header_, true)
            , debug(debug_)
            , must_be_present(must_be_present_)
        {
        }

        String getName() const override { return "CheckInsertDeduplicationTokenTransform"; }

        void transform(Chunk & chunk) override
        {
            if (!must_be_present)
                return;

            auto token_info = chunk.getChunkInfo<DedupTokenInfo>();
            if (!token_info)
                throw Exception(ErrorCodes::LOGICAL_ERROR, "Chunk has to have DedupTokenInfo as ChunkInfo, {}", debug);
        }

    private:
        String debug;
        bool must_be_present = false;
    };


    class ExtendDeduplicationWithBlockNumberTokenTransform : public ISimpleTransform
    {
    public:
        explicit ExtendDeduplicationWithBlockNumberTokenTransform(const Block & header_)
                : ISimpleTransform(header_, header_, true)
        {
        }

        String getName() const override { return "ExtendDeduplicationWithBlockNumberTokenTransform"; }

        void transform(Chunk & chunk) override
        {
            auto token_info = chunk.extractChunkInfo<DedupTokenInfo>();
            if (!token_info)
                throw Exception(ErrorCodes::LOGICAL_ERROR, "Chunk has to have DedupTokenInfo as ChunkInfo");

            auto block_number_info = chunk.getChunkInfo<SerialBlockNumberInfo>();
            if (!block_number_info)
                throw Exception(ErrorCodes::LOGICAL_ERROR, "Chunk has to have SerialBlockNumberInfo as ChunkInfo");

            token_info->addTokenPart(fmt::format(":block-{}", block_number_info->block_number));
            chunk.addChunkInfo(std::move(token_info));
        }
    };

    class ExtendDeduplicationWithTokenPartTransform : public ISimpleTransform
    {
    public:
        ExtendDeduplicationWithTokenPartTransform(String token_part_, const Block & header_)
                : ISimpleTransform(header_, header_, true)
                , token_part(token_part_)
        {
        }

        String getName() const override { return "ExtendDeduplicationWithBlockNumberTokenTransform"; }

        void transform(Chunk & chunk) override
        {
            auto token_info = chunk.extractChunkInfo<DedupTokenInfo>();
            if (!token_info)
                throw Exception(ErrorCodes::LOGICAL_ERROR, "Chunk has to have DedupTokenInfo as ChunkInfo");

            token_info->addTokenPart(token_part);
            chunk.addChunkInfo(std::move(token_info));
        }

    private:
        String token_part = {};
    };

}
