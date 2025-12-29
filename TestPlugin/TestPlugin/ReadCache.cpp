#include "ReadCache.hpp"
#include "WebSocketServer.hpp"


void worker_thread()
{
    g_workerRunning = true;
    while (g_workerRunning) {
        Job job;

        // Wait for jobs
        {
            std::unique_lock<std::mutex> lock(g_jobs_mutex);
            g_jobs_cv.wait(lock, [] {
                return !g_workerRunning || !g_jobs.empty();
                });
            if (!g_workerRunning)
                break;
            job = std::move(g_jobs.front());
            g_jobs.pop();
        }

        std::string cmd, response;

        switch (job.type) {

        case JobType::OpenProcess: {

            cmd = "{\"cmd\":\"open_process\",\"process\":\"" + job.processName + "\"}";
			LOG("SENDING %s\n", cmd.c_str());
            if (SendWebSocketCommand(cmd, response, 1000)) {
                // optional: log / parse result
				LOG(L"Opened process: %S\n", job.processName.c_str());
            }
            else {
				LOG("Failed to open process: %s\n", job.processName.c_str());
            }
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
                // parse "data":"HEX"
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
                            std::lock_guard<std::mutex> lock(g_cache_mutex);
                            g_cache[key] = std::move(block);
                        }
                    }
                }
            }
            break;
        }

        case JobType::Write: {
            // encode job.data as hex
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

            // We might update cache optimistically here as well.
            break;
        }
        }
    }
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

    {
        std::lock_guard<std::mutex> lock(g_jobs_mutex);
        // wake the worker
        g_jobs_cv.notify_all();
    }

    if (g_workerThread && g_workerThread->joinable())
        g_workerThread->join();

    delete g_workerThread;
    g_workerThread = nullptr;
}

