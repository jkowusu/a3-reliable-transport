#include <iostream>
#include <fstream>
#include <string>
#include <cstring>
#include <vector>
#include <map>
#include <chrono>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>  // socket(), bind(), listen(), accept()
#include <netdb.h>
#include <iomanip>
#include <cstdio>
#include <cstdlib>
#include <array>
#include <cassert>
#include <random>
#include "PacketHeader.h"
#include "crc32.h"

using namespace std;




const int MAX_PACKET_SIZE = 1472; // Including the custom header and data
const int CUSTOM_HEADER_SIZE = 16;
const int MAX_DATA_SIZE = MAX_PACKET_SIZE - CUSTOM_HEADER_SIZE;

struct Packet {
    PacketHeader header;
    char data[MAX_DATA_SIZE];

};

struct InputParams{
    string receiverIP;           // Argument for receiver IP
    string inputFileName;      // Argument for input file name
    string logFilePath;           // Argument for log file path
    int receiverPort;         // Convert receiver port to integer
    int windowSize;       // Convert window size to integer
    unsigned startEndSeqNum;
    bool isStart;
};

void headerNtoh(PacketHeader &header) {
    header.type = ntohl(header.type);
    header.seqNum = ntohl(header.seqNum);
    header.length = ntohl(header.length);
    header.checksum = ntohl(header.checksum);
}

void headerHton(PacketHeader &header) {
    header.type = htonl(header.type);
    header.seqNum = htonl(header.seqNum);
    header.length = htonl(header.length);
    header.checksum = htonl(header.checksum);
}

int getRandomNumber()
{
    random_device rd;
    mt19937 gen(rd());
    uniform_int_distribution<int> dis(1, 100); // Range from 1 to 100

    return dis(gen);
}

void logOutput(ofstream &logFile, const PacketHeader &header)
{
    logFile << ntohl(header.type) << " " 
            << ntohl(header.seqNum) << " "
            << ntohl(header.length) << " " 
            << ntohl(header.checksum) << endl;
}


int sendStartOrEndPacket(int sock, struct sockaddr_in &receiverAddr, InputParams &params, std::ofstream &log) {
    PacketHeader packet;
    // Set the header for the START or END packet
    packet.seqNum = params.startEndSeqNum;
    packet.type = params.isStart ? 0 : 1;  // 0 for START, 1 for END
    packet.length = 0;
    packet.checksum = 0;



    ssize_t bytesSent, bytesRecv;
    socklen_t addrLen = sizeof(receiverAddr);

    // Ack to be received
    PacketHeader ack;

    while (true) {
        // Send the START or END packet
        headerHton(packet);

        bytesSent = sendto(sock, &packet, CUSTOM_HEADER_SIZE, 0, 
                           (struct sockaddr *)&receiverAddr, sizeof(receiverAddr));
        if (bytesSent == -1) {
            perror("Failed to send START/END packet");
            return -1;
        }

        // Log the send
        headerNtoh(packet);
        logOutput(log, packet);


        // Wait for an ACK
        bytesRecv = recvfrom(sock, &ack, CUSTOM_HEADER_SIZE, 0, (struct sockaddr *)&receiverAddr, &addrLen);

        if (bytesRecv == -1) {
            if (errno == EWOULDBLOCK || errno == EAGAIN) {
                // Timeout occurred, meaning no ACK was received in 500ms
                std::cout << "Timeout waiting for ACK. Retrying..." << std::endl;
                continue;  // Retransmit on the next loop iteration
            } else {
                perror("Error receiving ACK");
                return -1;
            }
        } else {
            // Convert the received ACK fields to host byte order
            headerNtoh(ack);
            
            // Check if the received packet is the expected ACK
            if (ack.type == 3 && ack.seqNum == params.startEndSeqNum) {  // Type 3 is ACK
                // Log the ACK
                logOutput(log, ack);  // Assuming this function logs ACK details
                break;  // Exit the loop as ACK was received
            } else {
                std::cout << "Received unexpected packet or incorrect ACK. Retrying..." << std::endl;
            }
        }
    }

    return 0;
}


int sendDataPackets(int sock, struct sockaddr_in &receiverAddr, 
                    std::vector<Packet> &packets, InputParams &params, std::ofstream &log) {
    unsigned int base = 0;  // Left pointer (first unacknowledged packet)
    unsigned int nextSeqNum = 0;  // Right pointer (next packet to send)
    size_t totalPackets = packets.size();
    socklen_t addrLen = sizeof(receiverAddr);
    PacketHeader ack;

    std::vector<bool> isAcked(totalPackets, false);

    // Initial sending of packets within the window
    for (; nextSeqNum < base + params.windowSize && nextSeqNum < totalPackets; nextSeqNum++) {
        ssize_t bytesSent = sendto(sock, &packets[nextSeqNum], sizeof(packets[nextSeqNum]), 0, 
                                   (struct sockaddr *)&receiverAddr, sizeof(receiverAddr));
        if (bytesSent == -1) {
            perror("Failed to send DATA packet");
            return -1;
        }
        PacketHeader packet = packets[nextSeqNum].header;
        headerNtoh(packet);
        logOutput(log, packet);
    }

    while (base < totalPackets) {
        // Wait to receive an ACK with 500ms timeout
        ssize_t bytesRecv = recvfrom(sock, &ack, CUSTOM_HEADER_SIZE, 0, 
                                     (struct sockaddr *)&receiverAddr, &addrLen);

        if (bytesRecv == -1) {
            if (errno == EWOULDBLOCK || errno == EAGAIN) {
                // Timeout occurred; retransmit unacknowledged packets in current window
                std::cout << "Timeout waiting for ACK. Retransmitting unacknowledged packets in window..." << std::endl;
                for (unsigned int i = base; i < base + params.windowSize && i < nextSeqNum; ++i) {
                    if (!isAcked[i]) {
                        ssize_t bytesSent = sendto(sock, &packets[i], sizeof(packets[i]), 0, 
                                                   (struct sockaddr *)&receiverAddr, sizeof(receiverAddr));
                        if (bytesSent == -1) {
                            perror("Failed to retransmit DATA packet");
                            return -1;
                        }
                        PacketHeader packet = packets[i].header;
                        headerNtoh(packet);
                        logOutput(log, packet);
                    }
                }
                continue;  // Wait for ACKs again after retransmission
            } else {
                perror("Error receiving ACK");
                return -1;
            }
        } else {
            // Process the received ACK
            headerNtoh(ack);
            unsigned int ackSeqNum = ack.seqNum;
            logOutput(log, ack);

            // Mark packet as acknowledged
            if (ackSeqNum < totalPackets) {
                isAcked[ackSeqNum] = true;
            }

            // Move the base up only if the current base is acknowledged
            if (ackSeqNum == base) { 
                while (base < totalPackets && isAcked[base]) {
                    base++;
                }

                // Send new packets within the updated window bounds
                while (nextSeqNum < base + params.windowSize && nextSeqNum < totalPackets) {
                    if (!isAcked[nextSeqNum]) {  // Send only unacknowledged packets
                        ssize_t bytesSent = sendto(sock, &packets[nextSeqNum], sizeof(packets[nextSeqNum]), 0, 
                                                   (struct sockaddr *)&receiverAddr, sizeof(receiverAddr));
                        if (bytesSent == -1) {
                            perror("Failed to send DATA packet");
                            return -1;
                        }
                        PacketHeader packet = packets[nextSeqNum].header;
                        headerNtoh(packet);
                        logOutput(log, packet);
                    }
                    nextSeqNum++;
                }
            }
        }
    }

    std::cout << "All packets have been sent and acknowledged." << std::endl;
    return 0;
}



// A sender will make a socket, connect to a server (given provided IP and Port), send, close connection
int openConnections(InputParams &params, vector<Packet> &packets){
    
  int sockfd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sockfd == -1) {
        perror("Failed to create UDP socket");
        exit(EXIT_FAILURE);
    }

    //Set up the sockaddr struct
    struct sockaddr_in addr;
	addr.sin_family = AF_INET;
	struct hostent *host = gethostbyname(params.receiverIP.c_str());
	if (host == nullptr) {
		cout << "Error: Unknown host " << params.receiverIP << "\n";
		return -1;
	}
	memcpy(&(addr.sin_addr), host->h_addr, host->h_length);
	addr.sin_port = htons(params.receiverPort);

    // Set timeout for receiving ACK
    struct timeval tv;
    tv.tv_sec = 0;                // 0 seconds
    tv.tv_usec = 500 * 1000;      // 500 milliseconds
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));

    ofstream logg(params.logFilePath, ios::out);
    if (!logg) {
        cerr << "Failed to open log file: " << params.logFilePath << endl;
        return 1;
    }

    //Send Start
    params.isStart = true;
    if (sendStartOrEndPacket(sockfd,addr,params,logg) != 0){
        cerr << "Specifically Something went wrong with sending start packets\n";
    };
    // Send Data
    sendDataPackets(sockfd,addr,packets, params,logg);

    // Send End
    params.isStart = false;
    if (sendStartOrEndPacket(sockfd,addr,params,logg) != 0){
        cerr << "Specifically Something went wrong with sending end packets\n";
    };

    // Close the connections.
    int close_retval = close(sockfd);
    assert(close_retval == 0);

    logg.close();

    return 0;
}

int main(int argc, char *argv[]) {

    if (argc != 6) {
        cerr << "Invalid number of arguments. Usage: ./wSender <receiver-IP> <receiver-port> <window-size> <input-file> <log>" << endl;
        exit(1);
    }

    InputParams inputParams;

    inputParams.receiverIP =  argv[1];      
    inputParams.receiverPort  = atoi(argv[2]); 
    inputParams.windowSize = atoi(argv[3]);     
    inputParams.inputFileName  = argv[4]; 
    inputParams.logFilePath = argv[5];     
    inputParams.startEndSeqNum = getRandomNumber();

    
    vector<Packet> packets;


    // Read the input file in chunks and create a packet for each chuck
    ifstream input(inputParams.inputFileName);
    if (!input) {
        cerr << "Failed to open input file: " << inputParams.inputFileName << endl;
        return 1;
    }
    int seqNum = 0;

    while(input){
        Packet packet;

        // Read in a chunk of data from the file
        input.read(packet.data,MAX_DATA_SIZE);

        //Set the bytes read from the file
        int bytesRead = input.gcount();
        if (bytesRead == 0){
            break;
        }
        //Set the header
        packet.header.length = htonl(bytesRead);
        packet.header.seqNum = htonl(seqNum++);
        packet.header.type = htonl(2); // Data Packet
        packet.header.checksum = htonl(crc32(packet.data,bytesRead));

        // Add the packet to my vector
        packets.push_back(packet);

    }

    input.close();

    openConnections(inputParams, packets);

    return 0;
}