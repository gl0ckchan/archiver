#include <iostream>
#include <vector>

#include <Windows.h>

#include "src/miniz.h"
#include "src/threadpool.h"

DWORD WINAPI writer_thread(LPVOID lpParam) {
    thread_params* params = (thread_params*)lpParam;
    mz_zip_archive* zip = params->zip;
    thread_pool* thread_pool = params->thread_pool;

    while (1) {
        EnterCriticalSection(&thread_pool->_mutex);

        while (thread_pool->_ready_queue.empty() && !thread_pool->_workers_done) {
            SleepConditionVariableCS(&thread_pool->_condition_var, &thread_pool->_mutex, INFINITE);
        }

        if (thread_pool->_ready_queue.empty() && thread_pool->_workers_done) {
            LeaveCriticalSection(&thread_pool->_mutex);
            break;
        }

        compressed_file* cf = thread_pool->_ready_queue.front();
        thread_pool->_ready_queue.pop_front();
        LeaveCriticalSection(&thread_pool->_mutex);

        mz_bool status = mz_zip_writer_add_mem_ex_v2(
            zip,                                               // pZip
            cf->zip_path,                                      // pArchive_name
            cf->compressed_data,                               // pBuf
            cf->compressed_size,                               // buf_size
            NULL,                                              // pComment
            0,                                                 // comment_size
            MZ_BEST_COMPRESSION | MZ_ZIP_FLAG_COMPRESSED_DATA, // level_and_flags
            cf->original_size,                                 // uncomp_size
            cf->crc32,                                         // uncomp_crc32
            NULL,                                              // last_modified
            NULL,                                              // user_extra_data
            0,                                                 // user_extra_data_len
            NULL,                                              // user_extra_data_central
            0                                                  // user_extra_data_central_len
        );

        if (!status) {
            std::cerr << "failed to write compressed data to zip\n";
        }

        mz_free(cf->compressed_data);
        free(cf->zip_path);
        free(cf);
    }
    return 0;
}

char* wchar_to_utf8(const wchar_t* wstr) {
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, NULL, 0, NULL, NULL);
    char* str = (char*)malloc(size_needed);
    WideCharToMultiByte(CP_UTF8, 0, wstr, -1, str, size_needed, NULL, NULL);
    return str;
}

size_t get_physical_core_count() {
    DWORD length = 0;
    GetLogicalProcessorInformation(nullptr, &length);

    if (GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
        std::vector<SYSTEM_LOGICAL_PROCESSOR_INFORMATION> buffer(length / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION));

        if (GetLogicalProcessorInformation(buffer.data(), &length)) {
            size_t core_count = 0;
            for (const auto& info : buffer) {
                if (info.Relationship == RelationProcessorCore) {
                    core_count++;
                }
            }
            return core_count;
        }
    }
    return 0;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <root folder>\n";
        return 69;
    }

    size_t cores = get_physical_core_count();
    std::cout << "cores: " << cores << std::endl;

    int argc_w = 0;
    wchar_t **argv_w = CommandLineToArgvW(GetCommandLineW(), &argc_w);
    if (!argv_w || argc_w < 2) {
        std::cerr << "Failed to get wide args\n";
        return 69;
    }

    wchar_t* root = argv_w[1];

    std::vector<wchar_t*> folders;
    folders.push_back(_wcsdup(root));

    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    mz_zip_writer_init_file(&zip, "trollface.zip", 0);

    thread_pool thread_pool;
    init_thread_pool(&thread_pool, cores);

    thread_params writer_params;
    writer_params.zip = &zip;
    writer_params.thread_pool = &thread_pool;

    HANDLE writer = CreateThread(NULL, 0, writer_thread, &writer_params, 0, NULL);

    while (!folders.empty()) {
        wchar_t *current = folders.back();
        folders.pop_back();

        wchar_t *pattern =
            (wchar_t *)malloc((wcslen(current) + 3) * sizeof(wchar_t));
        wcscpy(pattern, current);
        wcscat(pattern, L"\\*");

        WIN32_FIND_DATAW file_data;
        HANDLE h = FindFirstFileW(pattern, &file_data);
        if (h == INVALID_HANDLE_VALUE) {
            std::wcerr << "cannot open " << current << ": " << GetLastError() << "\n";

            free(pattern);
            free(current);
            continue;
        }

        do {
            if (wcscmp(file_data.cFileName, L".") == 0 ||
                wcscmp(file_data.cFileName, L"..") == 0)
                continue;

            size_t chars_needed = wcslen(current) + 1 + wcslen(file_data.cFileName) + 1;
            wchar_t *full = (wchar_t *)malloc(chars_needed * sizeof(wchar_t));
            swprintf_s(full, chars_needed, L"%s\\%s", current, file_data.cFileName);

            if (file_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                std::wcout << L"[dir]  " << full << "\n";
                folders.push_back(full);
            }
            else {
                const wchar_t *rel = full + wcslen(root);
                while (*rel == L'\\' || *rel == L'/')
                    rel++;

                char *rel_utf8 = wchar_to_utf8(rel);
                char *full_utf8 = wchar_to_utf8(full);

                for (int i = 0; rel_utf8[i] != '\0'; i++) {
                    if (rel_utf8[i] == '\\') {
                        rel_utf8[i] = '/';
                    }
                }

                zip_task *task = (zip_task *)malloc(sizeof(zip_task));
                task->zip = NULL;
                task->file_name = rel_utf8;
                task->file_path = full_utf8;

                add_task_to_pool(&thread_pool, task);

                std::wcout << L"[file] " << full << std::endl;

                free(full);
            }
        } while (FindNextFileW(h, &file_data));

        FindClose(h);
        free(pattern);
        free(current);
    }

    destroy_thread_pool(&thread_pool);

    WaitForSingleObject(writer, INFINITE);
    CloseHandle(writer);

    mz_zip_writer_finalize_archive(&zip);
    mz_zip_writer_end(&zip);

    DeleteCriticalSection(&thread_pool._mutex);

    LocalFree(argv_w);
    return 0;
}
