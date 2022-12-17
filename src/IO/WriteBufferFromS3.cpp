#include "config.h"
#include <Common/ProfileEvents.h>

#if USE_AWS_S3

#include <Common/logger_useful.h>
#include <Common/Throttler.h>
#include <Interpreters/Cache/FileCache.h>

#include <IO/WriteBufferFromS3.h>
#include <IO/WriteHelpers.h>
#include <IO/S3Common.h>
#include <Interpreters/Context.h>

#include <aws/s3/S3Client.h>
#include <aws/s3/model/CreateMultipartUploadRequest.h>
#include <aws/s3/model/CompleteMultipartUploadRequest.h>
#include <aws/s3/model/PutObjectRequest.h>
#include <aws/s3/model/UploadPartRequest.h>
#include <aws/s3/model/HeadObjectRequest.h>

#include <utility>


namespace ProfileEvents
{
    extern const Event WriteBufferFromS3Bytes;
    extern const Event S3WriteBytes;

    extern const Event S3HeadObject;
    extern const Event S3CreateMultipartUpload;
    extern const Event S3CompleteMultipartUpload;
    extern const Event S3UploadPart;
    extern const Event S3PutObject;

    extern const Event DiskS3HeadObject;
    extern const Event DiskS3CreateMultipartUpload;
    extern const Event DiskS3CompleteMultipartUpload;
    extern const Event DiskS3UploadPart;
    extern const Event DiskS3PutObject;
}

namespace DB
{
// S3 protocol does not allow to have multipart upload with more than 10000 parts.
// In case server does not return an error on exceeding that number, we print a warning
// because custom S3 implementation may allow relaxed requirements on that.
const int S3_WARN_MAX_PARTS = 10000;

namespace ErrorCodes
{
    extern const int S3_ERROR;
    extern const int INVALID_CONFIG_PARAMETER;
}

struct WriteBufferFromS3::UploadPartTask
{
    Aws::S3::Model::UploadPartRequest req;
    bool is_finished = false;
    std::string tag;
    std::exception_ptr exception;
};

struct WriteBufferFromS3::PutObjectTask
{
    Aws::S3::Model::PutObjectRequest req;
    bool is_finished = false;
    std::exception_ptr exception;
};

WriteBufferFromS3::Memory::~Memory()
{
    if (data)
        allocator.free(data, size);
}

WriteBufferFromS3::WriteBufferFromS3(
    std::shared_ptr<const Aws::S3::S3Client> client_ptr_,
    const String & bucket_,
    const String & key_,
    const S3Settings::RequestSettings & request_settings_,
    std::optional<std::map<String, String>> object_metadata_,
    size_t buffer_size_,
    ThreadPoolCallbackRunner<void> schedule_,
    const WriteSettings & write_settings_)
    : WriteBuffer(nullptr, 0)
    , bucket(bucket_)
    , key(key_)
    , request_settings(request_settings_)
    , client_ptr(std::move(client_ptr_))
    , object_metadata(std::move(object_metadata_))
    , upload_part_size(request_settings_.min_upload_part_size)
    , schedule(std::move(schedule_))
    , write_settings(write_settings_)
{
    memory = std::make_unique<Memory>();
    memory->size = request_settings.max_single_part_upload_size;

    if (buffer_size_)
        memory->size = std::min(buffer_size_, request_settings.max_single_part_upload_size);

    memory->data = static_cast<char *>(memory->allocator.alloc(memory->size));
    set(memory->data, memory->size);
}

static void reallocBuffer(WriteBufferFromS3::Memory & memory, size_t max_size)
{
    static constexpr size_t factor = 4;
    size_t new_size = roundUpToPowerOfTwoOrZero(memory.size) * factor;
    new_size = std::min(new_size, max_size);

    memory.data = static_cast<char *>(memory.allocator.realloc(memory.data, memory.size, new_size));
    memory.size = new_size;
}

static void reallocBuffer(WriteBuffer & buffer, WriteBufferFromS3::Memory & memory, size_t max_size)
{
    size_t prev_size = memory.size;
    reallocBuffer(memory, max_size);
    buffer.set(memory.data, memory.size, prev_size);
    buffer.buffer() = BufferBase::Buffer(memory.data + prev_size, memory.data + memory.size);
}

void WriteBufferFromS3::nextImpl()
{
    written_bytes_in_memory = pos - memory->data;
    if (size_t available_bytes = available(); available_bytes > 0)
    {
        buffer() = BufferBase::Buffer(pos, pos + available_bytes);
        return;
    }

    if (multipart_upload_id.empty() && memory->size < request_settings.max_single_part_upload_size)
    {
        reallocBuffer(*this, *memory, request_settings.max_single_part_upload_size);
        return;
    }

    ProfileEvents::increment(ProfileEvents::WriteBufferFromS3Bytes, memory->size);
    if (write_settings.remote_throttler)
        write_settings.remote_throttler->add(memory->size);

    if (multipart_upload_id.empty())
        createMultipartUpload();

    if (part_number == 0 && memory->size < upload_part_size)
    {
        /// This is a case where max_single_part_upload_size < min_upload_part_size.
        /// For the first part of multipart upload we may need to realloc a buffer from single part upload a few times.
        /// For other parts, buffer size is always upload_part_size.
        reallocBuffer(*this, *memory, upload_part_size);
        return;
    }

    writePart(allocateBuffer());
    waitForReadyBackGroundTasks();
}

std::shared_ptr<Aws::StringStream> WriteBufferFromS3::allocateBuffer()
{
    size_t cur_size = memory->size;
    auto temporary_buffer = Aws::MakeShared<Aws::SimpleStringStream>("temporary buffer", std::move(memory), cur_size);

    memory = std::make_unique<Memory>();
    memory->size = upload_part_size;
    memory->data = static_cast<char *>(memory->allocator.alloc(memory->size));

    set(memory->data, memory->size);
    written_bytes_in_memory = 0;

    temporary_buffer->exceptions(std::ios::badbit);
    return temporary_buffer;
}

WriteBufferFromS3::~WriteBufferFromS3()
{
#ifndef NDEBUG
    if (!finalized)
    {
        LOG_ERROR(log, "WriteBufferFromS3 is not finalized in destructor. It's a bug");
        std::terminate();
    }
#else
    try
    {
        finalize();
    }
    catch (...)
    {
        tryLogCurrentException(__PRETTY_FUNCTION__);
    }
#endif
}

void WriteBufferFromS3::preFinalize()
{
    next();
    is_prefinalized = true;

    // if (written_bytes_in_memory == 0)
    //     return;

    auto temporary_buffer = Aws::MakeShared<Aws::SimpleStringStream>("temporary buffer", std::move(memory), written_bytes_in_memory);
    written_bytes_in_memory = 0;

    if (multipart_upload_id.empty())
    {
        makeSinglepartUpload(std::move(temporary_buffer));
    }
    else
    {
        /// Write rest of the data as last part.
        writePart(std::move(temporary_buffer));
    }
}

void WriteBufferFromS3::finalizeImpl()
{
    if (!is_prefinalized)
        preFinalize();

    waitForAllBackGroundTasks();

    if (!multipart_upload_id.empty())
        completeMultipartUpload();

    if (true) //request_settings.check_objects_after_upload)
    {
        LOG_TRACE(log, "Checking object {} exists after upload", key);

        auto response = S3::headObject(*client_ptr, bucket, key, "", write_settings.for_object_storage);
        if (!response.IsSuccess())
            throw S3Exception(fmt::format("Object {} from bucket {} disappeared immediately after upload, it's a bug in S3 or S3 API.", key, bucket), response.GetError().GetErrorType());
        else
            LOG_TRACE(log, "Object {} exists after upload", key);
    }
}

void WriteBufferFromS3::createMultipartUpload()
{
    Aws::S3::Model::CreateMultipartUploadRequest req;
    req.SetBucket(bucket);
    req.SetKey(key);

    /// If we don't do it, AWS SDK can mistakenly set it to application/xml, see https://github.com/aws/aws-sdk-cpp/issues/1840
    req.SetContentType("binary/octet-stream");

    if (object_metadata.has_value())
        req.SetMetadata(object_metadata.value());

    ProfileEvents::increment(ProfileEvents::S3CreateMultipartUpload);
    if (write_settings.for_object_storage)
        ProfileEvents::increment(ProfileEvents::DiskS3CreateMultipartUpload);

    auto outcome = client_ptr->CreateMultipartUpload(req);

    if (outcome.IsSuccess())
    {
        multipart_upload_id = outcome.GetResult().GetUploadId();
        LOG_TRACE(log, "Multipart upload has created. Bucket: {}, Key: {}, Upload id: {}", bucket, key, multipart_upload_id);
    }
    else
        throw S3Exception(outcome.GetError().GetMessage(), outcome.GetError().GetErrorType());
}

void WriteBufferFromS3::writePart(std::shared_ptr<Aws::StringStream> temporary_buffer)
{
    auto size = temporary_buffer->tellp();

    LOG_TRACE(log, "Writing part. Bucket: {}, Key: {}, Upload_id: {}, Size: {}", bucket, key, multipart_upload_id, size);

    if (size < 0)
    {
        LOG_WARNING(log, "Skipping part upload. Buffer is in bad state, it means that we have tried to upload something, but got an exception.");
        return;
    }

    if (size == 0)
    {
        LOG_TRACE(log, "Skipping writing part. Buffer is empty.");
        return;
    }

    if (TSA_SUPPRESS_WARNING_FOR_READ(part_tags).size() == S3_WARN_MAX_PARTS)
    {
        // Don't throw exception here by ourselves but leave the decision to take by S3 server.
        LOG_WARNING(log, "Maximum part number in S3 protocol has reached (too many parts). Server may not accept this whole upload.");
    }

    if (schedule)
    {
        UploadPartTask * task = nullptr;

        {
            std::lock_guard lock(bg_tasks_mutex);
            task = &upload_object_tasks.emplace_back();
            ++num_added_bg_tasks;
        }

        /// Notify waiting thread when task finished
        auto task_finish_notify = [&, task]()
        {
            std::lock_guard lock(bg_tasks_mutex);
            task->is_finished = true;
            ++num_finished_bg_tasks;

            /// Notification under mutex is important here.
            /// Otherwise, WriteBuffer could be destroyed in between
            /// Releasing lock and condvar notification.
            bg_tasks_condvar.notify_one();
        };

        try
        {
            fillUploadRequest(task->req, std::move(temporary_buffer));

            schedule([this, task, task_finish_notify]()
            {
                try
                {
                    processUploadRequest(*task);
                }
                catch (...)
                {
                    task->exception = std::current_exception();
                }

                task_finish_notify();
            }, 0);
        }
        catch (...)
        {
            task_finish_notify();
            throw;
        }
    }
    else
    {
        UploadPartTask task;
        auto & tags = TSA_SUPPRESS_WARNING_FOR_WRITE(part_tags); /// Suppress warning because schedule == false.

        fillUploadRequest(task.req, std::move(temporary_buffer));
        processUploadRequest(task);
        tags.push_back(task.tag);
    }
}

void WriteBufferFromS3::fillUploadRequest(Aws::S3::Model::UploadPartRequest & req, std::shared_ptr<Aws::StringStream> temporary_buffer)
{
    /// Increase part number.
    ++part_number;
    if (!multipart_upload_id.empty() && (part_number > request_settings.max_part_number))
    {
        throw Exception(
            ErrorCodes::INVALID_CONFIG_PARAMETER,
            "Part number exceeded {} while writing {} bytes to S3. Check min_upload_part_size = {}, max_upload_part_size = {}, "
            "upload_part_size_multiply_factor = {}, upload_part_size_multiply_parts_count_threshold = {}, max_single_part_upload_size = {}",
            request_settings.max_part_number, count(), request_settings.min_upload_part_size, request_settings.max_upload_part_size,
            request_settings.upload_part_size_multiply_factor, request_settings.upload_part_size_multiply_parts_count_threshold,
            request_settings.max_single_part_upload_size);
    }

    /// Setup request.
    req.SetBucket(bucket);
    req.SetKey(key);
    req.SetPartNumber(static_cast<int>(part_number));
    req.SetUploadId(multipart_upload_id);
    req.SetContentLength(temporary_buffer->tellp());
    req.SetBody(temporary_buffer);

    /// If we don't do it, AWS SDK can mistakenly set it to application/xml, see https://github.com/aws/aws-sdk-cpp/issues/1840
    req.SetContentType("binary/octet-stream");

    /// Maybe increase `upload_part_size` (we need to increase it sometimes to keep `part_number` less or equal than `max_part_number`).
    if (!multipart_upload_id.empty() && (part_number % request_settings.upload_part_size_multiply_parts_count_threshold == 0))
    {
        upload_part_size *= request_settings.upload_part_size_multiply_factor;
        upload_part_size = std::min(upload_part_size, request_settings.max_upload_part_size);
    }
}

void WriteBufferFromS3::processUploadRequest(UploadPartTask & task)
{
    ProfileEvents::increment(ProfileEvents::S3UploadPart);
    if (write_settings.for_object_storage)
        ProfileEvents::increment(ProfileEvents::DiskS3UploadPart);

    auto outcome = client_ptr->UploadPart(task.req);

    if (outcome.IsSuccess())
    {
        task.tag = outcome.GetResult().GetETag();
        std::lock_guard lock(bg_tasks_mutex); /// Protect part_tags from race
        LOG_TRACE(log, "Writing part finished. Bucket: {}, Key: {}, Upload_id: {}, Etag: {}, Parts: {}", bucket, key, multipart_upload_id, task.tag, part_tags.size());
    }
    else
        throw S3Exception(outcome.GetError().GetMessage(), outcome.GetError().GetErrorType());
}

void WriteBufferFromS3::completeMultipartUpload()
{
    const auto & tags = TSA_SUPPRESS_WARNING_FOR_READ(part_tags);

    LOG_TRACE(log, "Completing multipart upload. Bucket: {}, Key: {}, Upload_id: {}, Parts: {}", bucket, key, multipart_upload_id, tags.size());

    if (tags.empty())
        throw Exception("Failed to complete multipart upload. No parts have uploaded", ErrorCodes::S3_ERROR);

    Aws::S3::Model::CompleteMultipartUploadRequest req;
    req.SetBucket(bucket);
    req.SetKey(key);
    req.SetUploadId(multipart_upload_id);

    Aws::S3::Model::CompletedMultipartUpload multipart_upload;
    for (size_t i = 0; i < tags.size(); ++i)
    {
        Aws::S3::Model::CompletedPart part;
        multipart_upload.AddParts(part.WithETag(tags[i]).WithPartNumber(static_cast<int>(i + 1)));
    }

    req.SetMultipartUpload(multipart_upload);

    size_t max_retry = std::max(request_settings.max_unexpected_write_error_retries, 1UL);
    for (size_t i = 0; i < max_retry; ++i)
    {
        ProfileEvents::increment(ProfileEvents::S3CompleteMultipartUpload);
        if (write_settings.for_object_storage)
            ProfileEvents::increment(ProfileEvents::DiskS3CompleteMultipartUpload);

        auto outcome = client_ptr->CompleteMultipartUpload(req);

        if (outcome.IsSuccess())
        {
            LOG_TRACE(log, "Multipart upload has completed. Bucket: {}, Key: {}, Upload_id: {}, Parts: {}", bucket, key, multipart_upload_id, tags.size());
            break;
        }
        else if (outcome.GetError().GetErrorType() == Aws::S3::S3Errors::NO_SUCH_KEY)
        {
            /// For unknown reason, at least MinIO can respond with NO_SUCH_KEY for put requests
            /// BTW, NO_SUCH_UPLOAD is expected error and we shouldn't retry it
            LOG_INFO(log, "Multipart upload failed with NO_SUCH_KEY error for Bucket: {}, Key: {}, Upload_id: {}, Parts: {}, will retry", bucket, key, multipart_upload_id, tags.size());
        }
        else
        {
            throw S3Exception(
                outcome.GetError().GetErrorType(),
                "Message: {}, Key: {}, Bucket: {}, Tags: {}",
                outcome.GetError().GetMessage(), key, bucket, fmt::join(tags.begin(), tags.end(), " "));
        }
    }
}

void WriteBufferFromS3::makeSinglepartUpload(std::shared_ptr<Aws::StringStream> temporary_buffer)
{
    auto size = temporary_buffer->tellp();
    bool with_pool = static_cast<bool>(schedule);

    LOG_TRACE(log, "Making single part upload. Bucket: {}, Key: {}, Size: {}, WithPool: {}", bucket, key, size, with_pool);

    if (size < 0)
    {
        LOG_WARNING(log, "Skipping single part upload. Buffer is in bad state, it mean that we have tried to upload something, but got an exception.");
        return;
    }

    if (schedule)
    {
        put_object_task = std::make_unique<PutObjectTask>();

        /// Notify waiting thread when put object task finished
        auto task_notify_finish = [&]()
        {
            std::lock_guard lock(bg_tasks_mutex);
            put_object_task->is_finished = true;

            /// Notification under mutex is important here.
            /// Othervies, WriteBuffer could be destroyed in between
            /// Releasing lock and condvar notification.
            bg_tasks_condvar.notify_one();
        };

        try
        {
            fillPutRequest(put_object_task->req, std::move(temporary_buffer));

            schedule([this, task_notify_finish]()
            {
                try
                {
                    processPutRequest(*put_object_task);
                }
                catch (...)
                {
                    put_object_task->exception = std::current_exception();
                }

                task_notify_finish();
            }, 0);
        }
        catch (...)
        {
            task_notify_finish();
            throw;
        }
    }
    else
    {
        PutObjectTask task;
        fillPutRequest(task.req, std::move(temporary_buffer));
        processPutRequest(task);
    }
}

void WriteBufferFromS3::fillPutRequest(Aws::S3::Model::PutObjectRequest & req, std::shared_ptr<Aws::StringStream> temporary_buffer)
{
    req.SetBucket(bucket);
    req.SetKey(key);
    req.SetContentLength(temporary_buffer->tellp());
    req.SetBody(temporary_buffer);
    if (object_metadata.has_value())
        req.SetMetadata(object_metadata.value());

    /// If we don't do it, AWS SDK can mistakenly set it to application/xml, see https://github.com/aws/aws-sdk-cpp/issues/1840
    req.SetContentType("binary/octet-stream");
}

void WriteBufferFromS3::processPutRequest(const PutObjectTask & task)
{
    size_t max_retry = std::max(request_settings.max_unexpected_write_error_retries, 1UL);
    for (size_t i = 0; i < max_retry; ++i)
    {
        ProfileEvents::increment(ProfileEvents::S3PutObject);
        if (write_settings.for_object_storage)
            ProfileEvents::increment(ProfileEvents::DiskS3PutObject);
        auto outcome = client_ptr->PutObject(task.req);
        bool with_pool = static_cast<bool>(schedule);
        if (outcome.IsSuccess())
        {
            LOG_TRACE(log, "Single part upload has completed. Bucket: {}, Key: {}, Object size: {}, WithPool: {}", bucket, key, task.req.GetContentLength(), with_pool);
            break;
        }
        else if (outcome.GetError().GetErrorType() == Aws::S3::S3Errors::NO_SUCH_KEY)
        {
            /// For unknown reason, at least MinIO can respond with NO_SUCH_KEY for put requests
            LOG_INFO(log, "Single part upload failed with NO_SUCH_KEY error for Bucket: {}, Key: {}, Object size: {}, WithPool: {}, will retry", bucket, key, task.req.GetContentLength(), with_pool);
        }
        else
            throw S3Exception(
                outcome.GetError().GetErrorType(),
                "Message: {}, Key: {}, Bucket: {}, Object size: {}, WithPool: {}",
                outcome.GetError().GetMessage(), key, bucket, task.req.GetContentLength(), with_pool);
    }
}

void WriteBufferFromS3::waitForReadyBackGroundTasks()
{
    if (schedule)
    {
        std::unique_lock lock(bg_tasks_mutex);

        /// Suppress warnings because bg_tasks_mutex is actually hold, but tsa annotations do not understand std::unique_lock
        auto & tasks = TSA_SUPPRESS_WARNING_FOR_WRITE(upload_object_tasks);

        while (!tasks.empty() && tasks.front().is_finished)
        {
            auto & task = tasks.front();
            auto exception = task.exception;
            auto tag = std::move(task.tag);
            tasks.pop_front();

            if (exception)
            {
                waitForAllBackGroundTasksUnlocked(lock);
                std::rethrow_exception(exception);
            }

            TSA_SUPPRESS_WARNING_FOR_WRITE(part_tags).push_back(tag);
        }
    }
}

void WriteBufferFromS3::waitForAllBackGroundTasks()
{
    if (schedule)
    {
        std::unique_lock lock(bg_tasks_mutex);
        waitForAllBackGroundTasksUnlocked(lock);
    }
}

void WriteBufferFromS3::waitForAllBackGroundTasksUnlocked(std::unique_lock<std::mutex> & bg_tasks_lock)
{
    if (schedule)
    {
        bg_tasks_condvar.wait(bg_tasks_lock, [this]() {return TSA_SUPPRESS_WARNING_FOR_READ(num_added_bg_tasks) == TSA_SUPPRESS_WARNING_FOR_READ(num_finished_bg_tasks); });

        /// Suppress warnings because bg_tasks_mutex is actually hold, but tsa annotations do not understand std::unique_lock
        auto & tasks = TSA_SUPPRESS_WARNING_FOR_WRITE(upload_object_tasks);
        while (!tasks.empty())
        {
            auto & task = tasks.front();

            if (task.exception)
                std::rethrow_exception(task.exception);

            TSA_SUPPRESS_WARNING_FOR_WRITE(part_tags).push_back(task.tag);

            tasks.pop_front();
        }

        if (put_object_task)
        {
            bg_tasks_condvar.wait(bg_tasks_lock, [this]() { return put_object_task->is_finished; });
            if (put_object_task->exception)
                std::rethrow_exception(put_object_task->exception);
        }
    }
}

}

#endif
