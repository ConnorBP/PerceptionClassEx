#include "Plugin.h"

#include <libwebsockets.h>
#include <thread>
#include <atomic>
#include <mutex>
#include <vector>
#include <string>
#include <windows.h>
#include "simdjson.h"


#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "Userenv.lib")
#pragma comment(lib, "Dbghelp.lib")
#pragma comment(lib, "Crypt32.lib")
#pragma comment(lib, "Iphlpapi.lib")

using namespace simdjson;

static std::thread g_wsThread;
static std::atomic<bool> g_wsRunning{ false };
static std::mutex g_procMutex;
static HANDLE g_attachedProcess = nullptr;
static DWORD g_attachedPid = 0;
static uintptr_t g_attachedBase = 0;

static int callback_ws(struct lws* wsi, enum lws_callback_reasons reason,
    void* user, void* in, size_t len)
{
    static std::string send_buffer;
    switch (reason) {
    case LWS_CALLBACK_ESTABLISHED:
        LOG("WebSocket connection established\n");
        break;
    case LWS_CALLBACK_CLOSED:
        LOG("WebSocket connection closed\n");
        break;
    case LWS_CALLBACK_RECEIVE: {
        ondemand::parser parser;
        std::string_view msg((const char*)in, len);
        ondemand::document doc;
        simdjson::padded_string padded(msg);
        //auto doc = parser.iterate(padded);
        auto error = parser.iterate(padded).get(doc);
        if (error) break;

        std::string cmd;
        // Extract "cmd" field
        {
            auto cmd_val = doc["cmd"];
            if (cmd_val.error() == SUCCESS) {
                cmd = std::string(cmd_val.get_string().value());
            }
        }

        std::string response;
        if (cmd == "attach") {
            // TODO: Implement process lookup by name, set g_attachedProcess, g_attachedPid, g_attachedBase
            response = R"({"error":"attach not implemented"})";
            LOG("Attach command received, but not implemented\n");
        }
        else if (cmd == "detach") {
            std::lock_guard<std::mutex> lock(g_procMutex);
            if (g_attachedProcess) {
                CloseHandle(g_attachedProcess);
                g_attachedProcess = nullptr;
                g_attachedPid = 0;
                g_attachedBase = 0;
            }
            response = R"({"result":"detached"})";
			LOG("Detached from process\n");
        }
        else if (cmd == "read") {
			LOGV("Read command received\n");
            std::lock_guard<std::mutex> lock(g_procMutex);
            if (!g_attachedProcess) {
                response = R"({"error":"not attached"})";
            }
            else {
                uint64_t addr = 0;
                uint32_t size = 0;
                auto addr_val = doc["address"];
                auto size_val = doc["size"];
                if (addr_val.error() == SUCCESS) addr = addr_val.get_uint64().value();
                if (size_val.error() == SUCCESS) size = uint32_t(size_val.get_uint64().value());
                if (size == 0 || size > 4096) {
                    response = R"({"error":"invalid size"})";
                }
                else {
                    std::vector<uint8_t> buffer(size);
                    SIZE_T bytesRead = 0;
                    if (ReadProcessMemory(g_attachedProcess, (LPCVOID)addr, buffer.data(), size, &bytesRead) && bytesRead == size) {
                        std::string hex;
                        for (auto b : buffer) {
                            char buf[3];
                            sprintf_s(buf, "%02X", b);
                            hex += buf;
                        }
                        response = "{\"data\":\"" + hex + "\"}";
                    }
                    else {
                        response = R"({"error":"read failed"})";
                    }
                }
            }
        }
        else if (cmd == "write") {
			LOGV("Write command received\n");
            std::lock_guard<std::mutex> lock(g_procMutex);
            if (!g_attachedProcess) {
                response = R"({"error":"not attached"})";
            }
            else {
                uint64_t addr = 0;
                std::string hex;
                auto addr_val = doc["address"];
                auto data_val = doc["data"];
                if (addr_val.error() == SUCCESS) addr = addr_val.get_uint64().value();
                if (data_val.error() == SUCCESS) hex = std::string(data_val.get_string().value());
                std::vector<uint8_t> bytes;
                for (size_t i = 0; i + 1 < hex.size(); i += 2) {
                    uint8_t byte = (uint8_t)strtoul(hex.substr(i, 2).c_str(), nullptr, 16);
                    bytes.push_back(byte);
                }
                SIZE_T bytesWritten = 0;
                if (WriteProcessMemory(g_attachedProcess, (LPVOID)addr, bytes.data(), bytes.size(), &bytesWritten) && bytesWritten == bytes.size()) {
                    response = R"({"result":"write ok"})";
                }
                else {
                    response = R"({"error":"write failed"})";
                }
            }
        }
        if (!response.empty()) {
            send_buffer = response;
            lws_callback_on_writable(wsi);
        }
        break;
    }
    case LWS_CALLBACK_SERVER_WRITEABLE: {
        if (!send_buffer.empty()) {
            std::vector<unsigned char> buf(LWS_PRE + send_buffer.size());
            memcpy(buf.data() + LWS_PRE, send_buffer.data(), send_buffer.size());
            lws_write(wsi, buf.data() + LWS_PRE, send_buffer.size(), LWS_WRITE_TEXT);
            send_buffer.clear();
        }
        break;
    }
    default:
        break;
    }
    return 0;
}

static struct lws_protocols protocols[] = {
    { "ws", callback_ws, 0, 4096, },
    { NULL, NULL, 0, 0 }
};

static void ws_server_thread() {
    struct lws_context_creation_info info = {};
    info.port = 9001;
    info.protocols = protocols;
    info.gid = -1;
    info.uid = -1;
    struct lws_context* context = lws_create_context(&info);
    if (!context) return;
    while (g_wsRunning) {
        lws_service(context, 50);
    }
    lws_context_destroy(context);
}

void StartWebSocketServer() {
    if (g_wsRunning.exchange(true)) return;
    g_wsThread = std::thread(ws_server_thread);
}

void StopWebSocketServer() {
    if (!g_wsRunning.exchange(false)) return;
    if (g_wsThread.joinable()) g_wsThread.join();
}