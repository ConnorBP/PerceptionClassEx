#include "ReadCache.hpp"
#include "WebSocketServer.hpp"

std::map<ReadKey, CachedBlock> g_cache;
std::shared_mutex g_cache_mutex;


std::queue<Job> g_jobs;
std::mutex g_jobs_mutex;
std::condition_variable g_jobs_cv;

std::atomic<bool> g_workerRunning{ false };
std::thread* g_workerThread = nullptr;

void worker_thread()
{
    LOG("STARTING WORKER THREAD");

    while (g_workerRunning) {

        Job job;
        bool haveJob = false;

        {
            std::lock_guard<std::mutex> lock(g_jobs_mutex);
            if (!g_jobs.empty()) {
                job = std::move(g_jobs.front());
                g_jobs.pop();
                haveJob = true;
            }
        }

        if (!haveJob) {
            // No work to do right now, back off a bit
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        std::string cmd, response;

        switch (job.type) {

        case JobType::OpenProcess: {
            char buf[128];
            snprintf(buf, sizeof(buf),
                "{\"cmd\":\"open_process\",\"pid\":%u}", job.pid);
            std::string cmd(buf), response;
            SendWebSocketCommand(cmd, response, 1000);
            break;
        }

        case JobType::CloseProcess: {
            cmd = "{\"cmd\":\"close_process\"}";
            SendWebSocketCommand(cmd, response, 500);
            break;
        }

        case JobType::Read: {
            char buf[128];
            snprintf(buf, sizeof(buf),
                "{\"cmd\":\"read\",\"address\":%llu,\"size\":%llu}",
                (unsigned long long)job.address,
                (unsigned long long)job.data.size());
            cmd = buf;

            if (SendWebSocketCommand(cmd, response, 100)) {
                auto pos = response.find("\"data\":\"");
                if (pos != std::string::npos) {
                    pos += 8;
                    auto end = response.find("\"", pos);
                    if (end != std::string::npos) {
                        std::string hex = response.substr(pos, end - pos);
                        std::vector<uint8_t> bytes;
                        bytes.reserve(hex.size() / 2);
                        for (size_t i = 0; i + 1 < hex.size(); i += 2) {
                            unsigned int b = 0;
                            sscanf(hex.c_str() + i, "%2x", &b);
                            bytes.push_back((uint8_t)b);
                        }

                        ReadKey key{ job.address, job.data.size() };
                        CachedBlock block;
                        block.data = std::move(bytes);

                        {
                            std::unique_lock<std::shared_mutex> lock(g_cache_mutex);
                            g_cache[key] = std::move(block);
                        }
                    }
                }
            }
            break;
        }

        case JobType::Write: {
            static const char hex_digits[] = "0123456789ABCDEF";
            std::string hex;
            hex.reserve(job.data.size() * 2);
            for (auto b : job.data) {
                hex.push_back(hex_digits[(b >> 4) & 0xF]);
                hex.push_back(hex_digits[b & 0xF]);
            }

            char bufw[128];
            snprintf(bufw, sizeof(bufw),
                "{\"cmd\":\"write\",\"address\":%llu,\"data\":\"%s\"}",
                (unsigned long long)job.address,
                hex.c_str());
            cmd = bufw;
            SendWebSocketCommand(cmd, response, 100);
            break;
        }
        }
    }

    LOG("LEAVING WORKER THREAD");
}


bool StartWorker()
{
    bool expected = false;
    if (!g_workerRunning.compare_exchange_strong(expected, true))
        return false;

    g_workerThread = new std::thread(worker_thread);

    return g_workerThread != nullptr;
}


void StopWorker()
{
    bool expected = true;
    if (!g_workerRunning.compare_exchange_strong(expected, false))
        return;

    if (g_workerThread && g_workerThread->joinable())
        g_workerThread->join();

    delete g_workerThread;
    g_workerThread = nullptr;
}

