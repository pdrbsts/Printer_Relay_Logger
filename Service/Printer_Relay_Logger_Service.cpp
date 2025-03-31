#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#define NOMINMAX

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winsvc.h>
#include <tchar.h>

#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <chrono>
#include <iomanip>
#include <filesystem>
#include <atomic>
#include <sstream>
#include <cstdlib>
#include <cstdio>
#include <algorithm>
#include <sstream>

#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "Advapi32.lib")

#define SERVICE_NAME L"PrinterRelayLogger"
#define DISPLAY_NAME L"Printer Relay Logger Service"
#define SERVICE_DESCRIPTION L"Relays TCP traffic (e.g., printer jobs on port 9100) and logs the data."

std::string g_executable_dir;
std::string g_ini_filename = "Printer_Relay_Logger.ini";
std::string g_local_host = "127.0.0.1";
std::string g_local_port_str = "9100";
std::string g_relay_host;
std::string g_relay_port_str = "9100";
std::string g_log_directory_name = "printer_logs";
std::string g_data_directory_name = "printer_data";
const std::string LOG_FILENAME_PREFIX = "printer_log_";
const std::string LOG_FILENAME_SUFFIX = ".log";
const char* LOG_FILENAME_FORMAT = "%Y-%m-%d_%H";
const int LOG_BACKUP_COUNT = 720;

const int LOG_LEVEL = 0; // 0 = Info, 1 = Debug
constexpr size_t BUFFER_SIZE = 4096;

std::mutex g_log_mutex;
std::ofstream g_log_file;
std::string g_current_log_filename;
int g_current_log_file_hour = -1;
std::atomic<bool> g_shutdown_requested = false;

SERVICE_STATUS g_serviceStatus;
SERVICE_STATUS_HANDLE g_serviceStatusHandle = NULL;
HANDLE g_serviceStopEvent = INVALID_HANDLE_VALUE;
SOCKET g_listen_socket = INVALID_SOCKET;

void WINAPI ServiceMain(DWORD dwArgc, LPWSTR *lpszArgv);
void WINAPI ServiceCtrlHandler(DWORD dwCtrl);
DWORD WINAPI ServiceWorkerThread(LPVOID lpParam);
void Log(int level, const std::string& message);
bool ParseIniFile(const std::string& filename, std::string& local_host, std::string& local_port, std::string& relay_host, std::string& relay_port);
std::string GetTimestamp();
void ReportEventLog(WORD type, DWORD eventID, const std::string& message);

std::string trim(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\n\r\f\v");
    if (std::string::npos == first) {
        return str;
    }
    size_t last = str.find_last_not_of(" \t\n\r\f\v");
    return str.substr(first, (last - first + 1));
}

bool ParseIniFile(const std::string& filename,
                  std::string& local_host,
                  std::string& local_port,
                  std::string& relay_host,
                  std::string& relay_port)
{
    std::ifstream ini_file(filename);
    if (!ini_file.is_open()) {
        Log(99,"[ERROR] INI file not found or cannot be opened: " + filename + ". Service cannot start without RelayHost.");
        ReportEventLog(EVENTLOG_ERROR_TYPE, 1001, "INI file not found or cannot be opened: " + filename + ". Service cannot start without RelayHost.");
        return false;
    }

    Log(0, "[INFO] Reading configuration from INI file: " + filename);
    std::string line;
    bool found_config = false;
    bool found_relay_host = false;
    while (std::getline(ini_file, line)) {
        line = trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#') {
            continue;
        }

        size_t equals_pos = line.find('=');
        if (equals_pos == std::string::npos) {
            continue;
        }

        std::string key = trim(line.substr(0, equals_pos));
        std::string value = trim(line.substr(equals_pos + 1));

        if (key == "LocalHost") {
            local_host = value;
            found_config = true;
        } else if (key == "LocalPort") {
            local_port = value;
            found_config = true;
        } else if (key == "RelayHost") {
            relay_host = value;
            found_config = true;
            if (!value.empty()) found_relay_host = true;
        } else if (key == "RelayPort") {
            relay_port = value;
            found_config = true;
        }
    }

    ini_file.close();
    if (!found_relay_host) {
        Log(99, "[ERROR] RelayHost setting is missing or empty in INI file: " + filename);
        ReportEventLog(EVENTLOG_ERROR_TYPE, 1002, "RelayHost setting is missing or empty in INI file: " + filename);
        return false;
    }
    return true;
}

std::string GetTimestamp() {
    auto now = std::chrono::system_clock::now();
    auto now_c = std::chrono::system_clock::to_time_t(now);
    std::tm now_tm;
    localtime_s(&now_tm, &now_c);

    auto duration = now.time_since_epoch();
    auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(duration).count() % 1000;

    std::stringstream ss;
    ss << std::put_time(&now_tm, "%Y-%m-%d %H:%M:%S") << '.' << std::setfill('0') << std::setw(3) << millis;
    return ss.str();
}

std::string DataToHexSnippet(const char* data, int len, size_t max_bytes_to_show = 32) {
    std::stringstream ss;
    ss << std::hex << std::setfill('0');
    size_t bytes_to_show = std::min((size_t)len, max_bytes_to_show);
    for (size_t i = 0; i < bytes_to_show; ++i) {
        ss << std::setw(2) << static_cast<int>(static_cast<unsigned char>(data[i]));
        if (i < bytes_to_show - 1) ss << " ";
    }
    if ((size_t)len > bytes_to_show) {
        ss << "...";
    }
    return ss.str();
}

std::string GetAddressString(const sockaddr* addr) {
    char ip_str[INET6_ADDRSTRLEN];
    void* sin_addr = nullptr;
    int port = 0;

    if (addr->sa_family == AF_INET) {
        sockaddr_in* ipv4 = (sockaddr_in*)addr;
        sin_addr = &(ipv4->sin_addr);
        port = ntohs(ipv4->sin_port);
    } else if (addr->sa_family == AF_INET6) {
        sockaddr_in6* ipv6 = (sockaddr_in6*)addr;
        sin_addr = &(ipv6->sin6_addr);
        port = ntohs(ipv6->sin6_port);
    } else {
        return "Unknown Address Family";
    }

    if (inet_ntop(addr->sa_family, sin_addr, ip_str, sizeof(ip_str))) {
        return std::string(ip_str) + ":" + std::to_string(port);
    } else {
        return "Invalid Address";
    }
}

int GetCurrentHour() {
    auto now = std::chrono::system_clock::now();
    auto now_c = std::chrono::system_clock::to_time_t(now);
    std::tm now_tm;
    localtime_s(&now_tm, &now_c);
    return now_tm.tm_hour;
}

std::string GenerateLogFilename(int hour) {
     auto now = std::chrono::system_clock::now();
     auto now_c = std::chrono::system_clock::to_time_t(now);
     std::tm now_tm;
     localtime_s(&now_tm, &now_c);
     now_tm.tm_hour = hour;

     char time_buffer[100];
     strftime(time_buffer, sizeof(time_buffer), LOG_FILENAME_FORMAT, &now_tm);

     std::filesystem::path dir_path = g_executable_dir;
     dir_path /= g_log_directory_name;
     std::filesystem::path file_path = dir_path / (LOG_FILENAME_PREFIX + std::string(time_buffer) + LOG_FILENAME_SUFFIX);
     return file_path.string();
}

std::string GenerateDataFilename(const std::string& log_prefix) {
    std::string timestamp = GetTimestamp();
    std::replace(timestamp.begin(), timestamp.end(), ':', '_');
    std::replace(timestamp.begin(), timestamp.end(), '.', '_');
    std::replace(timestamp.begin(), timestamp.end(), ' ', '_');

    size_t start_pos = log_prefix.find('[');
    size_t end_pos = log_prefix.find(']');
    std::string client_info = "unknown_client";
    if (start_pos != std::string::npos && end_pos != std::string::npos && start_pos < end_pos) {
        client_info = log_prefix.substr(start_pos + 1, end_pos - start_pos - 1);
        std::replace(client_info.begin(), client_info.end(), ':', '_');
    }

    std::filesystem::path dir_path = g_executable_dir;
    dir_path /= g_data_directory_name;
    std::filesystem::path file_path = dir_path / ("data_" + timestamp + "_" + client_info + ".bin");
    return file_path.string();
}

void RotateLogsIfNeeded() {
    int current_hour = GetCurrentHour();
    if (current_hour == g_current_log_file_hour && g_log_file.is_open()) {
        return;
    }

    if (g_log_file.is_open()) {
        g_log_file.close();
        std::cout << "[" << GetTimestamp() << "] [INFO] Closed log file: " << g_current_log_filename << std::endl; // Log to console might not be visible
    }

    std::string new_filename = GenerateLogFilename(current_hour);
    std::filesystem::path log_dir_path = std::filesystem::path(new_filename).parent_path();
    std::string log_dir_str = log_dir_path.string();

    g_current_log_filename = new_filename;
    g_current_log_file_hour = current_hour;

    try {
        if (!std::filesystem::exists(log_dir_path)) {
             std::filesystem::create_directories(log_dir_path);
        }

        std::vector<std::filesystem::path> log_files;
        if (std::filesystem::is_directory(log_dir_path)) {
            for (const auto& entry : std::filesystem::directory_iterator(log_dir_path)) {
                if (entry.is_regular_file()) {
                    std::string fname = entry.path().filename().string();
                    if (fname.rfind(LOG_FILENAME_PREFIX, 0) == 0 && fname.find(LOG_FILENAME_SUFFIX) != std::string::npos) {
                        log_files.push_back(entry.path());
                    }
                }
            }
        }

        std::sort(log_files.begin(), log_files.end(), [](const auto& a, const auto& b) {
            return std::filesystem::last_write_time(a) < std::filesystem::last_write_time(b);
        });

        int files_to_remove = static_cast<int>(log_files.size()) - LOG_BACKUP_COUNT;
        if (files_to_remove > 0) {
             for (int i = 0; i < files_to_remove && i < log_files.size(); ++i) {
                 if (log_files[i].string() != new_filename) {
                    std::error_code ec;
                    std::filesystem::remove(log_files[i], ec);
                     if (ec) {
                         std::cerr << "[" << GetTimestamp() << "] [WARN] Failed to remove old log file " << log_files[i] << ": " << ec.message() << std::endl;
                     } else {
                         std::cout << "[" << GetTimestamp() << "] [INFO] Removed old log file: " << log_files[i].string() << std::endl;
                     }
                 }
             }
        }

    } catch (const std::filesystem::filesystem_error& e) {
        std::cerr << "[" << GetTimestamp() << "] [ERROR] Filesystem error during log rotation: " << e.what() << std::endl;
        ReportEventLog(EVENTLOG_WARNING_TYPE, 2001, "Filesystem error during log rotation: " + std::string(e.what()));
    } catch (const std::exception& e) {
         std::cerr << "[" << GetTimestamp() << "] [ERROR] Unexpected error during log rotation: " << e.what() << std::endl;
         ReportEventLog(EVENTLOG_WARNING_TYPE, 2002, "Unexpected error during log rotation: " + std::string(e.what()));
    }

    g_log_file.open(g_current_log_filename, std::ios::app);
    if (!g_log_file.is_open()) {
        std::cerr << "[" << GetTimestamp() << "] [ERROR] Failed to open new log file: " << g_current_log_filename << std::endl;
        ReportEventLog(EVENTLOG_ERROR_TYPE, 2003, "Failed to open new log file: " + g_current_log_filename);
    } else {
         std::cout << "[" << GetTimestamp() << "] [INFO] Opened log file: " << g_current_log_filename << std::endl;
         g_log_file << "[" << GetTimestamp() << "] [INFO] Log file opened." << std::endl;
         g_log_file.flush();
    }
}

void Log(int level, const std::string& message) {
    if (level > LOG_LEVEL && level != 99) return;

    std::lock_guard<std::mutex> lock(g_log_mutex);

    std::string level_str = (level == 99) ? "ERROR" : ((level == 1) ? "DEBUG" : "INFO");
    std::string timestamp = GetTimestamp();
    std::string formatted_message = "[" + timestamp + "] [" + level_str + "] " + message;

    std::ostream& out_stream = (level == 99) ? std::cerr : std::cout;
    out_stream << formatted_message << std::endl;

    try {
        RotateLogsIfNeeded();
        if (g_log_file.is_open()) {
            g_log_file << formatted_message << std::endl;
            if(level == 99 || level == 0) g_log_file.flush();
        }
    } catch (const std::exception& e) {
         std::cerr << "[" << GetTimestamp() << "] [CRITICAL] Exception during logging to file: " << e.what() << std::endl;
    }
}

void ReportEventLog(WORD type, DWORD eventID, const std::string& message) {
    HANDLE hEventSource = NULL;
    LPCWSTR lpszStrings[1];
    std::wstring wmessage = std::wstring(message.begin(), message.end());

    hEventSource = RegisterEventSourceW(NULL, SERVICE_NAME);

    if (NULL != hEventSource) {
        lpszStrings[0] = wmessage.c_str();

        ReportEventW(hEventSource,        // Event log handle
                    type,                // Event type
                    0,                   // Event category
                    eventID,             // Event identifier
                    NULL,                // No user security identifier
                    1,                   // Number of substitution strings
                    0,                   // No binary data
                    lpszStrings,         // Array of strings
                    NULL);               // No binary data

        DeregisterEventSource(hEventSource);
    } else {
        std::cerr << "[" << GetTimestamp() << "] [CRITICAL] Failed to register event source for logging." << std::endl;
    }
}

void PipeDataThread(SOCKET source_socket, SOCKET dest_socket, const std::string& source_desc, const std::string& dest_desc, const std::string& log_prefix) {
    char buffer[BUFFER_SIZE];
    int bytes_received;
    int bytes_sent;
    int total_bytes = 0;
    int result;
    std::ofstream data_file;
    std::string data_filename;
    bool is_client_to_relay = source_desc.rfind("Client ", 0) == 0 && dest_desc.rfind("Relay ", 0) == 0;

    Log(0, log_prefix + "Starting pipe: " + source_desc + " -> " + dest_desc);

    if (is_client_to_relay) {
        std::filesystem::path data_dir_path = g_executable_dir;
        data_dir_path /= g_data_directory_name;
        try {
            if (!std::filesystem::exists(data_dir_path)) {
                std::filesystem::create_directories(data_dir_path);
            }
            data_filename = GenerateDataFilename(log_prefix);
            data_file.open(data_filename, std::ios::binary | std::ios::app);
            if (!data_file.is_open()) {
                 Log(99, log_prefix + "Failed to open data file for writing: " + data_filename);
                 ReportEventLog(EVENTLOG_WARNING_TYPE, 3001, log_prefix + "Failed to open data file: " + data_filename);
            } else {
                 Log(0, log_prefix + "Opened data file for recording: " + data_filename);
            }
        } catch (const std::exception& e) {
             Log(99, log_prefix + "Error preparing data directory/file " + data_filename + ": " + e.what());
             ReportEventLog(EVENTLOG_WARNING_TYPE, 3002, log_prefix + "Error preparing data file " + data_filename + ": " + e.what());
        }

    }

    while (!g_shutdown_requested) {
        bytes_received = recv(source_socket, buffer, sizeof(buffer), 0);

        if (bytes_received > 0) {
            total_bytes += bytes_received;
            Log(0, log_prefix + "Relaying " + std::to_string(bytes_received) + " bytes from " + source_desc + " to " + dest_desc
                  + ". Snippet: [" + DataToHexSnippet(buffer, bytes_received) + "]");
            Log(1, log_prefix + "Data Hex: " + DataToHexSnippet(buffer, bytes_received, bytes_received));

            if (is_client_to_relay && data_file.is_open()) {
                data_file.write(buffer, bytes_received);
                if (!data_file) {
                    Log(99, log_prefix + "Error writing to data file: " + data_filename);
                    ReportEventLog(EVENTLOG_WARNING_TYPE, 3003, log_prefix + "Error writing data file: " + data_filename);
                    data_file.close();
                }
            }

            bytes_sent = send(dest_socket, buffer, bytes_received, 0);
            if (bytes_sent == SOCKET_ERROR) {
                Log(99, log_prefix + "send failed from " + source_desc + " to " + dest_desc + " with error: " + std::to_string(WSAGetLastError()));
                break;
            }
            if (bytes_sent != bytes_received) {
                 Log(99, log_prefix + "send did not send all bytes from " + source_desc + " to " + dest_desc + ". Sent: " + std::to_string(bytes_sent) + ", Expected: " + std::to_string(bytes_received));
                break;
            }
             Log(1, log_prefix + "Wrote " + std::to_string(bytes_sent) + " bytes to " + dest_desc);

        } else if (bytes_received == 0) {
            Log(1, log_prefix + "Connection closed gracefully (EOF) by " + source_desc);
            break;
        } else {
            int error_code = WSAGetLastError();
             if (error_code == WSAECONNRESET || error_code == WSAECONNABORTED || error_code == WSAESHUTDOWN) {
                 Log(0, log_prefix + "Connection reset/aborted by " + source_desc + " (Error: " + std::to_string(error_code) + ")");
             } else if (error_code == WSAEINTR) {
                  Log(0, log_prefix + "Recv interrupted on " + source_desc);
             } else if (g_shutdown_requested) {
                  Log(0, log_prefix + "Recv stopped due to shutdown request on " + source_desc);
             } else {
                 Log(99, log_prefix + "recv failed from " + source_desc + " with error: " + std::to_string(error_code));
             }
            break;
        }
    }

    if (bytes_received >= 0 && !g_shutdown_requested) {
        result = shutdown(dest_socket, SD_SEND);
        if (result == SOCKET_ERROR) {
             int shutdown_err = WSAGetLastError();
             if (shutdown_err != WSAENOTCONN && shutdown_err != WSAECONNRESET && shutdown_err != WSAESHUTDOWN) {
                 Log(0, log_prefix + "shutdown(SD_SEND) failed for " + dest_desc + " with error: " + std::to_string(shutdown_err));
             }
        } else {
             Log(1, log_prefix + "Shutdown SD_SEND successful for " + dest_desc);
        }
    }

    if (data_file.is_open()) {
        data_file.close();
        Log(0, log_prefix + "Closed data file: " + data_filename);
    }

    Log(0, log_prefix + "Pipe finished (" + source_desc + " -> " + dest_desc + "). Total bytes: " + std::to_string(total_bytes));
}

void HandleClientThread(SOCKET client_socket, std::string client_addr_str) {
    SOCKET relay_socket = INVALID_SOCKET;
    int result;
    struct addrinfo *relay_addr_result = nullptr, *ptr = nullptr, hints;
    std::string log_prefix = "[" + client_addr_str + "] ";

    Log(0, log_prefix + "Accepted connection.");

    ZeroMemory(&hints, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    result = getaddrinfo(g_relay_host.c_str(), g_relay_port_str.c_str(), &hints, &relay_addr_result);
    if (result != 0) {
        Log(99, log_prefix + "getaddrinfo failed for relay host " + g_relay_host + " with error: " + std::to_string(result));
        closesocket(client_socket);
        return;
    }

    Log(0, log_prefix + "Attempting to connect to Relay " + g_relay_host + ":" + g_relay_port_str + "...");

    for (ptr = relay_addr_result; ptr != nullptr; ptr = ptr->ai_next) {
        relay_socket = socket(ptr->ai_family, ptr->ai_socktype, ptr->ai_protocol);
        if (relay_socket == INVALID_SOCKET) {
            Log(99, log_prefix + "socket() failed for relay connection with error: " + std::to_string(WSAGetLastError()));
            continue;
        }

        result = connect(relay_socket, ptr->ai_addr, (int)ptr->ai_addrlen);
        if (result == SOCKET_ERROR) {
            closesocket(relay_socket);
            relay_socket = INVALID_SOCKET;
            int error_code = WSAGetLastError();
             Log(0, log_prefix + "connect() failed for relay address " + GetAddressString(ptr->ai_addr) + " with error: " + std::to_string(error_code));
            continue;
        }
        break;
    }

    freeaddrinfo(relay_addr_result);

    if (relay_socket == INVALID_SOCKET) {
        Log(99, log_prefix + "Unable to connect to relay server " + g_relay_host + ":" + g_relay_port_str);
        closesocket(client_socket);
        return;
    }

     sockaddr_storage relay_peer_addr;
     int peer_addr_len = sizeof(relay_peer_addr);
     std::string relay_desc = "Relay Unknown";
     if (getpeername(relay_socket, (sockaddr*)&relay_peer_addr, &peer_addr_len) == 0) {
         relay_desc = "Relay " + GetAddressString((sockaddr*)&relay_peer_addr);
     }

    Log(0, log_prefix + "Successfully connected to " + relay_desc);

    std::string client_desc = "Client " + client_addr_str;
    std::thread client_to_relay_thread;
    std::thread relay_to_client_thread;

    try {
        client_to_relay_thread = std::thread(PipeDataThread, client_socket, relay_socket, client_desc, relay_desc, log_prefix);
        relay_to_client_thread = std::thread(PipeDataThread, relay_socket, client_socket, relay_desc, client_desc, log_prefix);
        client_to_relay_thread.detach();
        relay_to_client_thread.detach();
        Log(1, log_prefix + "Pipe threads detached.");

    } catch (const std::system_error& e) {
        Log(99, log_prefix + "System error creating pipe threads: " + e.what());
        ReportEventLog(EVENTLOG_ERROR_TYPE, 4001, log_prefix + "System error creating pipe threads: " + std::string(e.what()));
         if (relay_socket != INVALID_SOCKET) closesocket(relay_socket);
         if (client_socket != INVALID_SOCKET) closesocket(client_socket);
    } catch (const std::exception& e) {
         Log(99, log_prefix + "Exception creating pipe threads: " + e.what());
         ReportEventLog(EVENTLOG_ERROR_TYPE, 4002, log_prefix + "Exception creating pipe threads: " + std::string(e.what()));
         if (relay_socket != INVALID_SOCKET) closesocket(relay_socket);
         if (client_socket != INVALID_SOCKET) closesocket(client_socket);
    }
}

void ReportSvcStatus(DWORD dwCurrentState, DWORD dwWin32ExitCode, DWORD dwWaitHint) {
    static DWORD dwCheckPoint = 1;

    g_serviceStatus.dwCurrentState = dwCurrentState;
    g_serviceStatus.dwWin32ExitCode = dwWin32ExitCode;
    g_serviceStatus.dwWaitHint = dwWaitHint;

    if (dwCurrentState == SERVICE_START_PENDING || dwCurrentState == SERVICE_STOP_PENDING)
        g_serviceStatus.dwCheckPoint = dwCheckPoint++;
    else
        g_serviceStatus.dwCheckPoint = 0;

    SetServiceStatus(g_serviceStatusHandle, &g_serviceStatus);
}

void WINAPI ServiceCtrlHandler(DWORD dwCtrl) {
    switch (dwCtrl) {
    case SERVICE_CONTROL_STOP:
        ReportSvcStatus(SERVICE_STOP_PENDING, NO_ERROR, 30000);
        Log(0, "Service stop request received.");
        ReportEventLog(EVENTLOG_INFORMATION_TYPE, 103, "Service stop request received.");

        g_shutdown_requested = true;

        if (g_listen_socket != INVALID_SOCKET) {
            closesocket(g_listen_socket);
            g_listen_socket = INVALID_SOCKET;
            Log(1,"Listener socket closed to interrupt accept().");
        }

        SetEvent(g_serviceStopEvent);
        break;

    case SERVICE_CONTROL_INTERROGATE:
        break;

    default:
        break;
    }

    ReportSvcStatus(g_serviceStatus.dwCurrentState, NO_ERROR, 0);
}

void WINAPI ServiceMain(DWORD dwArgc, LPWSTR* lpszArgv) {
    g_serviceStatusHandle = RegisterServiceCtrlHandlerW(SERVICE_NAME, ServiceCtrlHandler);
    if (!g_serviceStatusHandle) {
        Log(99, "Failed to register service control handler. Error: " + std::to_string(GetLastError()));
        ReportEventLog(EVENTLOG_ERROR_TYPE, 101, "Failed to register service control handler. Error: " + std::to_string(GetLastError()));
        return;
    }

    g_serviceStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_serviceStatus.dwServiceSpecificExitCode = 0;
    g_serviceStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP;

    ReportSvcStatus(SERVICE_START_PENDING, NO_ERROR, 3000);

    g_serviceStopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (g_serviceStopEvent == NULL) {
        ReportSvcStatus(SERVICE_STOPPED, GetLastError(), 0);
        Log(99, "Failed to create stop event. Error: " + std::to_string(GetLastError()));
        ReportEventLog(EVENTLOG_ERROR_TYPE, 102, "Failed to create stop event. Error: " + std::to_string(GetLastError()));
        return;
    }

    HANDLE hThread = CreateThread(NULL, 0, ServiceWorkerThread, NULL, 0, NULL);
    if (hThread == NULL) {
         ReportSvcStatus(SERVICE_STOPPED, GetLastError(), 0);
         Log(99, "Failed to create service worker thread. Error: " + std::to_string(GetLastError()));
         ReportEventLog(EVENTLOG_ERROR_TYPE, 104, "Failed to create service worker thread. Error: " + std::to_string(GetLastError()));
         CloseHandle(g_serviceStopEvent);
         return;
    }

    Log(0,"ServiceMain: Worker thread created. Waiting for stop signal.");
    WaitForSingleObject(hThread, INFINITE);
    Log(0,"ServiceMain: Worker thread finished or stop event received.");

    CloseHandle(g_serviceStopEvent);
    g_serviceStopEvent = INVALID_HANDLE_VALUE;
    CloseHandle(hThread);

    ReportSvcStatus(SERVICE_STOPPED, NO_ERROR, 0);
    Log(0,"ServiceMain: Service stopped successfully reported.");
    ReportEventLog(EVENTLOG_INFORMATION_TYPE, 105, "Service stopped successfully.");

     if (g_log_file.is_open()) {
          std::lock_guard<std::mutex> lock(g_log_mutex);
          g_log_file.flush();
          g_log_file.close();
          std::cout << "[" << GetTimestamp() << "] [INFO] Closed final log file in ServiceMain: " << g_current_log_filename << std::endl;
     }
}

DWORD WINAPI ServiceWorkerThread(LPVOID lpParam) {
    Log(0, "ServiceWorkerThread started.");

    WSADATA wsaData;
    int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (result != 0) {
        Log(99, "WSAStartup failed with error: " + std::to_string(result));
        ReportEventLog(EVENTLOG_ERROR_TYPE, 5001, "WSAStartup failed: " + std::to_string(result));
        ReportSvcStatus(SERVICE_STOPPED, result, 0);
        return 1;
    }
    Log(1, "Winsock initialized.");

    std::string ini_path = (std::filesystem::path(g_executable_dir) / g_ini_filename).string();
    if (!ParseIniFile(ini_path, g_local_host, g_local_port_str, g_relay_host, g_relay_port_str)) {
        Log(99, "Failed to parse INI file or missing RelayHost. Service cannot run.");
        WSACleanup();
        ReportSvcStatus(SERVICE_STOPPED, ERROR_INVALID_DATA, 0);
        return 1;
    }
    Log(0, "Configuration loaded.");
    Log(0, "  Local: " + g_local_host + ":" + g_local_port_str);
    Log(0, "  Relay: " + g_relay_host + ":" + g_relay_port_str);
    Log(0, "  Log Dir: " + (std::filesystem::path(g_executable_dir) / g_log_directory_name).string());
    Log(0, "  Data Dir: " + (std::filesystem::path(g_executable_dir) / g_data_directory_name).string());


     try {
         std::filesystem::path log_dir_abs = std::filesystem::path(g_executable_dir) / g_log_directory_name;
         if (!std::filesystem::exists(log_dir_abs)) {
             std::filesystem::create_directories(log_dir_abs);
             Log(0,"Created log directory: " + log_dir_abs.string());
         }
         std::filesystem::path data_dir_abs = std::filesystem::path(g_executable_dir) / g_data_directory_name;
          if (!std::filesystem::exists(data_dir_abs)) {
              std::filesystem::create_directories(data_dir_abs);
              Log(0,"Created data directory: " + data_dir_abs.string());
          }
     } catch (const std::exception& e) {
         Log(99, "Error creating log/data directories: " + std::string(e.what()));
         ReportEventLog(EVENTLOG_ERROR_TYPE, 5002, "Error creating directories: " + std::string(e.what()));
     }

    struct addrinfo *listen_addr_result = nullptr, hints;
    ZeroMemory(&hints, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_PASSIVE;

    result = getaddrinfo(g_local_host.c_str(), g_local_port_str.c_str(), &hints, &listen_addr_result);
    if (result != 0) {
        Log(99, "getaddrinfo for local bind failed with error: " + std::to_string(result));
        ReportEventLog(EVENTLOG_ERROR_TYPE, 5003, "getaddrinfo failed: " + std::to_string(result));
        WSACleanup();
        ReportSvcStatus(SERVICE_STOPPED, result, 0);
        return 1;
    }

    g_listen_socket = socket(listen_addr_result->ai_family, listen_addr_result->ai_socktype, listen_addr_result->ai_protocol);
    if (g_listen_socket == INVALID_SOCKET) {
        Log(99, "socket() for listener failed with error: " + std::to_string(WSAGetLastError()));
        ReportEventLog(EVENTLOG_ERROR_TYPE, 5004, "Listener socket() failed: " + std::to_string(WSAGetLastError()));
        freeaddrinfo(listen_addr_result);
        WSACleanup();
        ReportSvcStatus(SERVICE_STOPPED, WSAGetLastError(), 0);
        return 1;
    }

    result = bind(g_listen_socket, listen_addr_result->ai_addr, (int)listen_addr_result->ai_addrlen);
    if (result == SOCKET_ERROR) {
        Log(99, "bind() failed with error: " + std::to_string(WSAGetLastError()));
        ReportEventLog(EVENTLOG_ERROR_TYPE, 5005, "Listener bind() failed: " + std::to_string(WSAGetLastError()));
        freeaddrinfo(listen_addr_result);
        closesocket(g_listen_socket); g_listen_socket = INVALID_SOCKET;
        WSACleanup();
        ReportSvcStatus(SERVICE_STOPPED, WSAGetLastError(), 0);
        return 1;
    }

    freeaddrinfo(listen_addr_result);

    result = listen(g_listen_socket, SOMAXCONN);
    if (result == SOCKET_ERROR) {
        Log(99, "listen() failed with error: " + std::to_string(WSAGetLastError()));
        ReportEventLog(EVENTLOG_ERROR_TYPE, 5006, "Listener listen() failed: " + std::to_string(WSAGetLastError()));
        closesocket(g_listen_socket); g_listen_socket = INVALID_SOCKET;
        WSACleanup();
        ReportSvcStatus(SERVICE_STOPPED, WSAGetLastError(), 0);
        return 1;
    }

    ReportSvcStatus(SERVICE_RUNNING, NO_ERROR, 0);
    Log(0, "Service is RUNNING. Listening on " + g_local_host + ":" + g_local_port_str);
    ReportEventLog(EVENTLOG_INFORMATION_TYPE, 100, "Service started successfully. Listening on " + g_local_host + ":" + g_local_port_str + ", Relaying to " + g_relay_host + ":" + g_relay_port_str);

    while (!g_shutdown_requested) {
        sockaddr_storage client_addr;
        int client_addr_size = sizeof(client_addr);
        SOCKET client_socket = accept(g_listen_socket, (struct sockaddr*)&client_addr, &client_addr_size);

        if (client_socket == INVALID_SOCKET) {
            int error_code = WSAGetLastError();
            if (g_shutdown_requested) {
                Log(0,"accept() interrupted by shutdown request.");
                break;
            }
            if (error_code == WSAEINTR) {
                Log(0, "accept() interrupted (WSAEINTR), continuing loop.");
                continue;
            }

            Log(99, "accept() failed with error: " + std::to_string(error_code));
            ReportEventLog(EVENTLOG_WARNING_TYPE, 5007, "accept() failed: " + std::to_string(error_code));
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        std::string client_addr_str = GetAddressString((struct sockaddr*)&client_addr);
        Log(1, "Accepted connection from " + client_addr_str);

        try {
             std::thread(HandleClientThread, client_socket, client_addr_str).detach();
        } catch (const std::system_error& e) {
            Log(99, "Failed to create handler thread: " + std::string(e.what()));
            ReportEventLog(EVENTLOG_ERROR_TYPE, 5008, "Failed to create handler thread: " + std::string(e.what()));
            closesocket(client_socket);
        } catch (const std::exception& e) {
             Log(99, "Exception creating handler thread: " + std::string(e.what()));
             ReportEventLog(EVENTLOG_ERROR_TYPE, 5009, "Exception creating handler thread: " + std::string(e.what()));
             closesocket(client_socket);
        }
    }

    Log(0, "ServiceWorkerThread: Shutdown initiated. Cleaning up...");

    if (g_listen_socket != INVALID_SOCKET) {
        closesocket(g_listen_socket);
        g_listen_socket = INVALID_SOCKET;
        Log(1,"Cleaned up listener socket in worker thread.");
    }

    WSACleanup();
    Log(1, "Winsock cleaned up.");
    Log(0, "ServiceWorkerThread finished.");
    return 0;
}

int main(int argc, char* argv[]) {
    char path[MAX_PATH];
    GetModuleFileNameA(NULL, path, MAX_PATH);
    std::filesystem::path exePath = path;
    g_executable_dir = exePath.parent_path().string();

    if (!SetCurrentDirectoryA(g_executable_dir.c_str())) {
         std::cerr << "Failed to set working directory to " << g_executable_dir << ". Error: " << GetLastError() << std::endl;
    } else {
         std::cout << "Working directory set to: " << g_executable_dir << std::endl;
    }

    SERVICE_TABLE_ENTRYW ServiceTable[] = {
        { (LPWSTR)SERVICE_NAME, (LPSERVICE_MAIN_FUNCTIONW)ServiceMain },
        { NULL, NULL }
    };

    if (!StartServiceCtrlDispatcherW(ServiceTable)) {
        DWORD error = GetLastError();
        if (error == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) {
            std::cerr << "This program must be run as a Windows service." << std::endl;
            std::cerr << "Install using: sc create PrinterRelayLogger binPath= \"" << path << "\"" << std::endl;
            std::cerr << "Start using:   sc start PrinterRelayLogger" << std::endl;
            std::cerr << "Stop using:    sc stop PrinterRelayLogger" << std::endl;
            std::cerr << "Delete using:  sc delete PrinterRelayLogger" << std::endl;

        } else {
            std::cerr << "StartServiceCtrlDispatcher failed with error: " << error << std::endl;
            std::ofstream errFile(g_executable_dir + "\\service_startup_error.log");
            if(errFile.is_open()){
                errFile << "[" << GetTimestamp() << "] StartServiceCtrlDispatcher failed with error: " << error << std::endl;
                errFile.close();
            }
        }
        return 1;
    }
    return 0;
}
