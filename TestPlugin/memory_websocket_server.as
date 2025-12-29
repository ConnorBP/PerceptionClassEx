// WebSocket Memory Server for Perception.cx
// Safe, callback-correct, engine-compliant implementation

// ------------------------------------------------------------
// Globals (value types, never compared to null)
// ------------------------------------------------------------
ws_t   g_ws;
proc_t g_proc;
bool attached = false;
int    g_callback_id = 0;

// ------------------------------------------------------------
// Networking helpers
// ------------------------------------------------------------
void send_json_response(dictionary &in response)
{
    if (!g_ws.is_open())
    return;
    string json, err;
    if (json_stringify(response, json, err)) {
        //log_console("WS: sending response: " + json);
        g_ws.send_text(json);
    } else {
        log_error("JSON stringify failed: " + err);
    }
}


// ------------------------------------------------------------
// Command handlers
// ------------------------------------------------------------
void handle_open_process(dictionary &in req)
{
    dictionary res;
    
    double pid_d;
    if (!req.get("pid", pid_d))
    {
        res.set("error", "missing pid");
        send_json_response(res);
        return;
    }
    
    uint pid = uint(pid_d);
    log_console("attaching to pid: " + pid);
    
    if (g_proc.alive())
    g_proc.deref();
    
    g_proc = ref_process(pid);
    if (!g_proc.alive())
    {
        attached = false;
        res.set("error", "attach failed");
    }
    else
    {
        attached = true;
        res.set("result", "attached");
        res.set("pid", double(g_proc.pid()));
        res.set("base", double(g_proc.base_address()));
    }
    send_json_response(res);
}


void handle_close_process()
{
    if (g_proc.alive())
    g_proc.deref();
    g_proc = proc_t();
    dictionary res;
    res.set("result", "detached");
    send_json_response(res);
}

void handle_read(dictionary &in req)
{
    dictionary res;
    if (!attached || !g_proc.alive())
    {
        attached = false;
        res.set("error", "not attached");
        send_json_response(res);
        return;
    }
    double addr_d, size_d;
    if (!req.get("address", addr_d) || !req.get("size", size_d))
    {
        res.set("error", "missing address/size");
        send_json_response(res);
        return;
    }
    uint64 addr = uint64(addr_d);
    uint size = uint(size_d);
    if (size == 0 || size > 4096)
    {
        res.set("error", "invalid size");
        send_json_response(res);
        return;
    }
    array<uint8> data;
    g_proc.rvm(addr, size, data);
    if (data.length() != size)
    {
        res.set("error", "read failed");
        send_json_response(res);
        return;
    }
    // Convert array<uint8> → string for util_hex_encode
    string raw;
    raw.resize(data.length());
    for (uint i = 0; i < data.length(); i++)
    raw[i] = uint8(data[i]);
    res.set("data", util_hex_encode(raw));
    send_json_response(res);
}

void handle_write(dictionary &in req)
{
    dictionary res;
    if (!attached || !g_proc.alive())
    {
        attached = false;
        res.set("error", "not attached");
        send_json_response(res);
        return;
    }
    double addr_d;
    string hex;
    if (!req.get("address", addr_d) || !req.get("data", hex))
    {
        res.set("error", "missing fields");
        send_json_response(res);
        return;
    }
    string raw;
    string err;
    if (!util_hex_decode(hex, raw, err))
    {
        res.set("error", "invalid hex: " + err);
        send_json_response(res);
        return;
    }
    // Convert string → array<uint8>
    array<uint8> bytes;
    bytes.resize(raw.length());
    for (uint i = 0; i < raw.length(); i++)
    bytes[i] = uint8(raw[i]);
    if (g_proc.wvm(uint64(addr_d), bytes))
    res.set("result", "write ok");
    else
    res.set("error", "write failed");
    send_json_response(res);
}

void handle_request(dictionary &in req)
{
    string cmd;
    if (!req.get("cmd", cmd))
    return;
    if      (cmd == "open_process")  handle_open_process(req);
    else if (cmd == "close_process") handle_close_process();
    else if (cmd == "read")          handle_read(req);
    else if (cmd == "write")         handle_write(req);
}

// ------------------------------------------------------------
// WebSocket pump (callback-safe)
// ------------------------------------------------------------
void websocket_pump()
{
    if (!g_ws.is_open())
    return;
    
    string msg;
    bool is_text = false;
    bool is_closed = false;
    
    while (g_ws.poll(msg, is_text, is_closed))
    {
        if (is_text)
        {
            
            //log_console("WS: received text: " + msg);
            dictionary req;
            string err;
            if (json_parse(msg, req, err))
            handle_request(req);
        }
        if (is_closed)
        break;
    }
    
    if (is_closed)
    {
        if (g_proc.alive())
        g_proc.deref();
        g_proc = proc_t();
        g_ws.close();
        g_ws = ws_t();
    }
}


void websocket_callback(int, int)
{
    websocket_pump();
}

// ------------------------------------------------------------
// Entry / Exit
// ------------------------------------------------------------
int main()
{
    log_console("Starting WebSocket memory server");
    g_ws = ws_connect("ws://127.0.0.1:9001/memory", 5000);
    if (!g_ws.is_open())
    {
        log_error("WebSocket connect failed");
        return -1;
    }
    g_callback_id = register_callback(websocket_callback, 1, 0);
    if (g_callback_id == 0)
    {
        g_ws.close();
        g_ws = ws_t();
        return -1;
    }
    log_console("Server running");
    return 1;
}

void on_unload()
{
    if (g_callback_id != 0)
    unregister_callback(g_callback_id);
    if (g_ws.is_open())
    g_ws.close();
    g_ws = ws_t();
    if (g_proc.alive())
    g_proc.deref();
    g_proc = proc_t();
    log_console("Server unloaded");
}
