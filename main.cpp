#include <iostream>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <vector>
#include <set>
#include <fstream>
#include <nlohmann/json.hpp> // Include the nlohmann json library

#pragma comment(lib, "ws2_32.lib") // Link the Winsock library

using json = nlohmann::json;

// Define the Packet structure
struct Packet {
    char symbol[5];        // 4 bytes for symbol + null terminator
    char buySellIndicator; // 1 byte for B/S
    int quantity;          // 4 bytes, big-endian
    int price;             // 4 bytes, big-endian
    int sequence;          // 4 bytes, big-endian
};

// Initialize Winsock
bool initializeWinsock() {
    WSADATA wsaData;
    int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (result != 0) {
        std::cerr << "WSAStartup failed: " << result << "\n";
        return false;
    }
    return true;
}

// Connect to the ABX server
SOCKET connectToServer(const char* server_ip, int port) {
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
        std::cerr << "Could not create socket: " << WSAGetLastError() << "\n";
        return INVALID_SOCKET;
    }

    sockaddr_in server;
    server.sin_family = AF_INET;
    server.sin_port = htons(port);
    inet_pton(AF_INET, server_ip, &server.sin_addr);

    if (connect(sock, (sockaddr*)&server, sizeof(server)) < 0) {
        std::cerr << "Connection failed: " << WSAGetLastError() << "\n";
        closesocket(sock);
        return INVALID_SOCKET;
    }

    return sock;
}

// Send request to stream all packets
void streamAllPackets(SOCKET sock) {
    uint8_t payload[2];
    payload[0] = 1; // Call Type 1: Stream All Packets
    payload[1] = 0; // ResendSeq (not used for Stream All Packets)

    if (send(sock, (char*)payload, sizeof(payload), 0) < 0) {
        std::cerr << "Failed to send request\n";
    }
}

// Request missing packets
void requestMissingPackets(SOCKET sock, const std::set<int>& missingSequences) {
    for (int seq : missingSequences) {
        uint8_t resendPayload[5];
        resendPayload[0] = 2; // Call Type 2: Resend Packet
        *(uint32_t*)(resendPayload + 1) = htonl(seq); // Sequence number to be resent

        if (send(sock, (char*)resendPayload, sizeof(resendPayload), 0) < 0) {
            std::cerr << "Failed to request resend for sequence " << seq << "\n";
        }

        char buffer[17]; // Total packet size: 4 + 1 + 4 + 4 + 4 = 17 bytes
        int totalBytesRead = 0;

        while (totalBytesRead < sizeof(buffer)) {
            int bytesReceived = recv(sock, buffer + totalBytesRead, sizeof(buffer) - totalBytesRead, 0);
            if (bytesReceived <= 0) {
                std::cerr << "Error receiving resent packet\n";
                break;
            }
            totalBytesRead += bytesReceived;
        }

        if (totalBytesRead == sizeof(buffer)) {
            Packet packet;
            std::memcpy(packet.symbol, buffer, 4);
            packet.symbol[4] = '\0'; // Null terminate symbol string
            packet.buySellIndicator = buffer[4];
            packet.quantity = ntohl(*(int*)(buffer + 5)); // Convert big-endian to host byte order
            packet.price = ntohl(*(int*)(buffer + 9));    // Convert big-endian to host byte order
            packet.sequence = ntohl(*(int*)(buffer + 13)); // Convert big-endian to host byte order

            // Store resent packet
            std::cout << "Received resent packet with sequence: " << packet.sequence << "\n";
        }
        else {
            std::cerr << "Resent packet was incomplete\n";
        }
    }
}

// Improved receiveData function
void receiveData(SOCKET sock, std::vector<Packet>& packets) {
    char buffer[17]; // Total packet size: 4 + 1 + 4 + 4 + 4 = 17 bytes
    std::set<int> receivedSequences;

    int bytesRead;
    while ((bytesRead = recv(sock, buffer, sizeof(buffer), 0)) > 0) {
        if (bytesRead < sizeof(buffer)) {
            std::cerr << "Incomplete packet received\n";
            continue; // Skip or handle partial packet reception
        }

        Packet packet;
        std::memcpy(packet.symbol, buffer, 4);
        packet.symbol[4] = '\0'; // Null terminate symbol string
        packet.buySellIndicator = buffer[4];
        packet.quantity = ntohl(*(int*)(buffer + 5)); // Convert big-endian to host byte order
        packet.price = ntohl(*(int*)(buffer + 9));    // Convert big-endian to host byte order
        packet.sequence = ntohl(*(int*)(buffer + 13)); // Convert big-endian to host byte order

        // Store packet
        packets.push_back(packet);
        receivedSequences.insert(packet.sequence);
    }

    if (bytesRead <= 0) {
        std::cerr << "Server disconnected or error occurred: " << WSAGetLastError() << "\n";
    }

    // Handle missing sequences
    int lastSequence = packets.back().sequence;
    std::set<int> missingSequences;

    for (int i = 1; i <= lastSequence; ++i) {
        if (receivedSequences.find(i) == receivedSequences.end()) {
            missingSequences.insert(i);
        }
    }

    if (!missingSequences.empty()) {
        requestMissingPackets(sock, missingSequences);
    }
}

// Write data to JSON file
void writeToJSON(const std::vector<Packet>& packets) {
    json jsonArray = json::array();

    for (const auto& packet : packets) {
        json jsonObject;
        jsonObject["symbol"] = packet.symbol;
        jsonObject["buy_sell"] = std::string(1, packet.buySellIndicator);
        jsonObject["quantity"] = packet.quantity;
        jsonObject["price"] = packet.price;
        jsonObject["sequence"] = packet.sequence;
        jsonArray.push_back(jsonObject);
    }

    std::ofstream outputFile("output.json");
    outputFile << jsonArray.dump(4); // Pretty-print with indentation
}

int main() {
    if (!initializeWinsock()) {
        return 1;
    }

    const char* server_ip = "127.0.0.1"; // Replace with actual server IP address
    int port = 3000;

    SOCKET sock = connectToServer(server_ip, port);
    if (sock == INVALID_SOCKET) return 1;

    streamAllPackets(sock);

    std::vector<Packet> packets;
    receiveData(sock, packets);

    writeToJSON(packets);

    closesocket(sock); // Close the socket
    WSACleanup(); // Clean up Winsock
    return 0;
}
