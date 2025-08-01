/**
 * ==================================================
 *   _____ _ _ _             _                     
 *  |     |_| | |___ ___ ___|_|_ _ _____           
 *  | | | | | | | -_|   |   | | | |     |          
 *  |_|_|_|_|_|_|___|_|_|_|_|_|___|_|_|_|          
 * 
 * ==================================================
 * 
 * Copyright (c) 2025 Project Millennium
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "http.h"
#include <thread>
#include <nlohmann/json.hpp>
#include <filesystem>

#ifdef _WIN32
#include <winsock2.h>
#include <iphlpapi.h>
#include <tchar.h>
#pragma comment(lib, "iphlpapi.lib")
#include <tlhelp32.h>
#include <psapi.h>
#elif __linux__
#include <fstream>
#include <sstream>
#elif __APPLE__
#include <cstdlib>
#include <cstdio>
#endif
#include "cmd.h"


static bool bHasCheckedConnection = false;

class SocketHelpers
{
private:
    u_short debuggerPort;

    const u_short GetDebuggerPort()
    {
        #ifdef _WIN32
        {
            std::unique_ptr<StartupParameters> startupParams = std::make_unique<StartupParameters>();
            return startupParams->GetRemoteDebuggerPort();
        }
        #endif
        return 8080;
    }

    const std::string GetDebuggerUrl()
    {
        return fmt::format("http://127.0.0.1:{}", debuggerPort);
    }

    struct SteamConnectionProps
    {
        bool hasConnection;
        std::string processName = {};
    };

#ifdef _WIN32
    std::string GetProcessName(DWORD processID) 
    {
        char Buffer[MAX_PATH];
        HANDLE Handle = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, processID);

        if (Handle)
        {
            if (GetModuleFileNameExA(Handle, 0, Buffer, MAX_PATH))
            {
                return std::string(Buffer);
            }
            CloseHandle(Handle);
        }

        return {};
    }
#endif

    SteamConnectionProps GetSteamConnectionProps()
    {
#ifdef _WIN32
        // Windows-specific code
        DWORD size = 0;
        PMIB_TCPTABLE2 tcpTable;
        GetTcpTable2(nullptr, &size, TRUE);
        tcpTable = (PMIB_TCPTABLE2)malloc(size);

        if (GetTcpTable2(tcpTable, &size, TRUE) != NO_ERROR) 
        {
            free(tcpTable);
            return { 
                false,
                "Error getting TCP table"
            };
        }

        for (DWORD i = 0; i < tcpTable->dwNumEntries; i++)
        {
            if (ntohs((u_short)tcpTable->table[i].dwLocalPort) == debuggerPort)
            {
                const auto targetProcess = std::filesystem::path(GetProcessName(tcpTable->table[i].dwOwningPid));

                if (targetProcess.string().empty())
                {
                    // Skip if we can't get the process name
                    continue;
                }

                return { 
                    targetProcess.filename().string() == "steamwebhelper.exe", 
                    targetProcess.string()
                };
            }
        }
#endif
        return { true };
    }

public:
    struct ConnectSocketProps
    {
        std::string commonName;
        std::function<std::string()> fetchSocketUrl;

        std::function<void(
            websocketpp::client<websocketpp::config::asio_client>*,
            websocketpp::connection_hdl)> onConnect;

        std::function<void(
            websocketpp::client<websocketpp::config::asio_client>*,
            websocketpp::connection_hdl,
            std::shared_ptr<websocketpp::config::core_client::message_type>)> onMessage;
    };

    /**
     * @brief Verify the socket connection was opened by the Steam Client 
     * (i.e owned by Steam), and if not report a faulty connection.
     */
    const void VerifySteamConnection()
    {
        if (bHasCheckedConnection)
        {
            return;
        }

        auto [canConnect, processName] = this->GetSteamConnectionProps();
        if (!canConnect)
        {
            const std::string message = fmt::format(
                "Millennium can't connect to Steam because the target port '{}' is currently being used by '{}'.\n"
                "To address this you must uninstall/close the conflicting app, change the port it uses (assuming its possible), or uninstall Millennium.\n\n"
                "Millennium & Steam will now close until further action is taken.", debuggerPort, processName
            );

            #ifdef _WIN32
            MessageBoxA(nullptr, message.c_str(), "Fatal Error", MB_ICONERROR);
            #endif
            Logger.Warn(message);
            std::exit(1);
        }

        bHasCheckedConnection = true;
    }

    SocketHelpers() : debuggerPort(GetDebuggerPort())
    {
        Logger.Log("Opting to use '{}' for SteamDBG port", debuggerPort);
        this->VerifySteamConnection();
    }

    /**
     * @brief Get the Steam browser context.
     * It can locally be accessed at localhost:%PORT%/json/version
     * 
     * @return std::string The Steam browser context.
     */
    const std::string GetSteamBrowserContext()
    {
        try
        {
            std::string browserUrl = fmt::format("{}/json/version", this->GetDebuggerUrl());
            nlohmann::basic_json<> instance = nlohmann::json::parse(Http::Get(browserUrl.c_str()));

            return instance["webSocketDebuggerUrl"];
        }
        catch (nlohmann::detail::exception& exception)
        {
            LOG_ERROR("An error occurred while making a connection to Steam browser context. It's likely that the debugger port '{}' is in use by another process. exception -> {}", debuggerPort, exception.what());
            std::exit(1);
        }
    }

    void ConnectSocket(ConnectSocketProps socketProps)
    {
        std::string socketUrl;
        websocketpp::client<websocketpp::config::asio_client> socketClient;

        const auto [commonName, fetchSocketUrl, onConnect, onMessage] = socketProps;
 
        try
        {
            socketUrl = fetchSocketUrl();
        }
        /** The request was broke early before it was received. Likely because Millennium is shutting down. */
        catch (HttpError& exception)
        {
            Logger.Warn("Failed to get Steam browser context: {}", exception.GetMessage());
            return;
        }

        if (socketUrl.empty())
        {
            LOG_ERROR("[{}] Socket URL is empty. Aborting connection.", commonName);
            return;
        }
    
        try
        {
            socketClient.set_access_channels(websocketpp::log::alevel::none);
            socketClient.clear_error_channels(websocketpp::log::elevel::none);
            socketClient.set_error_channels(websocketpp::log::elevel::all);
    
            socketClient.init_asio();
    
            // Validate handlers before binding
            if (!onConnect || !onMessage)
            {
                LOG_ERROR("[{}] Invalid event handlers. Connection aborted.", commonName);
                return;
            }
    
            socketClient.set_open_handler(bind(onConnect, &socketClient, std::placeholders::_1));
            socketClient.set_message_handler(bind(onMessage, &socketClient, std::placeholders::_1, std::placeholders::_2));

            websocketpp::lib::error_code errorCode;
            auto con = socketClient.get_connection(socketUrl, errorCode);

            if (errorCode)
            {
                LOG_ERROR("[{}] Failed to establish connection: {} [{}]", commonName, errorCode.message(), errorCode.value());
                return;
            }
    
            socketClient.connect(con);
            socketClient.run();
        }
        catch (const websocketpp::exception& ex)
        {
            LOG_ERROR("[{}] WebSocket exception thrown -> {}", commonName, ex.what());
        }
        catch (const std::exception& ex)
        {
            LOG_ERROR("[{}] Standard exception caught -> {}", commonName, ex.what());
        }
        catch (...)
        {
            LOG_ERROR("[{}] Unknown exception caught.", commonName);
        }
    
        Logger.Log("Disconnected from [{}] module...", commonName);
    }
};