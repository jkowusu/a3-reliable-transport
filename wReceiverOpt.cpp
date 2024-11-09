#include <iostream>
#include <fstream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <vector>
#include <cstring>
#include "crc32.h"
#include "PacketHeader.h"
#include <map>
#include <unordered_map>

#define MAX_PACKET_SIZE 1472

const int MAX_DATA_SIZE = MAX_PACKET_SIZE - sizeof(PacketHeader);


using namespace std;

struct InputParams{
    string outputDirectory;      // The directory that the wReceiver will store the output files, i.e the FILE-i.out files.
    string logFilePath;           // Argument for log file path
    int listeningPort;         // Convert receiver port to integer
    int windowSize;       // Convert window size to integer
    unsigned startEndSeqNum;
    bool isStart;
};


void logOutput(ofstream &logFile, const PacketHeader &header)
{
    logFile << (header.type) << " " << (header.seqNum) << " "
            << (header.length) << " " << (header.checksum) << endl;
}

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



void receiveFile(int listenSocket, int fileIndex, struct sockaddr_in &clientAddr,
                 ofstream &log, InputParams &params) {    
    string outputFile = params.outputDirectory + "/FILE-" + to_string(fileIndex) + ".out";
    ofstream out(outputFile, ios::binary);

    if (!out) {
        cerr << "Error creating: " << outputFile << "\n";
        return;
    }

    socklen_t addrLen = sizeof(clientAddr);
    char buffer[MAX_PACKET_SIZE];
    std::map<unsigned int, std::vector<char> > packetBuffer;
    PacketHeader ack;
    int expectedSeqNum = 0;

    while (true) {
        ssize_t bytesRecv = recvfrom(listenSocket, buffer, MAX_PACKET_SIZE, 0, 
                                     (struct sockaddr *)&clientAddr, &addrLen);

        if (bytesRecv == -1) {
            perror("Failed to receive data");
            continue;
        }

        PacketHeader *header = reinterpret_cast<PacketHeader *>(buffer);
        headerNtoh(*header);
        logOutput(log, *header);

        if (header->type == 0) {
            cout << "Cannot have a START message in the middle of an existing connection\n";
            continue;
        }

        if (header->type == 1) {
            // End Packet
            ack.checksum = 0;
            ack.length = 0;
            ack.seqNum = params.startEndSeqNum;
            ack.type = 3;

            headerHton(ack);
            ssize_t bytesSent = sendto(listenSocket, &ack, sizeof(ack), 0, 
                                       (struct sockaddr *)&clientAddr, sizeof(clientAddr));
            if (bytesSent == -1) {
                perror("Failed to send END packet ACK");
                continue;
            }

            headerNtoh(ack);
            logOutput(log, ack);
            break;
        }

        if (header->type == 2) {
            unsigned int headerSeqNum = header->seqNum;
            size_t dataLength = header->length;

            if (headerSeqNum >= expectedSeqNum + params.windowSize) {
                cout << "Packet outside of window size, dropping\n";
                continue;
            }

            char *data = buffer + sizeof(PacketHeader);
            if (dataLength > 0 && dataLength <= MAX_DATA_SIZE) {
                if (crc32(data, dataLength) != header->checksum) {
                    cout << "Checksum mismatch, dropping packet\n";
                    continue;
                }

                // Acknowledge the received packet’s sequence number
                ack.seqNum = headerSeqNum;
                ack.type = 3;
                ack.checksum = 0;
                ack.length = 0;
                headerHton(ack);

                ssize_t bytesSent = sendto(listenSocket, &ack, sizeof(ack), 0, 
                                           (struct sockaddr *)&clientAddr, sizeof(clientAddr));
                if (bytesSent == -1) {
                    perror("Failed to send ACK for received packet");
                }

                headerNtoh(ack);
                logOutput(log, ack);

                if (headerSeqNum == expectedSeqNum) {
                    out.write(data, dataLength);
                    expectedSeqNum++;

                    while (packetBuffer.count(expectedSeqNum)) {
                        out.write(packetBuffer[expectedSeqNum].data(), packetBuffer[expectedSeqNum].size());
                        packetBuffer.erase(expectedSeqNum);
                        expectedSeqNum++;
                    }
                } else {
                    packetBuffer[headerSeqNum] = std::vector<char>(data, data + dataLength);
                }
            }
        }
    }

    out.close();
}




void connect(int listenSocket, InputParams &params) {
   
   //Initiate output file
    ofstream log(params.logFilePath);
    if (!log){
        cerr << "Failed to open input file: " << params.logFilePath << endl;
        exit(1);
    }
    //Make clientAddr struct
    struct sockaddr_in clientAddr;
    memset(&clientAddr, 0, sizeof(clientAddr));  // Zero out the client address structure
    socklen_t len = sizeof(clientAddr);  // Initialize length for client address

    PacketHeader header;
    PacketHeader ack;
   
    int fileIndex = 0;

    // Continuously listen
    while (true) {
        // Receive data from the client
        ssize_t bytesRecv = recvfrom(listenSocket, &header, sizeof(header), 0, 
                                     (struct sockaddr *)&clientAddr, &len);
        
        if (bytesRecv == -1) {
            perror("Failed to receive data");
            continue;
        }

        headerNtoh(header);

        logOutput(log,header);

        if ( header.type == 0){
            cout << "Start packet received\n";
            // Receive the file
            params.startEndSeqNum = header.seqNum;
            // Send an ack for the START packet
            ack.checksum = 0;
            ack.length = 0;
            ack.seqNum = params.startEndSeqNum;
            ack.type = 3;

            headerHton(ack);

            ssize_t bytesSent = sendto(listenSocket, &ack, sizeof(ack), 0, 
                               (struct sockaddr *)&clientAddr, sizeof(clientAddr));
        
            if (bytesSent == -1) {
                perror("Failed to send START packet ack");
                continue;
            }

            headerNtoh(ack); //Change back for proper logging
            logOutput(log,ack);
            // After getting the start packet and acking, we can start receiving 
            receiveFile(listenSocket, fileIndex++, clientAddr, log, params);
        }

    }

    log.close();
}


int main(int argc, char *argv[])
{
    if (argc != 5)
    {
        cerr << "WRONG NUMBER OF ARGS. Usage: ./wReceiver <port-num> <window-size> <output-dir> <log>" << endl;
        return 1;
    }

    InputParams params;


    params.listeningPort = atoi(argv[1]);
    params.windowSize = atoi(argv[2]);
    params.outputDirectory = argv[3];
    params.logFilePath = argv[4];
   

    int sockfd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sockfd < 0)
    {
        perror("SOCKET ERROR");
        return 1;
    }

    sockaddr_in serverAddr;
    // Zero out the server address structure and set properties
    memset(&serverAddr, 0, sizeof(serverAddr));
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(params.listeningPort);
    serverAddr.sin_addr.s_addr = INADDR_ANY;

    if (::bind(sockfd, (const struct sockaddr *)&serverAddr, sizeof(serverAddr)) == -1) {
    perror("BIND ERROR");
    close(sockfd);
    return 1;
}

    cout << "UDP Server is listening..." << endl;

    connect(sockfd, params);
    
    close(sockfd);
    return 0;
}