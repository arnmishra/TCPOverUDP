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

unsigned short int globalSocketUDP;

// void signalHandler(int val) {
// 	printf("Closing...\n");
// 	close(udpPort);
//     exit(1);
// }

void reliablyTransfer(char* hostname, unsigned short int hostUDPport, char* filename, unsigned long long int bytesToTransfer) {
}

int main(int argc, char** argv)
{
	if(argc != 5) {
		fprintf(stderr, "usage: %s receiver_hostname receiver_port filename_to_xfer bytes_to_xfer\n\n", argv[0]);
		exit(1);
	}

	// signal(SIGINT, signalHandler);

	// lol fuck that idk
	int sockfd;
	struct addrinfo hints, *servinfo, *p;
	int rv;
	int numbytes;

	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_DGRAM;

	if ((rv = getaddrinfo("127.0.0.1", "7777", &hints, &servinfo)) != 0) {
		fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
		return 1;
	}

	// loop through all the results and make a socket
	for (p = servinfo; p != NULL; p = p->ai_next) {
		if ((sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) == -1) {
			perror("talker: socket");
			continue;
		}
		break;
	}

	if (p == NULL) {
		fprintf(stderr, "talker: failed to create socket\n");
		return 2;
	}

	char *buf = "BRUH!";
	if ((numbytes = sendto(sockfd, buf, strlen(buf), 0, p->ai_addr, p->ai_addrlen)) == -1) {
		perror("sendto");
		exit(1);
	}

	freeaddrinfo(servinfo);


	// reliablyTransfer(receiverHostname, receiverPort, filename, numBytes);

	// sendMessage(0, "msg", 2);
}
