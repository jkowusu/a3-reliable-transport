#include <iostream>
#include <fstream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <vector>
#include <cstring>
#include <chrono> // For std::chrono::high_resolution_clock and timing
#include "crc32.h"
#include "PacketHeader.h"
#include <queue>
#include <random>

using namespace std;

const int MAX_PACKET_SIZE = 1472;

const int HEADER_SIZE = sizeof(PacketHeader);            // Size of the header
const int MAX_DATA_SIZE = MAX_PACKET_SIZE - HEADER_SIZE; // Max data size we can send per packet

int getRandomNumber()
{
    random_device rd;
    mt19937 gen(rd());
    uniform_int_distribution<int> dis(1, 100); // Range from 1 to 100

    return dis(gen);
}

void output(ofstream &logFile, const PacketHeader &header)
{
    logFile << (header.type) << " " << (header.seqNum) << " "
            << (header.length) << " " << (header.checksum) << endl;
}

bool verifyAck(PacketHeader &ackPacket, unsigned int currSeqNum)
{
    // type 3 means ACK
    return (ackPacket.type) == 3 && (ackPacket.seqNum) != currSeqNum;
}

bool waitForAck(int sock, struct sockaddr_in &receiverAddr,
                PacketHeader &ackPacket, unsigned int currSeqNum, int timeout, bool isStartOrEnd)
{
    struct timeval tv;
    tv.tv_sec = 0;               // 0 seconds
    tv.tv_usec = timeout * 1000; // 500 milliseconds = 500,000 microseconds
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
    socklen_t addrLen = sizeof(receiverAddr);

    while (true)
    {
        // Try to receive the packet
        int recvBytes = recvfrom(sock, &ackPacket, sizeof(ackPacket),
                                 0, (struct sockaddr *)&receiverAddr, &addrLen);
        ackPacket.type = ntohl(ackPacket.type);
        ackPacket.seqNum = ntohl(ackPacket.seqNum);
        ackPacket.length = ntohl(ackPacket.length);
        ackPacket.checksum = ntohl(ackPacket.checksum);
        if (recvBytes > 0)
        {
            cout << "RECIEVED ACK" << endl;
            // Packet received, verify ACK
            if ((ackPacket.type) == 3 && (ackPacket.seqNum) != currSeqNum && !isStartOrEnd)
            {
                cout << "ACK VALID" << endl;
                return true;
            }
            else if ((ackPacket.type) == 3 && (ackPacket.seqNum) == currSeqNum && isStartOrEnd)
            {
                cout << "ACK VALID" << endl;
                return true;
            }
            else
            {
                cout << "ACK INVALID" << endl;
            }
        }
        else if (recvBytes < 0)
        {
            if (errno == EWOULDBLOCK || errno == EAGAIN)
            {
                // Timeout occurred (after 500 milliseconds)
                cout << "TIMEOUT OCCURED" << endl;
                return false;
            }
            else
            {
                // Other error occurred
                cout << "Error receiving packet: " << strerror(errno) << endl;
                return false;
            }
        }
    }
}

void sendData(int sock, struct sockaddr_in &receiverAddr, int windowSize, ifstream &inputFile, ofstream &log)
{
    char buffer[MAX_DATA_SIZE];
    unsigned int base = 0;
    unsigned int nextSeqNum = 0;
    PacketHeader ackPacket;
    // socklen_t addrLen = sizeof(receiverAddr);
    vector<vector<char> > bufferedPackets(windowSize);
    while (!inputFile.eof() || base != nextSeqNum)
    {
        cout << "BASE: " << base << endl;
        cout << "NEXTSEQNUM: " << nextSeqNum << endl;
        while (nextSeqNum < base + windowSize && !inputFile.eof())
        {
            inputFile.read(buffer, MAX_DATA_SIZE);
            int bytesRead = inputFile.gcount();
            if (bytesRead > 0)
            {
                PacketHeader dataPacket;
                dataPacket.type = htonl(2);
                dataPacket.seqNum = htonl(nextSeqNum);
                dataPacket.length = htonl(bytesRead);
                dataPacket.checksum = htonl(crc32(buffer, bytesRead));

                vector<char> packet(HEADER_SIZE + bytesRead);
                memcpy(packet.data(), &dataPacket, HEADER_SIZE);
                memcpy(packet.data() + HEADER_SIZE, buffer, bytesRead);

                bufferedPackets[nextSeqNum % windowSize] = packet;
                cout << "SENDING PACKET: " << nextSeqNum << endl;
                sendto(sock, packet.data(), HEADER_SIZE + bytesRead, 0, (struct sockaddr *)&receiverAddr, sizeof(receiverAddr));
                dataPacket.type = ntohl(dataPacket.type);
                dataPacket.seqNum = ntohl(dataPacket.seqNum);
                dataPacket.length = ntohl(dataPacket.length);
                dataPacket.checksum = ntohl(dataPacket.checksum);
                output(log, dataPacket);

                nextSeqNum++;
            }
        }
        cout << "WAITING FOR ACK" << endl;
        bool ackReceived = waitForAck(sock, receiverAddr, ackPacket, base, 500, false);
        output(log, ackPacket);
        if (ackReceived)
        {
            cout << "DATA ACK RECIEVED" << endl;
            base = ackPacket.seqNum;
        }
        else
        {
            cout << "RESENDING PACKETS" << endl;
            for (unsigned int i = base; i < nextSeqNum; i++)
            {
                cout << "RESENDING PACKET: " << i << endl;
                vector<char> &packet = bufferedPackets[i % windowSize];
                output(log, ackPacket);
                sendto(sock, packet.data(), packet.size(), 0, (struct sockaddr *)&receiverAddr, sizeof(receiverAddr));
            }
        }
    }
    cout << "DONE SENDING DATA" << endl;
}

void sendStartOrEnd(int sock, struct sockaddr_in &receiverAddr, bool isStart, ofstream &log)
{
    PacketHeader packet;
    PacketHeader ack;

    packet.type = isStart ? 0 : 1;
    packet.seqNum = getRandomNumber();
    packet.length = 0;
    packet.checksum = 0;
    packet.checksum = 0; // Checksum is only calculated for packets with data

    // Send the START or END packet with timeout and retransmission
    bool ackReceived = false;
    while (!ackReceived)
    {
        output(log, packet);
        packet.type = htonl(packet.type);
        packet.seqNum = htonl(packet.seqNum);
        sendto(sock, &packet, sizeof(packet), 0, (struct sockaddr *)&receiverAddr, sizeof(receiverAddr));
        // Wait for ACK with 500ms timeout
        ackReceived = waitForAck(sock, receiverAddr, ack, ntohl(packet.seqNum), 500, true);
        // ack.type = ntohl(ack.type);
        // ack.seqNum = ntohl(ack.seqNum);
        output(log, ack);
    }
}

void startConnection(int port, const char *ip, int windowSize, ifstream &inputFile, ofstream &log)
{
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in receiverAddr;
    memset(&receiverAddr, 0, sizeof(receiverAddr));
    receiverAddr.sin_family = AF_INET;
    receiverAddr.sin_port = htons(port);
    inet_pton(AF_INET, ip, &receiverAddr.sin_addr);

    // Send START
    cout << "SENDING START" << endl;
    sendStartOrEnd(sock, receiverAddr, true, log);

    // Send DATA packets
    sendData(sock, receiverAddr, windowSize, inputFile, log);

    // SEND END
    cout << "SENDING END" << endl;
    sendStartOrEnd(sock, receiverAddr, false, log);

    close(sock);
}

int main(int argc, char *argv[])
{
    if (argc != 6)
    {
        cerr << "WRONG NUMBER OF ARGS" << endl;
        return 1;
    }
    const char *receiverIP = argv[1];
    int receiverPort = atoi(argv[2]);
    int windowSize = atoi(argv[3]);
    const char *inputFile = argv[4];
    const char *logFile = argv[5];

    ifstream input(inputFile, ios::binary);
    ofstream log(logFile, ios::out);

    startConnection(receiverPort, receiverIP, windowSize, input, log);

    input.close();
    log.close();

    return 0;
}