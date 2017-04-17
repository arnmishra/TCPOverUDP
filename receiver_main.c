#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <signal.h>
#include <netdb.h>

#include "common.h"

// void signalHandler(int val) {
// 	printf("Closing...\n");
// 	close(udpPort);
//     exit(1);
// }

void reliablyReceive(unsigned short int myUDPport, char* destinationFile) {
}

int main(int argc, char** argv) {
	if (argc != 3)
	{
		fprintf(stderr, "usage: %s UDP_port filename_to_write\n\n", argv[0]);
		exit(1);
	}

	// signal(SIGINT, signalHandler);

	unsigned short int udpPort;
	udpPort = (unsigned short int)atoi(argv[1]);


	// LOl fucking shit bruh

	int s;

    struct addrinfo hints, *result;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET; // INET for IPv4
    hints.ai_socktype =  SOCK_DGRAM;
    hints.ai_flags =  AI_PASSIVE;

    getaddrinfo(NULL, "7777", &hints, &result);

    int sockfd = socket(result->ai_family, result->ai_socktype, result->ai_protocol);

    if (bind(sockfd, result->ai_addr, result->ai_addrlen) != 0) {
        perror("bind()");
        exit(1);
    }
    struct sockaddr_storage addr;
    int addrlen = sizeof(addr);

    while(1){
        char buf[1024];
        printf("Waiting to recv...\n");
        ssize_t byte_count = recvfrom(sockfd, buf, sizeof(buf), 0, (struct sockaddr *)&addr, &addrlen);
        buf[byte_count] = '\0';

        printf("Read %d chars\n", byte_count);
        printf("===\n");
        printf("%s\n", buf);

        // printf("\n\nTrying to send message back?\n");

        // char *buffer = "COME BACK NIGGA\n";
        // sendto(sockfd, buffer, strlen(buffer), 0, (struct sockaddr *)&addr, &addrlen);
    }

	// reliablyReceive(udpPort, argv[2]);
}

/**
receive message
get the sequence number
if seq_num==expected: increment expected and write to file
else: throw away
send ack + seq_num
**/