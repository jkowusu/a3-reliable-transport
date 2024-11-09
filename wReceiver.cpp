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

using namespace std;

bool istheexpected(const PacketHeader &header, unsigned int sequenceNumber)
{
    return (header.seqNum) == sequenceNumber;
}

bool process(PacketHeader *header, char *buffer, ofstream &outputfile,
             unsigned int &sequenceNumber, unsigned int windowSize,
             map<unsigned int, vector<char> > &packetBuffer)
{
    unsigned int seqNum = (header->seqNum);
    unsigned int length_of_header = (header->length);
    if (seqNum >= sequenceNumber + windowSize)
    {
        cerr << "Dropping packet with seqNum: " << seqNum << endl;
        return false;
    }
    if (istheexpected(*header, sequenceNumber))
    {
        outputfile.write(buffer + sizeof(PacketHeader), length_of_header);
        sequenceNumber += 1;
        while (packetBuffer.count(sequenceNumber))
        {
            outputfile.write(packetBuffer[sequenceNumber].data(), packetBuffer[sequenceNumber].size());
            packetBuffer.erase(sequenceNumber);
            sequenceNumber += 1;
        }
        return true;
    }
    else if (seqNum > sequenceNumber)
    {
        packetBuffer[header->seqNum] = vector<char>(buffer + sizeof(PacketHeader),
                                                    buffer + sizeof(PacketHeader) + length_of_header);
        cerr << "Buffered out-of-order packet with seqNum: " << seqNum << endl;
        return true;
    }

    return false;
}

void log(ofstream &logFile, const PacketHeader &header)
{
    logFile << (header.type) << " " << (header.seqNum)
            << " " << (header.length) << " " << (header.checksum) << endl;
}

// PacketHeader maketheack(unsigned int sequenceNumber)
// {
//     PacketHeader ack = {htonl(3), htonl(sequenceNumber), 0, 0};
//     cout << "Sending ACK for next expected seqNum: " << sequenceNumber << endl;
//     return ack;
// }

void receiver(int sockfd, sockaddr_in &clientAddr, socklen_t &addrLen, unsigned int windowSize, const string &outputDirectory, ofstream &logFile)
{
    unsigned int sequence_number;
    char buffer[MAX_PACKET_SIZE];
    int indexFile = 0;
    map<unsigned int, vector<char> > packetBuffer;

    while (true)
    {
        string outputname = outputDirectory + "/FILE-" + to_string(indexFile) + ".out";
        ofstream output_file;
        while (true)
        {
            recvfrom(sockfd, buffer, MAX_PACKET_SIZE, 0, (sockaddr *)&clientAddr, &addrLen);
            PacketHeader *header = (PacketHeader *)buffer;
            header->type = ntohl(header->type);
            header->seqNum = ntohl(header->seqNum);
            header->length = ntohl(header->length);
            header->checksum = ntohl(header->checksum);
            if ((header->type) == 0)
            {
                // CREATE FILE AFTER START RECIEVED
                sequence_number = 0;
                packetBuffer.clear();
                output_file = ofstream(outputname, ios::binary);
                if (!output_file.is_open())
                {
                    cerr << "Error opening output file: " << outputname << " - " << strerror(errno) << endl;
                    return;
                }
                cout << "Received START packet" << endl;
                PacketHeader ack = {3,header->seqNum, 0, 0};
                cout << "Sending ACK for START packet" << endl;
                log(logFile, ack);
                ack.type = htonl(ack.type);
                ack.seqNum = htonl(ack.seqNum);
                sendto(sockfd, &ack, sizeof(ack), 0, (sockaddr *)&clientAddr, addrLen);
                break;
            }
            else
            {
                cout << "Must start connection first" << endl;
            }
        }

        while (true)
        {
            ssize_t length_of_recv = recvfrom(sockfd, buffer, MAX_PACKET_SIZE, 0, (sockaddr *)&clientAddr, &addrLen);
            PacketHeader *header = (PacketHeader *)buffer;
            header->type = ntohl(header->type);
            header->seqNum = ntohl(header->seqNum);
            header->length = ntohl(header->length);
            header->checksum = ntohl(header->checksum);
            if (length_of_recv < 0)
            {
                perror("ERROR");
                continue;
            }
            unsigned int val = (header->checksum);
            log(logFile, *header);
            unsigned int payload_length = length_of_recv - sizeof(PacketHeader);
            // Verify checksum on payload only
            unsigned int checksum = crc32(buffer + sizeof(PacketHeader), payload_length);

            if (checksum != val)
            {
                cout << "ERROR: Checksum mismatch." << endl;
                continue;
            }
            // START
            else if ((header->type) == 0)
            {
                cout << "Connection already going" << endl;
            }
            // END
            else if ((header->type) == 1)
            {
                cout << "Received END packet" << endl;
                PacketHeader ack = {3, header->seqNum, 0, 0};
                log(logFile, ack);
                ack.type = htonl(ack.type);
                ack.seqNum = htonl(ack.seqNum);
                cout << "Sending ACK for END packet" << endl;
                sendto(sockfd, &ack, sizeof(ack), 0, (sockaddr *)&clientAddr, addrLen);
                while (packetBuffer.count(sequence_number))
                {
                    output_file.write(packetBuffer[sequence_number].data(), packetBuffer[sequence_number].size());
                    packetBuffer.erase(sequence_number);
                    sequence_number += 1;
                }
                break;
            }
            // DATA
            else if ((header->type) == 2)
            {
                cout << "Received DATA packet with seqNum: " << (header->seqNum) << endl;
                cout << "Sending data to: " << outputname << endl;
                if (process(header, buffer, output_file, sequence_number, windowSize, packetBuffer))
                {
                    PacketHeader ack = {3, sequence_number, 0, 0};
                    log(logFile, ack);
                    ack.type = htonl(ack.type);
                    ack.seqNum = htonl(ack.seqNum);
                    sendto(sockfd, &ack, sizeof(ack), 0, (sockaddr *)&clientAddr, addrLen);
                }
            }
        }

        output_file.close();
        indexFile += 1;
    }
}

int main(int argc, char *argv[])
{
    if (argc != 5)
    {
        cerr << "WRONG NUMBER OF ARGS" << endl;
        return 1;
    }

    int port = atoi(argv[1]);
    int windowSize = atoi(argv[2]);
    string outputDirectory = argv[3];
    ofstream logFile(argv[4], ios::binary);

    if (!logFile.is_open())
    {
        cerr << "Log File ERROR" << endl;
        return 1;
    }

    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0)
    {
        perror("SOCKET ERROR");
        return 1;
    }

    sockaddr_in serverAddr, clientAddr;
    memset(&serverAddr, 0, sizeof(serverAddr));
    memset(&clientAddr, 0, sizeof(clientAddr));
    socklen_t addrLen = sizeof(clientAddr);
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(port);
    serverAddr.sin_addr.s_addr = INADDR_ANY;

    if (::bind(sockfd, (const struct sockaddr *)&serverAddr, sizeof(serverAddr)) == -1) {
    perror("BIND ERROR");
    close(sockfd);
    return 1;
}


    cout << "Receiver running..." << endl;
    receiver(sockfd, clientAddr, addrLen, windowSize, outputDirectory, logFile);
    close(sockfd);
    logFile.close();
    return 0;
}