// WebSocketServer.cpp (rewritten, non-blocking for UI thread)

#include "Plugin.h"
#include "WebSocketServer.hpp"

#include <libwebsockets.h>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <vector>
#include <string>
#include <map>
#include <windows.h>

// Link libraries as before
#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "Userenv.lib")
#pragma comment(lib, "Dbghelp.lib")
#pragma comment(lib, "Crypt32.lib")
#pragma comment(lib, "Iphlpapi.lib")

// Simple logging macro placeholder (replace if you already have one)
#ifndef LOG
#define LOG(fmt, ...) ReClassPrintConsole(L"[WSMem] " L##fmt, __VA_ARGS__)
#endif

// ---------------------------------------------
// Per-client state
// ---------------------------------------------

struct ClientState {
    // Protects everything below
    std::mutex mtx;

    // Queue of pending commands to send to this client
    std::queue<std::string> outgoing;

    // Synchronous single-response waiter
    // (ReClass only ever has one in-flight SendWebSocketCommand at a time)
    std::string last_response;
    bool waiting_for_response = false;
    //bool needs_write = false;
    std::condition_variable cv;
};

static std::map<struct lws*, std::shared_ptr<ClientState>> g_clients;
static std::mutex g_clients_mutex;

// libwebsockets context + thread
static std::thread* g_wsThread = nullptr;

static std::atomic<bool> g_wsRunning{ false };
static struct lws_context* g_context = nullptr;

// ---------------------------------------------
// libwebsockets protocol callback
// ---------------------------------------------

static int callback_ws(struct lws* wsi,
    enum lws_callback_reasons reason,
    void* user,
    void* in,
    size_t len)
{
    switch (reason) {

    case LWS_CALLBACK_ESTABLISHED:
    {
        LOG(L"WebSocket client connected\n");
        auto state = std::make_shared<ClientState>();

        {
            std::lock_guard<std::mutex> lock(g_clients_mutex);
            g_clients[wsi] = state;
        }
        break;
    }

    case LWS_CALLBACK_CLOSED:
    {
        LOG(L"WebSocket client disconnected\n");
        std::shared_ptr<ClientState> state;

        {
            std::lock_guard<std::mutex> lock(g_clients_mutex);
            auto it = g_clients.find(wsi);
            if (it != g_clients.end()) {
                state = it->second;
                g_clients.erase(it);
            }
        }

        if (state) {
            std::unique_lock<std::mutex> lock(state->mtx);
            // If someone is waiting on this client, wake them with empty response
            if (state->waiting_for_response) {
                state->waiting_for_response = false;
                state->last_response.clear();
                state->cv.notify_all();
            }
        }

        break;
    }

    case LWS_CALLBACK_RECEIVE:
    {
        std::shared_ptr<ClientState> state;

        {
            std::lock_guard<std::mutex> lock(g_clients_mutex);
            auto it = g_clients.find(wsi);
            if (it != g_clients.end())
                state = it->second;
        }

        if (state) {
            std::unique_lock<std::mutex> lock(state->mtx);
            state->last_response.assign(static_cast<const char*>(in), len);

            if (state->waiting_for_response) {
                state->waiting_for_response = false;
                state->cv.notify_all();
            }
        }

        break;
    }

    case LWS_CALLBACK_SERVER_WRITEABLE:
    {
        //LOGV(L"WRITEABLE FIRED");
        std::shared_ptr<ClientState> state;

        {
            std::lock_guard<std::mutex> lock(g_clients_mutex);
            auto it = g_clients.find(wsi);
            if (it != g_clients.end())
                state = it->second;
        }

        if (!state)
            break;

        std::string cmd;

        {
            std::lock_guard<std::mutex> lock(state->mtx);
            if (!state->outgoing.empty()) {
                cmd = std::move(state->outgoing.front());
                state->outgoing.pop();
            }
        }

        if (!cmd.empty()) {
            // libwebsockets requires LWS_PRE padding
            std::vector<unsigned char> buf(LWS_PRE + cmd.size());
            memcpy(buf.data() + LWS_PRE, cmd.data(), cmd.size());
            lws_write(wsi, buf.data() + LWS_PRE, cmd.size(), LWS_WRITE_TEXT);

    //        // If there are more messages queued, ask to be writeable again
    //        bool has_more = false;
    //        {
    //            std::lock_guard<std::mutex> lock(state->mtx);
    //            has_more = !state->outgoing.empty();
    //        }
    //        if (has_more) {
    //            //lws_callback_on_writable(wsi); // this CANNOT be called from the UI thread, no matter what.
				//state->needs_write = true;
    //        }
        }

        break;
    }

    default:
        break;
    }

    return 0;
}

// ---------------------------------------------
// Protocol list
// ---------------------------------------------

static struct lws_protocols protocols[] = {
    { "ws", callback_ws, 0, 4096, },
    { nullptr, nullptr, 0, 0 }
};

// ---------------------------------------------
// WebSocket server thread
// ---------------------------------------------

static void ws_server_thread()
{
    struct lws_context_creation_info info;
    memset(&info, 0, sizeof(info));

    info.port = 9001;
    info.protocols = protocols;
    info.gid = -1;
    info.uid = -1;

    g_context = lws_create_context(&info);
    if (!g_context) {
        LOG(L"Failed to create libwebsockets context\n");
        g_wsRunning = false;
        return;
    }

    LOG(L"WebSocket server started on port 9001\n");

    while (g_wsRunning) {
        // 10ms service timeout keeps latency low
        lws_service(g_context, 1);
		// After waking up, check if any clients need to be marked writeable
        {
            std::lock_guard<std::mutex> lock(g_clients_mutex);
            for (auto& kv : g_clients) {
                auto wsi = kv.first;
                auto state = kv.second;
                std::lock_guard<std::mutex> lock2(state->mtx);
                if (!state->outgoing.empty()) {
                    lws_callback_on_writable(wsi);
                }
            }
        }

    }

    // Cleanup
    lws_context_destroy(g_context);
    g_context = nullptr;

    LOG(L"WebSocket server stopped\n");
}

// ---------------------------------------------
// Public control functions
// ---------------------------------------------
void StartWebSocketServer()
{
    bool expected = false;
    if (!g_wsRunning.compare_exchange_strong(expected, true)) {
        return; // already running
    }

    g_wsThread = new std::thread(ws_server_thread);
}

void StopWebSocketServer()
{
    bool expected = true;
    if (!g_wsRunning.compare_exchange_strong(expected, false)) {
        return; // not running
    }

    if (g_context) {
        lws_cancel_service(g_context);
    }

    if (g_wsThread && g_wsThread->joinable()) {
        g_wsThread->join();
    }

    delete g_wsThread;
    g_wsThread = nullptr;

    // Clean up clients
    {
        std::lock_guard<std::mutex> lock(g_clients_mutex);
        for (auto& kv : g_clients) {
            auto& state = kv.second;
            if (state) {
                std::unique_lock<std::mutex> lock2(state->mtx);
                if (state->waiting_for_response) {
                    state->waiting_for_response = false;
                    state->last_response.clear();
                    state->cv.notify_all();
                }
            }
        }
        g_clients.clear();
    }
}


// ---------------------------------------------
// Command API used by Plugin.cpp
// ---------------------------------------------

// This function is called from ReClass (UI thread) and MUST NOT call any
// libwebsockets APIs directly. It only:
//   - chooses a client
//   - enqueues a command
//   - waits on a condition_variable for a response.
bool SendWebSocketCommand(const std::string& json,
    std::string& response,
    int timeoutMs)
{

    // instrumentation to monitor performance
    LARGE_INTEGER freq, t0, t1; QueryPerformanceFrequency(&freq); QueryPerformanceCounter(&t0);


    response.clear();

    if (!g_wsRunning || !g_context)
        return false;

    std::shared_ptr<ClientState> target_state;
    struct lws* target_wsi = nullptr;

    {
        std::lock_guard<std::mutex> lock(g_clients_mutex);
        if (g_clients.empty())
            return false;

        // Pick the first connected client for now
        auto it = g_clients.begin();
        target_wsi = it->first;
        target_state = it->second;
    }

    if (!target_state || !target_wsi)
        return false;

    {
        std::unique_lock<std::mutex> lock(target_state->mtx);

        // If someone else is already waiting, we either:
        // - block until it's done (simple), or
        // - fail immediately.
        // For now, we fail immediately to avoid making things worse.
        if (target_state->waiting_for_response) {
            return false;
        }

        // Queue outgoing command
        target_state->outgoing.push(json);

        // Mark that we're waiting for a response
        target_state->waiting_for_response = true;
        target_state->last_response.clear();
    }

    // Wake up the websocket thread so it will schedule writable
    lws_cancel_service(g_context);

    // Also ask libwebsockets (from its own thread) to call SERVER_WRITEABLE.
    // We can't call lws_callback_on_writable from here (wrong thread), so we
    // rely on the server thread to do it: after it wakes up, it will see
    // queued messages and mark wsi writeable via callback.
    //
    // To implement that, we don't have to do anything extra here:
    // - The server thread wakes due to lws_cancel_service
    // - libwebsockets will call our callback with SERVER_WRITEABLE
    //   when it’s allowed to write.
    //
    // If you want to be more aggressive, you can keep a flag per-client to
    // call lws_callback_on_writable(wsi) *inside* the server thread.

    // Now wait for the response
    bool result = false;
    {
        std::unique_lock<std::mutex> lock(target_state->mtx);

        bool ok = target_state->cv.wait_for(
            lock,
            std::chrono::milliseconds(timeoutMs),
            [&] { return !target_state->waiting_for_response; }
        );

        if (ok) {
            response = target_state->last_response;
            result = !response.empty();
        } else {
            // timeout
            target_state->waiting_for_response = false;
            target_state->last_response.clear();
            result= false;
        }

    }

    // output insrumentation
    QueryPerformanceCounter(&t1);
    double ms = double(t1.QuadPart - t0.QuadPart) * 1000.0 / double(freq.QuadPart);
    wchar_t buf[256];
    swprintf_s(buf, L"[WSMem] SendWebSocketCommand took %.2f ms\n", ms);
    ReClassPrintConsole(buf);

    return result;
}
