#pragma once

#include <deque>
#include <vector>

#include <Windows.h>
#include <winnt.h>

#include "miniz.h"

typedef struct {
    mz_zip_archive *zip;
    char *file_name;
    char *file_path;
} zip_task;

typedef struct {
    char *zip_path;
    void *compressed_data;
    size_t compressed_size;
    size_t original_size;
    mz_uint32 crc32;
} compressed_file;

typedef struct {
    PTP_POOL pool;
    PTP_CLEANUP_GROUP cleanup_group;

    std::deque<compressed_file *> _ready_queue;

    CRITICAL_SECTION _mutex;
    CONDITION_VARIABLE _condition_var;
    TP_CALLBACK_ENVIRON callback_env;

    bool _workers_done;
} thread_pool;

typedef struct {
    mz_zip_archive *zip;
    thread_pool *thread_pool;
} thread_params;

typedef struct {
    zip_task *task;
    thread_pool *thread_pool;
} worker_params;

VOID CALLBACK worker_callback(PTP_CALLBACK_INSTANCE Instance, PVOID Parameter, PTP_WORK Work) {
    worker_params *params = (worker_params *)Parameter;
    thread_pool *pool = params->thread_pool;
    zip_task *task = params->task;

    FILE *f = fopen(task->file_path, "rb");
    if (!f) {
        free(task->file_name);
        free(task->file_path);
        free(task);
        free(params);
        return;
    }

    fseek(f, 0, SEEK_END);
    size_t original_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *raw_data = (char *)malloc(original_size);
    fread(raw_data, 1, original_size, f);
    fclose(f);

    size_t compressed_size = 0;
    void *compressed_data = tdefl_compress_mem_to_heap(raw_data, original_size, &compressed_size, TDEFL_DEFAULT_MAX_PROBES);

    mz_uint32 crc = (mz_uint32)mz_crc32(MZ_CRC32_INIT, (const mz_uint8*)raw_data, original_size);

    free(raw_data);

    if (!compressed_data) {
        free(task->file_name);
        free(task->file_path);
        free(task);
        free(params);
        return;
    }

    compressed_file *result = (compressed_file *)malloc(sizeof(compressed_file));
    result->zip_path = task->file_name;
    result->compressed_data = compressed_data;
    result->compressed_size = compressed_size;
    result->original_size = original_size;
    result->crc32 = crc;

    EnterCriticalSection(&pool->_mutex);
    pool->_ready_queue.push_back(result);
    LeaveCriticalSection(&pool->_mutex);
    WakeConditionVariable(&pool->_condition_var);
    free(task->file_path);
    free(task);
    free(params);
}

void init_thread_pool(thread_pool *thread_pool, size_t number_of_threads) {
    thread_pool->pool = CreateThreadpool(NULL);
    SetThreadpoolThreadMaximum(thread_pool->pool, number_of_threads - 1);
    SetThreadpoolThreadMinimum(thread_pool->pool, 1);

    InitializeThreadpoolEnvironment(&thread_pool->callback_env);
    SetThreadpoolCallbackPool(&thread_pool->callback_env, thread_pool->pool);

    thread_pool->cleanup_group = CreateThreadpoolCleanupGroup();
    SetThreadpoolCallbackCleanupGroup(&thread_pool->callback_env, thread_pool->cleanup_group, NULL);

    InitializeCriticalSection(&thread_pool->_mutex);
    InitializeConditionVariable(&thread_pool->_condition_var);
    thread_pool->_workers_done = false;
}

void destroy_thread_pool(thread_pool *thread_pool) {
    CloseThreadpoolCleanupGroupMembers(thread_pool->cleanup_group, FALSE, NULL);
    CloseThreadpoolCleanupGroup(thread_pool->cleanup_group);
    CloseThreadpool(thread_pool->pool);
    DestroyThreadpoolEnvironment(&thread_pool->callback_env);

    EnterCriticalSection(&thread_pool->_mutex);
    thread_pool->_workers_done = true;
    WakeConditionVariable(&thread_pool->_condition_var);
    LeaveCriticalSection(&thread_pool->_mutex);
}

void add_task_to_pool(thread_pool *thread_pool, zip_task *zip_task) {
    worker_params *params = (worker_params *)malloc(sizeof(worker_params));
    params->task = zip_task;
    params->thread_pool = thread_pool;

    PTP_WORK work = CreateThreadpoolWork(worker_callback, params, &thread_pool->callback_env);
    SubmitThreadpoolWork(work);
}
