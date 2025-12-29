#pragma once

#include "Plugin.h"
#include <vector>
#include <map>
#include <queue>
#include <mutex>

struct ReadKey {
    uintptr_t address;
    size_t size;

    bool operator<(const ReadKey& other) const {
        return std::tie(address, size) < std::tie(other.address, other.size);
    }
};

struct CachedBlock {
    std::vector<uint8_t> data;
    // optionally timestamp, validity flags, etc.
};

static std::map<ReadKey, CachedBlock> g_cache;
static std::mutex g_cache_mutex;

enum class JobType {
    Read,
    Write,
    OpenProcess,
    CloseProcess
};

struct Job {
    JobType type;
    uintptr_t address;
    std::vector<uint8_t> data; // for write
    std::string processName;   // for open_process
};

static std::queue<Job> g_jobs;
static std::mutex g_jobs_mutex;
static std::condition_variable g_jobs_cv;

static std::atomic<bool> g_workerRunning{ false };
static std::thread* g_workerThread = nullptr;

//void worker_thread();
bool StartWorker();
void StopWorker();