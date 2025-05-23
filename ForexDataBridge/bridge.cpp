#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <string>
#include <mutex>
#include <fstream>
#include <thread>
#include <unordered_map>

#pragma comment(lib, "ws2_32.lib")

#define PORT 5050

std::mutex messageMutex;
std::unordered_map<std::string, bool> subscribedSymbols; // Tracks active subscriptions
SOCKET clientSocket = INVALID_SOCKET;
bool serverRunning = false;
std::string latestJsonFromNode = ""; // New: stores latest JSON message

// Open log file once at startup
std::ofstream logFile("C:\\Users\\upram\\OneDrive\\Desktop\\workspace\\Forex-Robot\\ForexDataBridge\\bridge.txt", std::ios::app);

// Logging function (flushes after every message)
void LogMessage(const std::string &message)
{
    logFile << message << std::endl;
    logFile.flush();
}

// Extract JSON values manually
std::string ExtractJsonValue(const std::string &json, const std::string &key)
{
    std::string searchKey = "\"" + key + "\":\"";
    size_t start = json.find(searchKey);
    if (start == std::string::npos)
        return "";
    start += searchKey.length();
    size_t end = json.find("\"", start);
    if (end == std::string::npos)
        return "";
    return json.substr(start, end - start);
}

// Persistent TCP Server function (runs in a thread)
void TCPServer()
{
    WSADATA wsaData;
    SOCKET serverSocket;
    struct sockaddr_in serverAddr;

    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
    {
        LogMessage("[DLL] WSAStartup failed!");
        return;
    }

    serverSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (serverSocket == INVALID_SOCKET)
    {
        LogMessage("[DLL] Socket creation failed!");
        WSACleanup();
        return;
    }

    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(PORT);
    serverAddr.sin_addr.s_addr = INADDR_ANY;

    if (bind(serverSocket, (struct sockaddr *)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR)
    {
        LogMessage("[DLL] Bind failed! Port might be in use.");
        closesocket(serverSocket);
        WSACleanup();
        return;
    }

    if (listen(serverSocket, SOMAXCONN) == SOCKET_ERROR)
    {
        LogMessage("[DLL] Listen failed!");
        closesocket(serverSocket);
        WSACleanup();
        return;
    }

    serverRunning = true;
    LogMessage("[DLL] TCP Server Started!");

    clientSocket = accept(serverSocket, NULL, NULL);
    if (clientSocket == INVALID_SOCKET)
    {
        LogMessage("[DLL] Accept failed!");
        closesocket(serverSocket);
        WSACleanup();
        return;
    }
    LogMessage("[DLL] Client Connected.");

    char buffer[1024];
    memset(buffer, 0, sizeof(buffer));
    while (serverRunning)
    {
        int bytesReceived = recv(clientSocket, buffer, sizeof(buffer) - 1, 0);
        if (bytesReceived > 0)
        {
            buffer[bytesReceived] = '\0';
            std::string receivedMessage = buffer;
            LogMessage("[DLL] Received: " + receivedMessage);

            std::string action = ExtractJsonValue(receivedMessage, "action");
            std::string symbol = ExtractJsonValue(receivedMessage, "symbol");

            std::lock_guard<std::mutex> lock(messageMutex);

            // Store full JSON message for MQL5
            latestJsonFromNode = receivedMessage;

            if (action == "SUBSCRIBE")
            {
                subscribedSymbols[symbol] = true;
                LogMessage("[DLL] Subscribed to: " + symbol);
            }
            else if (action == "UNSUBSCRIBE")
            {
                if (subscribedSymbols.count(symbol))
                {
                    subscribedSymbols[symbol] = false;
                    LogMessage("[DLL] Unsubscribed from: " + symbol);
                }
                else
                {
                    LogMessage("[DLL] Warning: Tried to unsubscribe non-existent symbol: " + symbol);
                }
            }

            // Log active symbols
            std::string activeSymbols;
            for (const auto &pair : subscribedSymbols)
            {
                activeSymbols += pair.first + ";";
            }
            LogMessage("[DLL] Active Symbols Sent to MQL5: " + activeSymbols);

            // Respond to Node.js
            std::string response = "{\"status\":\"processed\",\"symbol\":\"" + symbol + "\",\"action\":\"" + action + "\"}";
            std::string messageWithDelimiter = response + "\n";
            send(clientSocket, messageWithDelimiter.c_str(), messageWithDelimiter.size(), 0);

            LogMessage("[DLL] Sent to Node: " + response);
        }
        else if (bytesReceived == 0)
        {
            LogMessage("[DLL] Client disconnected.");
            break;
        }
    }

    closesocket(clientSocket);
    closesocket(serverSocket);
    WSACleanup();
    LogMessage("[DLL] TCP Server Stopped.");
}

// Start the TCP server (called from MQL5)
extern "C" __declspec(dllexport) int StartTCPServer()
{
    static bool serverStarted = false;
    if (!serverStarted)
    {
        std::thread serverThread(TCPServer);
        serverThread.detach();
        serverStarted = true;
        return 0;
    }
    return -1;
}

// Stop the TCP server (called from MQL5)
extern "C" __declspec(dllexport) int StopTCPServer()
{
    serverRunning = false;
    if (clientSocket != INVALID_SOCKET)
    {
        closesocket(clientSocket);
    }
    WSACleanup();
    LogMessage("[DLL] TCP Server Stopped.");
    return 0;
}

// Get the latest subscribed symbols (called from MQL5)
extern "C" __declspec(dllexport) const wchar_t *GetLatestMessage()
{
    static wchar_t buffer[1024];
    std::lock_guard<std::mutex> lock(messageMutex);

    std::string latestUpdates;
    for (const auto &pair : subscribedSymbols)
    {
        latestUpdates += pair.first + ";";
    }

    if (latestUpdates.empty())
    {
        latestUpdates = "NONE"; // Prevent MQL5 from processing empty string
    }

    std::wstring unicodeMessage = std::wstring(latestUpdates.begin(), latestUpdates.end());
    wcsncpy(buffer, unicodeMessage.c_str(), sizeof(buffer) / sizeof(buffer[0]) - 1);
    buffer[sizeof(buffer) / sizeof(buffer[0]) - 1] = L'\0';

    LogMessage("[DLL] Active Symbols Sent to MQL5: " + latestUpdates);
    return buffer;
}

// Send message to Node.js (called from MQL5)
extern "C" __declspec(dllexport) int SendMessageToNode(const wchar_t *message)
{
    int len = WideCharToMultiByte(CP_ACP, 0, message, -1, NULL, 0, NULL, NULL);
    std::string ansiMessage(len, 0);
    WideCharToMultiByte(CP_ACP, 0, message, -1, &ansiMessage[0], len, NULL, NULL);

    std::string taggedMessage = ansiMessage + "\n";

    if (clientSocket != INVALID_SOCKET)
    {
        send(clientSocket, taggedMessage.c_str(), taggedMessage.size(), 0);
        LogMessage("[DLL] Sent to Node: " + taggedMessage);
        return 0;
    }
    else
    {
        LogMessage("[DLL] No active connection to Node.js!");
        return -1;
    }
}

// New: Get latest JSON from Node.js (called from MQL5)
extern "C" __declspec(dllexport) const wchar_t *GetJsonFromNode()
{
    static wchar_t buffer[2048];
    std::lock_guard<std::mutex> lock(messageMutex);

    std::wstring unicodeMessage = std::wstring(latestJsonFromNode.begin(), latestJsonFromNode.end());
    wcsncpy(buffer, unicodeMessage.c_str(), sizeof(buffer) / sizeof(buffer[0]) - 1);
    buffer[sizeof(buffer) / sizeof(buffer[0]) - 1] = L'\0';

    return buffer;
}
