#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

// Prevent windows.h from defining min() and max() macros, which conflict with std::min/max
#define NOMINMAX 

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h> // For GetLocalTime, CreateDirectory, MoveFileEx etc. (if not using filesystem)

#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <chrono>
#include <iomanip>
#include <filesystem> // Requires C++17
#include <atomic>
#include <sstream>
#include <cstdlib> // For std::exit, std::atoi
#include <cstdio> // For sprintf_s
#include <algorithm> // For std::replace, std::remove


// Link with Ws2_32.lib
#pragma comment(lib, "Ws2_32.lib")

// --- Configuration ---
const char* LOCAL_HOST = "127.0.0.1";
const char* LOCAL_PORT_STR = "9100";
std::string g_relay_host; // MUST be provided via command line argument
const char* RELAY_PORT_STR = "9100";

const std::string LOG_DIRECTORY = "printer_logs";
const std::string LOG_FILENAME_PREFIX = "printer_log_"; // Prefix for log files
const std::string LOG_FILENAME_SUFFIX = ".log";
const char* LOG_FILENAME_FORMAT = "%Y-%m-%d_%H"; // Format for strftime used in filename
const int LOG_BACKUP_COUNT = 720; // Keep the last ~30 days (720 hours) of hourly log files
const std::string DATA_DIRECTORY = "printer_data"; // Directory to save relayed data

// Log level - Simplified for this example (0=Info, 1=Debug)
const int LOG_LEVEL = 0; // 0 = Info, 1 = Debug
constexpr size_t BUFFER_SIZE = 4096;
// --- End Configuration ---


// --- Global Logging Variables ---
std::mutex g_log_mutex;
std::ofstream g_log_file;
std::string g_current_log_filename;
int g_current_log_file_hour = -1; // Hour the current log file was opened for (-1 initially)
std::atomic<bool> g_shutdown_requested = false;
// --- End Global Logging Variables ---

// --- Helper Functions ---

// Get current timestamp as string
std::string GetTimestamp() {
    auto now = std::chrono::system_clock::now();
    auto now_c = std::chrono::system_clock::to_time_t(now);
    std::tm now_tm;
    localtime_s(&now_tm, &now_c); // Use localtime_s for safety

    // Get milliseconds
    auto duration = now.time_since_epoch();
    auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(duration).count() % 1000;

    std::stringstream ss;
    ss << std::put_time(&now_tm, "%Y-%m-%d %H:%M:%S") << '.' << std::setfill('0') << std::setw(3) << millis;
    return ss.str();
}

// Convert raw data to hex string snippet
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

// Get IP address string from sockaddr
std::string GetAddressString(const sockaddr* addr) {
    char ip_str[INET6_ADDRSTRLEN]; // Max length for IPv6
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

// Get current hour (0-23)
int GetCurrentHour() {
    auto now = std::chrono::system_clock::now();
    auto now_c = std::chrono::system_clock::to_time_t(now);
    std::tm now_tm;
    localtime_s(&now_tm, &now_c);
    return now_tm.tm_hour;
}

// Generate log filename for a specific hour
std::string GenerateLogFilename(int hour) {
     auto now = std::chrono::system_clock::now();
     auto now_c = std::chrono::system_clock::to_time_t(now);
     std::tm now_tm;
     localtime_s(&now_tm, &now_c);
     now_tm.tm_hour = hour; // Use the specified hour

     char time_buffer[100];
     strftime(time_buffer, sizeof(time_buffer), LOG_FILENAME_FORMAT, &now_tm);

     std::filesystem::path dir_path = LOG_DIRECTORY;
     std::filesystem::path file_path = dir_path / (LOG_FILENAME_PREFIX + std::string(time_buffer) + LOG_FILENAME_SUFFIX);
     return file_path.string();
}

// Helper function to generate data filename
std::string GenerateDataFilename(const std::string& log_prefix) {
    std::string timestamp = GetTimestamp();
    // Sanitize timestamp for filename (replace :, . with _)
    std::replace(timestamp.begin(), timestamp.end(), ':', '_');
    std::replace(timestamp.begin(), timestamp.end(), '.', '_');

    // Sanitize log_prefix (remove brackets, spaces, :)
    // Extract client IP:Port from log_prefix like "[127.0.0.1:12345] "
    size_t start_pos = log_prefix.find('[');
    size_t end_pos = log_prefix.find(']');
    std::string client_info = "unknown_client";
    if (start_pos != std::string::npos && end_pos != std::string::npos && start_pos < end_pos) {
        client_info = log_prefix.substr(start_pos + 1, end_pos - start_pos - 1);
        std::replace(client_info.begin(), client_info.end(), ':', '_'); // Replace colon in port
    }


    std::filesystem::path dir_path = DATA_DIRECTORY;
    std::filesystem::path file_path = dir_path / ("data_" + timestamp + "_" + client_info + ".bin");
    return file_path.string();
}


// Rotate log files if necessary (MUST be called with g_log_mutex held)
void RotateLogsIfNeeded() {
    int current_hour = GetCurrentHour();
    if (current_hour == g_current_log_file_hour && g_log_file.is_open()) {
        return; // No rotation needed
    }

    // Close the old log file if it's open
    if (g_log_file.is_open()) {
        g_log_file.close();
        std::cout << "[" << GetTimestamp() << "] [INFO] Closed log file: " << g_current_log_filename << std::endl;
    }

    // Generate new filename
    std::string new_filename = GenerateLogFilename(current_hour);
    g_current_log_filename = new_filename;
    g_current_log_file_hour = current_hour;

    // --- Handle Backups ---
    // This part involves renaming existing files based on their timestamp.
    // A simpler approach for Win32 might be to just keep the last N files found
    // by modification time, but let's try to mimic the Python logic roughly.
    // We will simply delete files older than backup_count * hours.
    try {
        std::filesystem::path log_dir(LOG_DIRECTORY);
        std::vector<std::filesystem::path> log_files;
        if (std::filesystem::exists(log_dir) && std::filesystem::is_directory(log_dir)) {
            for (const auto& entry : std::filesystem::directory_iterator(log_dir)) {
                if (entry.is_regular_file()) {
                    std::string fname = entry.path().filename().string();
                    if (fname.rfind(LOG_FILENAME_PREFIX, 0) == 0 && fname.find(LOG_FILENAME_SUFFIX) != std::string::npos) {
                        log_files.push_back(entry.path());
                    }
                }
            }
        }

        // Sort by last write time (oldest first)
        std::sort(log_files.begin(), log_files.end(), [](const auto& a, const auto& b) {
            return std::filesystem::last_write_time(a) < std::filesystem::last_write_time(b);
        });

        // Remove oldest files if count exceeds backup limit
        int files_to_remove = static_cast<int>(log_files.size()) - LOG_BACKUP_COUNT;
        if (files_to_remove > 0) { // Avoid removing the file we are about to open
             for (int i = 0; i < files_to_remove && i < log_files.size(); ++i) {
                 // Double check we don't remove the file we are about to open (unlikely but possible)
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
        std::cerr << "[" << GetTimestamp() << "] [ERROR] Filesystem error during log rotation cleanup: " << e.what() << std::endl;
    } catch (const std::exception& e) {
         std::cerr << "[" << GetTimestamp() << "] [ERROR] Unexpected error during log rotation cleanup: " << e.what() << std::endl;
    }
    // --- End Handle Backups ---

    // Open the new log file
    g_log_file.open(g_current_log_filename, std::ios::app); // Append mode
    if (!g_log_file.is_open()) {
        std::cerr << "[" << GetTimestamp() << "] [ERROR] Failed to open new log file: " << g_current_log_filename << std::endl;
    } else {
         std::cout << "[" << GetTimestamp() << "] [INFO] Opened log file: " << g_current_log_filename << std::endl;
         // Write a header maybe?
         g_log_file << "[" << GetTimestamp() << "] [INFO] Log file opened." << std::endl;
         g_log_file.flush();
    }
}


// Log message to console and file (thread-safe)
void Log(int level, const std::string& message) {
    if (level > LOG_LEVEL && level != 99) return; // 99 for errors/critical

    std::lock_guard<std::mutex> lock(g_log_mutex);

    std::string level_str = (level == 99) ? "ERROR" : ((level == 1) ? "DEBUG" : "INFO");
    std::string timestamp = GetTimestamp();
    std::string formatted_message = "[" + timestamp + "] [" + level_str + "] " + message;

    // Print to console
    std::ostream& out_stream = (level == 99) ? std::cerr : std::cout;
    out_stream << formatted_message << std::endl;

    // Rotate and write to file
    try {
        RotateLogsIfNeeded(); // Check and rotate if necessary
        if (g_log_file.is_open()) {
            g_log_file << formatted_message << std::endl;
            // No need to flush every message unless critical
            if(level == 99) g_log_file.flush();
        }
    } catch (const std::exception& e) {
         // Catch potential exceptions during file operations within the lock
         std::cerr << "[" << GetTimestamp() << "] [CRITICAL] Exception during logging to file: " << e.what() << std::endl;
    }
}

// --- Networking Logic ---

// Function executed by the pipe threads
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

    // If client -> relay, open data file
    if (is_client_to_relay) {
        data_filename = GenerateDataFilename(log_prefix);
        data_file.open(data_filename, std::ios::binary | std::ios::app); // Append mode just in case, though should be new file
        if (!data_file.is_open()) {
            Log(99, log_prefix + "Failed to open data file for writing: " + data_filename);
            // Continue piping even if file fails? Yes, core functionality is relaying.
        } else {
            Log(0, log_prefix + "Opened data file for recording: " + data_filename);
        }
    }


    while (!g_shutdown_requested) {
        bytes_received = recv(source_socket, buffer, sizeof(buffer), 0);

        if (bytes_received > 0) {
            total_bytes += bytes_received;
            Log(0, log_prefix + "Relaying " + std::to_string(bytes_received) + " bytes from " + source_desc + " to " + dest_desc
                  + ". Snippet: [" + DataToHexSnippet(buffer, bytes_received) + "]");
            Log(1, log_prefix + "Data Hex: " + DataToHexSnippet(buffer, bytes_received, bytes_received)); // Full hex if debug

            // Write received data to file if it's client->relay and file is open
            if (is_client_to_relay && data_file.is_open()) {
                data_file.write(buffer, bytes_received);
                if (!data_file) { // Check for write errors
                    Log(99, log_prefix + "Error writing to data file: " + data_filename);
                    data_file.close(); // Close file on error
                }
            }

            bytes_sent = send(dest_socket, buffer, bytes_received, 0);
            if (bytes_sent == SOCKET_ERROR) {
                Log(99, log_prefix + "send failed from " + source_desc + " to " + dest_desc + " with error: " + std::to_string(WSAGetLastError()));
                break;
            }
            if (bytes_sent != bytes_received) {
                 Log(99, log_prefix + "send did not send all bytes from " + source_desc + " to " + dest_desc + ". Sent: " + std::to_string(bytes_sent) + ", Expected: " + std::to_string(bytes_received));
                // Potentially handle partial send here, but often break is okay for simple proxy
                break;
            }
             Log(1, log_prefix + "Wrote " + std::to_string(bytes_sent) + " bytes to " + dest_desc);

        } else if (bytes_received == 0) {
            Log(1, log_prefix + "Connection closed gracefully (EOF) by " + source_desc);
            break; // Peer disconnected
        } else { // bytes_received == SOCKET_ERROR
            int error_code = WSAGetLastError();
             if (error_code == WSAECONNRESET || error_code == WSAECONNABORTED || error_code == WSAESHUTDOWN) {
                 Log(0, log_prefix + "Connection reset/aborted by " + source_desc + " (Error: " + std::to_string(error_code) + ")");
             } else if (error_code == WSAEINTR) { // Interrupted (maybe by closesocket on this thread's socket)
                  Log(0, log_prefix + "Recv interrupted on " + source_desc);
             }
              else if (g_shutdown_requested) {
                  Log(0, log_prefix + "Recv stopped due to shutdown request on " + source_desc);
              }
             else {
                 Log(99, log_prefix + "recv failed from " + source_desc + " with error: " + std::to_string(error_code));
             }
            break; // Error receiving data
        }
    }

    // Shutdown the sending side of the *destination* socket to signal EOF
    // This helps the *other* pipe thread break its recv loop cleanly.
    // Don't do this if the error was related to the destination already being closed.
    if (bytes_received >= 0) { // Only shutdown if recv didn't fail critically
        result = shutdown(dest_socket, SD_SEND);
        if (result == SOCKET_ERROR) {
             // Ignore errors like "not connected" which are expected if the other side already closed
             int shutdown_err = WSAGetLastError();
             if (shutdown_err != WSAENOTCONN && shutdown_err != WSAECONNRESET && shutdown_err != WSAESHUTDOWN) {
                 Log(0, log_prefix + "shutdown(SD_SEND) failed for " + dest_desc + " with error: " + std::to_string(shutdown_err));
             }
        } else {
             Log(1, log_prefix + "Shutdown SD_SEND successful for " + dest_desc);
        }
    }

    // Close data file if it was opened
    if (data_file.is_open()) {
        data_file.close();
        Log(0, log_prefix + "Closed data file: " + data_filename);
    }


    Log(0, log_prefix + "Pipe finished (" + source_desc + " -> " + dest_desc + "). Total bytes: " + std::to_string(total_bytes));
}


// Function executed by the client handler threads
void HandleClientThread(SOCKET client_socket, std::string client_addr_str) {
    SOCKET relay_socket = INVALID_SOCKET;
    int result;
    struct addrinfo *relay_addr_result = nullptr, *ptr = nullptr, hints;
    std::string log_prefix = "[" + client_addr_str + "] ";

    Log(0, log_prefix + "Accepted connection.");

    // --- Connect to Relay Server ---
    ZeroMemory(&hints, sizeof(hints));
    hints.ai_family = AF_UNSPEC; // Allow IPv4 or IPv6
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    result = getaddrinfo(g_relay_host.c_str(), RELAY_PORT_STR, &hints, &relay_addr_result);
    if (result != 0) {
        Log(99, log_prefix + "getaddrinfo failed for relay host " + g_relay_host + " with error: " + std::to_string(result));
        closesocket(client_socket);
        return;
    }

    Log(0, log_prefix + "Attempting to connect to Relay " + g_relay_host + ":" + RELAY_PORT_STR + "...");

    // Attempt to connect to an address until one succeeds
    for (ptr = relay_addr_result; ptr != nullptr; ptr = ptr->ai_next) {
        relay_socket = socket(ptr->ai_family, ptr->ai_socktype, ptr->ai_protocol);
        if (relay_socket == INVALID_SOCKET) {
            Log(99, log_prefix + "socket() failed for relay connection with error: " + std::to_string(WSAGetLastError()));
            continue; // Try next address
        }

        result = connect(relay_socket, ptr->ai_addr, (int)ptr->ai_addrlen);
        if (result == SOCKET_ERROR) {
            closesocket(relay_socket);
            relay_socket = INVALID_SOCKET;
            int error_code = WSAGetLastError();
             Log(0, log_prefix + "connect() failed for relay address " + GetAddressString(ptr->ai_addr) + " with error: " + std::to_string(error_code));
            continue; // Try next address
        }

        // Success!
        break;
    }

    freeaddrinfo(relay_addr_result); // Free the address info structure

    if (relay_socket == INVALID_SOCKET) {
        Log(99, log_prefix + "Unable to connect to relay server " + g_relay_host + ":" + RELAY_PORT_STR);
        closesocket(client_socket);
        return;
    }

     // Get actual relay endpoint address string
     sockaddr_storage relay_peer_addr;
     int peer_addr_len = sizeof(relay_peer_addr);
     std::string relay_desc = "Relay Unknown";
     if (getpeername(relay_socket, (sockaddr*)&relay_peer_addr, &peer_addr_len) == 0) {
         relay_desc = "Relay " + GetAddressString((sockaddr*)&relay_peer_addr);
     }


    Log(0, log_prefix + "Successfully connected to " + relay_desc);

    // --- Start Piping Data ---
    std::string client_desc = "Client " + client_addr_str;

    try {
        // Create threads for bidirectional piping
        std::thread client_to_relay_thread(PipeDataThread, client_socket, relay_socket, client_desc, relay_desc, log_prefix);
        std::thread relay_to_client_thread(PipeDataThread, relay_socket, client_socket, relay_desc, client_desc, log_prefix);

        // Wait for both pipe threads to complete
        client_to_relay_thread.join();
        Log(1, log_prefix + "Client->Relay pipe thread finished.");
        relay_to_client_thread.join();
        Log(1, log_prefix + "Relay->Client pipe thread finished.");

    } catch (const std::system_error& e) {
        Log(99, log_prefix + "System error creating/joining pipe threads: " + e.what());
    } catch (const std::exception& e) {
         Log(99, log_prefix + "Exception creating/joining pipe threads: " + e.what());
    }


    // --- Cleanup ---
    Log(0, log_prefix + "Closing connections.");
    if (relay_socket != INVALID_SOCKET) {
        closesocket(relay_socket);
        relay_socket = INVALID_SOCKET;
    }
    if (client_socket != INVALID_SOCKET) {
        closesocket(client_socket);
        client_socket = INVALID_SOCKET;
    }
    Log(0, log_prefix + "Connection handling finished.");
}


// --- Main Function ---
int main(int argc, char* argv[]) {
     // --- Argument Parsing ---
     if (argc < 2) {
         std::cerr << "Error: Missing required argument: Relay Host IP address." << std::endl;
         std::cerr << "Usage: " << (argc > 0 ? argv[0] : "printer_relay_logger_win32") << " <RELAY_HOST_IP>" << std::endl;
         return 1; // Exit if IP is not provided
     }

     g_relay_host = argv[1];
     std::cout << "Using relay host: " << g_relay_host << std::endl;

     if (argc > 2) {
          std::cerr << "Warning: Ignoring extra arguments. Only the first argument (Relay Host IP) is used." << std::endl;
     }

    // --- Setup Logging Directory ---
     try {
         std::filesystem::path log_dir(LOG_DIRECTORY);
         if (!std::filesystem::exists(log_dir)) {
             if (std::filesystem::create_directories(log_dir)) {
                 std::cout << "Created log directory: " << LOG_DIRECTORY << std::endl;
             } else {
                 std::cerr << "Error creating log directory: " << LOG_DIRECTORY << ". Logs will be saved in current directory if possible." << std::endl;
                 // Fallback handled implicitly by RotateLogsIfNeeded using relative paths
             }
         }
     } catch (const std::exception& e) {
         std::cerr << "Error accessing/creating log directory: " << e.what() << ". Attempting to proceed." << std::endl;
     }

     // --- Setup Data Directory ---
      try {
          std::filesystem::path data_dir(DATA_DIRECTORY);
          if (!std::filesystem::exists(data_dir)) {
              if (std::filesystem::create_directories(data_dir)) {
                  std::cout << "Created data directory: " << DATA_DIRECTORY << std::endl;
              } else {
                  std::cerr << "Error creating data directory: " << DATA_DIRECTORY << ". Data saving might fail." << std::endl;
              }
          }
      } catch (const std::exception& e) {
          std::cerr << "Error accessing/creating data directory: " << e.what() << ". Attempting to proceed." << std::endl;
      }


    // Initial Log Startup Messages (using std::cout before full logging is setup)
    std::cout << "==================================================" << std::endl;
    std::cout << "Starting Printer Relay Logger (Win32)" << std::endl;
    std::cout << "Listening on: " << LOCAL_HOST << ":" << LOCAL_PORT_STR << std::endl;
    std::cout << "Relaying to: " << g_relay_host << ":" << RELAY_PORT_STR << std::endl;
    std::cout << "Logging to directory: " << LOG_DIRECTORY << std::endl;
    std::cout << "Log filename format: " << LOG_FILENAME_PREFIX << "<YYYY-MM-DD_HH>" << LOG_FILENAME_SUFFIX << std::endl;
    std::cout << "Keeping " << LOG_BACKUP_COUNT << " backup log files." << std::endl;
    std::cout << "==================================================" << std::endl;


    // --- Initialize Winsock ---
    WSADATA wsaData;
    int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (result != 0) {
        Log(99, "WSAStartup failed with error: " + std::to_string(result));
        return 1;
    }

    SOCKET listen_socket = INVALID_SOCKET;
    SOCKET client_socket = INVALID_SOCKET;
    struct addrinfo *listen_addr_result = nullptr, hints;

    ZeroMemory(&hints, sizeof(hints));
    hints.ai_family = AF_INET; // Listen on IPv4 only for simplicity, change to AF_UNSPEC for dual-stack
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_PASSIVE; // For wildcard IP address

    // Resolve the local address and port to be used by the server
    result = getaddrinfo(LOCAL_HOST, LOCAL_PORT_STR, &hints, &listen_addr_result);
    if (result != 0) {
        Log(99, "getaddrinfo for local bind failed with error: " + std::to_string(result));
        WSACleanup();
        return 1;
    }

    // Create a SOCKET for the server to listen for client connections
    listen_socket = socket(listen_addr_result->ai_family, listen_addr_result->ai_socktype, listen_addr_result->ai_protocol);
    if (listen_socket == INVALID_SOCKET) {
        Log(99, "socket() for listener failed with error: " + std::to_string(WSAGetLastError()));
        freeaddrinfo(listen_addr_result);
        WSACleanup();
        return 1;
    }

    // Setup the TCP listening socket
    result = bind(listen_socket, listen_addr_result->ai_addr, (int)listen_addr_result->ai_addrlen);
    if (result == SOCKET_ERROR) {
        Log(99, "bind() failed with error: " + std::to_string(WSAGetLastError()));
        freeaddrinfo(listen_addr_result);
        closesocket(listen_socket);
        WSACleanup();
        return 1;
    }

    freeaddrinfo(listen_addr_result); // No longer needed

    result = listen(listen_socket, SOMAXCONN); // SOMAXCONN = reasonable backlog
    if (result == SOCKET_ERROR) {
        Log(99, "listen() failed with error: " + std::to_string(WSAGetLastError()));
        closesocket(listen_socket);
        WSACleanup();
        return 1;
    }

    Log(0, "Server listening on " + std::string(LOCAL_HOST) + ":" + LOCAL_PORT_STR + ". Press Ctrl+C to stop.");


    // --- Accept Client Connections Loop ---
    while (!g_shutdown_requested) {
        sockaddr_storage client_addr; // Use sockaddr_storage for IPv4/IPv6 compatibility
        int client_addr_size = sizeof(client_addr);

        client_socket = accept(listen_socket, (struct sockaddr*)&client_addr, &client_addr_size);
        if (client_socket == INVALID_SOCKET) {
            int error_code = WSAGetLastError();
            if (error_code == WSAEINTR || error_code == WSAECONNABORTED) {
                 Log(0, "accept() interrupted or aborted, likely due to shutdown.");
                 // Check g_shutdown_requested again if needed, or just break
                 if(g_shutdown_requested) break;
            } else {
                 Log(99, "accept() failed with error: " + std::to_string(error_code));
                 // Consider adding a small delay here before retrying accept on persistent errors
            }
            continue; // Continue listening or break if shutting down
        }

        std::string client_addr_str = GetAddressString((struct sockaddr*)&client_addr);
        Log(1, "Accepted connection from " + client_addr_str); // Debug log

        // Create a new thread to handle the client connection
        try {
             // Detach the thread - the server doesn't wait for it to finish.
             // The thread is responsible for closing its own sockets.
            std::thread(HandleClientThread, client_socket, client_addr_str).detach();
        } catch (const std::system_error& e) {
            Log(99, "Failed to create handler thread: " + std::string(e.what()));
            closesocket(client_socket); // Close the socket if thread creation failed
        } catch (const std::exception& e) {
             Log(99, "Exception creating handler thread: " + std::string(e.what()));
             closesocket(client_socket);
        }

        client_socket = INVALID_SOCKET; // Reset for the next accept call
    }

    // --- Shutdown ---
    Log(0, "Shutdown requested. Closing listener socket.");
    g_shutdown_requested = true; // Ensure flag is set

    if (listen_socket != INVALID_SOCKET) {
        closesocket(listen_socket);
        listen_socket = INVALID_SOCKET;
    }

    // Cleanup Winsock
    WSACleanup();

    // Close log file if open
     {
         std::lock_guard<std::mutex> lock(g_log_mutex);
         if (g_log_file.is_open()) {
             g_log_file.close();
             std::cout << "[" << GetTimestamp() << "] [INFO] Closed final log file: " << g_current_log_filename << std::endl;
         }
     }


    Log(0, "Printer Relay Logger stopped.");
    std::cout << "Printer Relay Logger stopped." << std::endl;

    return 0;
}
