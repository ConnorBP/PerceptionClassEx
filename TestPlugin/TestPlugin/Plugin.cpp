// Plugin.cpp

#include <windows.h>
#include <psapi.h>
#include <string>
#include <codecvt>

#include "Plugin.h"
#include "WebSocketServer.hpp"
#include "resource.h"
#include "ReadCache.hpp"

BOOL gTestPluginState = FALSE;

void StartWebSocketServer();
void StopWebSocketServer();

std::wstring GetProcessNameFromPid(DWORD dwProcessId);

HANDLE PLUGIN_CC MyOpenProcess(DWORD dwDesiredAccess, BOOL, DWORD dwProcessId)
{
    // Log for debugging
    ReClassPrintConsole(L"[WSMem] MyOpenProcess called for pid %u, access 0x%08X",
        dwProcessId, dwDesiredAccess);

    // Detect process browser enumeration
    bool isProbe =
        (dwDesiredAccess == (PROCESS_QUERY_INFORMATION | PROCESS_VM_READ)) ||
        (dwDesiredAccess == PROCESS_QUERY_LIMITED_INFORMATION);

    if (isProbe) {
        // Return a fake handle but DO NOT enqueue a job
        return (HANDLE)1;
    }

    // Real attach request
    Job job;
    job.type = JobType::OpenProcess;
    job.pid = dwProcessId;

    {
        std::lock_guard<std::mutex> lock(g_jobs_mutex);
        g_jobs.push(std::move(job));
    }
    g_jobs_cv.notify_one();

    return (HANDLE)1;
}


BOOL PLUGIN_CC MyCloseProcess(HANDLE)
{
    Job job;
    job.type = JobType::CloseProcess;
    {
        std::lock_guard<std::mutex> lock(g_jobs_mutex);
        g_jobs.push(std::move(job));
    }
    g_jobs_cv.notify_one();
    return TRUE;
}


BOOL 
PLUGIN_CC 
PluginInit( 
    OUT LPRECLASS_PLUGIN_INFO lpRCInfo 
)
{
    wcscpy_s( lpRCInfo->Name, L"Websocket Memory" );
    wcscpy_s( lpRCInfo->Version, L"1.0.0.0" );
    wcscpy_s( lpRCInfo->About, L"Access your memory using websockets" );
    lpRCInfo->DialogId = IDD_SETTINGS_DLG;

    if (!ReClassIsReadMemoryOverriden( ) && !ReClassIsWriteMemoryOverriden( ))
    {
        if (ReClassOverrideMemoryOperations( ReadCallback, WriteCallback ) == FALSE)
        {
            ReClassPrintConsole( L"[WSMem] Failed to register r/w callbacks, failed Plugin Init" );
            return FALSE;
        }
    }

    if (!ReClassOverrideOpenProcessOperation(MyOpenProcess)) {
        return FALSE;
        // Optionally: ReClassOverrideHandleOperations(MyOpenProcess, MyOpenThread);
    }


    //if (!StartWorker()) {
    //    return FALSE;
    //}


    gTestPluginState = TRUE;

    return TRUE;
}

VOID 
PLUGIN_CC 
PluginStateChange( 
    IN BOOL State 
)
{
    //
    // Update global state variable
    //
    gTestPluginState = State;


    if (State)
    {
        LOG( L"[WSMem] Enabled!" );



		// start the server
        StartWebSocketServer(); // sets g_wsRunning, creates g_context
        StartWorker(); // afterwards, now SendWebSocketCommand can succeed 
    }
    else
    {
        LOG( L"[WSMem] Disabled!" );

		// stop the worker
		StopWorker();

		// stop the server
        StopWebSocketServer();

        //
        // Remove our overrides if we're disabling/disabled.
        //
        if (ReClassGetCurrentReadMemory( ) == &ReadCallback)
            ReClassRemoveReadMemoryOverride( );

        if (ReClassGetCurrentWriteMemory( ) == &WriteCallback)
            ReClassRemoveWriteMemoryOverride( );
    }
}

INT_PTR 
PLUGIN_CC 
PluginSettingsDlg( 
    IN HWND hWnd, 
    IN UINT Msg, 
    IN WPARAM wParam, 
    IN LPARAM lParam 
)
{
    switch (Msg)
    {

    case WM_INITDIALOG:
    {
        if (gTestPluginState)
        {
            //
            // Apply checkboxes appropriately if we're in anm enabled state.
            //
            BOOL ReadChecked = (ReClassGetCurrentReadMemory( ) == &ReadCallback) ? BST_CHECKED : BST_UNCHECKED;
            BOOL WriteChecked = (ReClassGetCurrentWriteMemory( ) == &WriteCallback) ? BST_CHECKED : BST_UNCHECKED;

            SendMessage( GetDlgItem( hWnd, IDC_CHECK_READ_MEMORY_OVERRIDE ), BM_SETCHECK, MAKEWPARAM( ReadChecked, 0 ), 0 );
            EnableWindow( GetDlgItem( hWnd, IDC_CHECK_READ_MEMORY_OVERRIDE ), TRUE );

            SendMessage( GetDlgItem( hWnd, IDC_CHECK_WRITE_MEMORY_OVERRIDE ), BM_SETCHECK, MAKEWPARAM( WriteChecked, 0 ), 0 );
            EnableWindow( GetDlgItem( hWnd, IDC_CHECK_WRITE_MEMORY_OVERRIDE ), TRUE );
        }
        else
        {
            //
            // Make sure we can't touch the settings if we're in a disabled state.
            //
            SendMessage( GetDlgItem( hWnd, IDC_CHECK_READ_MEMORY_OVERRIDE ), BM_SETCHECK, MAKEWPARAM( BST_UNCHECKED, 0 ), 0 );
            EnableWindow( GetDlgItem( hWnd, IDC_CHECK_READ_MEMORY_OVERRIDE ), FALSE );

            SendMessage( GetDlgItem( hWnd, IDC_CHECK_WRITE_MEMORY_OVERRIDE ), BM_SETCHECK, MAKEWPARAM( BST_UNCHECKED, 0 ), 0 );
            EnableWindow( GetDlgItem( hWnd, IDC_CHECK_WRITE_MEMORY_OVERRIDE ), FALSE );
        }
    }
    return TRUE;

    case WM_COMMAND:
    {
        WORD NotificationCode = HIWORD( wParam );
        WORD ControlId = LOWORD( wParam );
        HWND hControlWnd = (HWND)lParam;
        
        if (NotificationCode == BN_CLICKED)
        {
            BOOLEAN bChecked = (SendMessage( hControlWnd, BM_GETCHECK, 0, 0 ) == BST_CHECKED);

            if (ControlId == IDC_CHECK_READ_MEMORY_OVERRIDE)
            {
                if (bChecked)
                {
                    //
                    // Make sure the read memory operation is not already overriden.
                    //
                    if (!ReClassIsReadMemoryOverriden( ))
                    {
                        ReClassOverrideReadMemoryOperation( &ReadCallback );
                    }
                    else
                    {
                        //
                        // Make sure it's not us!
                        //
                        if (ReClassGetCurrentReadMemory( ) != &ReadCallback)
                        {
                            //
                            // Ask the user whether or not they want to overwrite the other operation.
                            //
                            if (MessageBoxW( ReClassMainWindow( ),
                                L"Another plugin has already overriden the read operation.\n"
                                L"Would you like to overwrite their read override?",
                                L"Test Plugin", MB_YESNO ) == IDYES)
                            {
                                ReClassOverrideReadMemoryOperation( &ReadCallback );
                            }
                            else
                            {
                                //
                                // If the user chose no, then make sure our checkbox is unchecked.
                                //
                                SendMessage( GetDlgItem( hWnd, IDC_CHECK_READ_MEMORY_OVERRIDE ), 
                                    BM_SETCHECK, MAKEWPARAM( BST_UNCHECKED, 0 ), 0 );
                            }
                        }
                        else
                        {
                            //
                            // This shouldn't happen!
                            //
                            MessageBoxW( ReClassMainWindow( ), 
                                L"WTF! Plugin memory read operation is already set as the active override!", 
                                L"Test Plugin", MB_ICONERROR );
                        }
                    }
                }
                else
                {
                    //
                    // Only remove the read memory operation if it's ours!
                    //
                    if (ReClassGetCurrentReadMemory( ) == &ReadCallback)
                    {
                        ReClassRemoveReadMemoryOverride( );
                    }
                }			
            }
            else if (ControlId == IDC_CHECK_WRITE_MEMORY_OVERRIDE)
            {
                if (bChecked)
                {
                    //
                    // Make sure the write memory operation is not already overriden.
                    //
                    if (!ReClassIsWriteMemoryOverriden( ))
                    {
                        //
                        // We're all good to set our write memory operation!
                        //
                        ReClassOverrideWriteMemoryOperation( &WriteCallback );
                    }
                    else
                    {
                        //
                        // Make sure it's not us!
                        //
                        if (ReClassGetCurrentWriteMemory( ) != &WriteCallback)
                        {
                            //
                            // Ask the user whether or not they want to overwrite the other operation.
                            //
                            if (MessageBoxW( ReClassMainWindow( ),
                                L"Another plugin has already overriden the write operation.\n"
                                L"Would you like to overwrite their write override?",
                                L"Test Plugin", MB_YESNO ) == IDYES)
                            {
                                ReClassOverrideWriteMemoryOperation( &WriteCallback );
                            }
                            else
                            {
                                //
                                // If the user chose no, then make sure our checkbox is unchecked.
                                //
                                SendMessage( GetDlgItem( hWnd, IDC_CHECK_WRITE_MEMORY_OVERRIDE ),
                                    BM_SETCHECK, MAKEWPARAM( BST_UNCHECKED, 0 ), 0 );
                            }
                        }
                        else
                        {
                            //
                            // This shouldn't happen!
                            //
                            MessageBoxW( ReClassMainWindow( ),
                                L"WTF! Plugin memory write operation is already set as the active override!",
                                L"Test Plugin", MB_ICONERROR );
                        }
                    }
                }
                else
                {
                    //
                    // Only remove the read memory operation if it's ours!
                    //
                    if (ReClassGetCurrentWriteMemory( ) == &WriteCallback)
                    {
                        ReClassRemoveWriteMemoryOverride( );
                    }
                }
            }
        }	
    }
    break;

    case WM_CLOSE:
    {
        EndDialog( hWnd, 0 );
    }
    break;

    }
    return FALSE;
}

BOOL
PLUGIN_CC ReadCallback(
    IN LPVOID Address,
    IN LPVOID Buffer,
    IN SIZE_T Size,
    OUT PSIZE_T BytesRead
)
{
    uintptr_t addr = (uintptr_t)Address;

    {
        std::shared_lock<std::shared_mutex> lock(g_cache_mutex); // read only lock
        ReadKey key{ addr, Size };
        auto it = g_cache.find(key);
        if (it != g_cache.end() && it->second.data.size() == Size) {
            memcpy(Buffer, it->second.data.data(), Size);
            if (BytesRead) *BytesRead = Size;
            return TRUE;
        }
    }

    // Cache miss: enqueue async read, return zeros
    {
        Job job;
        job.type = JobType::Read;
        job.address = addr;
        job.data.resize(Size);

        {
            std::lock_guard<std::mutex> lock(g_jobs_mutex);
            g_jobs.push(std::move(job));
        }
        g_jobs_cv.notify_one();
    }

    ZeroMemory(Buffer, Size);
    if (BytesRead) *BytesRead = Size;
    return TRUE;
}


BOOL
PLUGIN_CC
WriteCallback(
    IN LPVOID Address,
    IN LPVOID Buffer,
    IN SIZE_T Size,
    OUT PSIZE_T BytesWritten
)
{
    uintptr_t addr = (uintptr_t)Address;

    // Update cache optimistically
    {
		std::unique_lock<std::shared_mutex> lock(g_cache_mutex); // write enabled lock
        ReadKey key{ addr, Size };
        CachedBlock block;
        block.data.assign((uint8_t*)Buffer, (uint8_t*)Buffer + Size);
        g_cache[key] = std::move(block);
    }

    // Enqueue async write
    {
        Job job;
        job.type = JobType::Write;
        job.address = addr;
        job.data.assign((uint8_t*)Buffer, (uint8_t*)Buffer + Size);

        {
            std::lock_guard<std::mutex> lock(g_jobs_mutex);
            g_jobs.push(std::move(job));
        }
        g_jobs_cv.notify_one();
    }

    if (BytesWritten) *BytesWritten = Size;
    return TRUE;
}


// Helper function to get process name from PID
std::wstring GetProcessNameFromPid(DWORD dwProcessId)
{
    std::wstring processName;
    HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, dwProcessId);
    if (hProcess)
    {
        WCHAR szProcessName[MAX_PATH] = L"<unknown>";
        if (GetModuleBaseNameW(hProcess, NULL, szProcessName, MAX_PATH))
        {
            processName = szProcessName;
        }
        CloseHandle(hProcess);
    }
    else
    {
        processName = L"<unknown>";
    }
    return processName;
}
