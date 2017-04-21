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

#define RWS 4

unsigned char seq_num;      // Sequence number
unsigned char NFE = 0;       // Next frame Expected
unsigned char LFA = 0;       // Last frame acceptable


// void signalHandler(int val) {
// 	printf("Closing...\n");
// 	close(udpPort);
//     exit(1);
// }


/**
receive message
get the sequence number
if seq_num==expected: increment expected and write to file
else: throw away
send ack + seq_num
**/

void reliablyReceive(char * myUDPport, char* destinationFile) {
    seq_num = malloc(1);
    struct addrinfo hints, *result;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET; // INET for IPv4
    hints.ai_socktype =  SOCK_DGRAM;
    hints.ai_flags =  AI_PASSIVE;

    getaddrinfo(NULL, myUDPport, &hints, &result);

	int sockfd = socket(result->ai_family, result->ai_socktype, result->ai_protocol);

    if (bind(sockfd, result->ai_addr, result->ai_addrlen) != 0) {
        perror("bind()");
        exit(1);
    }
    struct sockaddr_storage addr;
    int addrlen = sizeof(addr);
    char buf[1472];

    FILE * output_file = fopen(destinationFile, "w+");

    while(1)
    {
        ssize_t byte_count = recvfrom(sockfd, buf, sizeof(buf), 0, (struct sockaddr *)&addr, &addrlen);

        printf("Received message of length %zi: %s\n", byte_count, buf);

        memcpy(&seq_num, &buf[0], 1);

        printf("Sequence number: %u\n", seq_num);

        if(seq_num == NFE)
        {
            NFE++;
            fwrite(buf+1, 1, byte_count-1, output_file); // Skip the sequence number and write the rest
            fflush(output_file);
        }

        char *buffer = malloc(4);
        sprintf(buffer, "ack%u", seq_num);
        sendto(sockfd, buffer, strlen(buffer), 0, (struct sockaddr *)&addr, addrlen);
    }

}

int main(int argc, char** argv) {
	if (argc != 3)
	{
		fprintf(stderr, "usage: %s UDP_port filename_to_write\n\n", argv[0]);
		exit(1);
	}

	// signal(SIGINT, signalHandler);

	// unsigned short int udpPort;
	// udpPort = (unsigned short int)atoi(argv[1]);

	reliablyReceive(argv[1], argv[2]);
}

/**
receive message
get the sequence number
if seq_num==expected: increment expected and write to file
else: throw away
send ack + seq_num
**/