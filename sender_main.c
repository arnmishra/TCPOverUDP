#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <signal.h>
#include <netdb.h>
#include <pthread.h>

#include "common.h"

#define MAX_PACKET_SIZE 1472
#define MAX_DATA_SIZE 1471 // 1472B payload - 1B for sequence number

#define SWS 4
#define MAX_SEQUENCE_NUM (2 * SWS)

int sockfd;

unsigned char LAR = 0;		// Last acknowledged frame
unsigned char LFS = 0;		// Last frame sent
unsigned char seq_num;		// Sequence number

// NOT USED YET
// Store the last packets sent in the window (if resend needed)
typedef struct packet {
	int seq_num;
	char data[1500];
} packet;

// Argument for the pthread function receiveAcknowledgements
typedef struct acknowledgementThreadArg {
	unsigned short int udpPort;
	struct addrinfo *p;
} acknowledgementThreadArg;

void signalHandler(int val) {
	printf("Closing...\n");
	close(sockfd);
    exit(1);
}

void *receiveAcknowledgements(void *ptr) {
	acknowledgementThreadArg *arg = ptr;
	unsigned short int udpPort = arg->udpPort;
	struct addrinfo *p = arg->p;

	while (1) {
		char buf[10];
	    ssize_t byte_count = recvfrom(sockfd, buf, sizeof(buf), 0, p->ai_addr, &p->ai_addrlen);
	    buf[byte_count] = '\0';

	    printf("ACK: %s\n", buf);
	}
}

void *waitAndTimeout(void *ptr) {
	printf("Waiting... and timing out...\n");
}

// while seq_num < LAR + SWS && bytesToTransfer > 0: <-- mod this
// if bytesToTransfer > 1471:
// 	read in 1471 bytes for message
// 	bytesToTransfer -= 1471
// else
// 	read in bytesToTransfer
// add seq_num to start of message
// increment seq_num (w/ mod)
// send
void reliablyTransfer(char* hostname, unsigned short int hostUDPport, char* filename, unsigned long long int bytesToTransfer, struct addrinfo *p) {
	FILE *f = fopen(filename, "r");

	if (f == NULL) {
		printf("ERR: File %s does not exist\n", filename);
		exit(1);
	}

	while (bytesToTransfer > 0) {
		while (seq_num < (LAR + SWS) % MAX_SEQUENCE_NUM && bytesToTransfer > 0) {
			char buf[MAX_PACKET_SIZE];
			char *ptr = buf + 1;
			if (bytesToTransfer > MAX_DATA_SIZE) {
				size_t bytesRead = fread(ptr, MAX_DATA_SIZE, 1, f);
				bytesToTransfer -= MAX_DATA_SIZE;
				printf("bytesToTransfer now = %lld\n", bytesToTransfer);
			}
			else {
				size_t bytesRead = fread(ptr, bytesToTransfer, 1, f);
				bytesToTransfer = 0;
			}

			memcpy(buf, &seq_num, 1);
			seq_num = (seq_num + 1) % MAX_SEQUENCE_NUM;

			int numBytes;
			printf("Sending out a packet with seq_num %d\n", (int)seq_num);
			if ((numBytes = sendto(sockfd, buf, sizeof(buf), 0, p->ai_addr, p->ai_addrlen)) == -1) {
		        perror("sendto");
		        exit(1);
		    }
		}

		// Conditional wait here until an ACK is received, or something timesOut
	}

	fclose(f);
}

int main(int argc, char** argv)
{
	if(argc != 5) {
		fprintf(stderr, "usage: %s receiver_hostname receiver_port filename_to_xfer bytes_to_xfer\n\n", argv[0]);
		exit(1);
	}
	signal(SIGINT, signalHandler);

	char *hostname = argv[1];
	char *portNum  = argv[2];
	char *filename = argv[3];
	// printf("hostname = %s; portnum = %s\n", hostname, portNum);

	// lol fuck that idk
	struct addrinfo hints, *servinfo, *p;
	int rv;
	int numbytes;

	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_DGRAM;

	if ((rv = getaddrinfo(hostname, portNum, &hints, &servinfo)) != 0) {
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

	char *buffer = "BRUH!";
    if ((numbytes = sendto(sockfd, buffer, strlen(buffer), 0, p->ai_addr, p->ai_addrlen)) == -1) {
        perror("sendto");
        exit(1);
    }

    seq_num = 0;

    unsigned short int udpPort = (unsigned short int)atoi(portNum);
	unsigned long long int numBytes = atoll(argv[4]);

	// Create thread for receiving ACKs
	acknowledgementThreadArg arg = { .udpPort = udpPort, .p = p };
	pthread_t ack_id;
	if (pthread_create(&ack_id, NULL, receiveAcknowledgements, &arg) != 0) {
		perror("pthread_create");
		exit(1);
	}

	// Create thread for waiting and timeouts
	pthread_t timeout_id;
	if (pthread_create(&timeout_id, NULL, waitAndTimeout, NULL) != 0) {
		perror("pthread_create");
		exit(1);
	}

	reliablyTransfer(hostname, udpPort, filename, numBytes, p);

    freeaddrinfo(servinfo);

    void *result;
    pthread_join(ack_id, &result);
}

/**
receive_thread
while True:
	listen for message from receiver
	LAR = ack_val
	broadcast to wake up conditional variable

send_thread
while seq_num < LAR + SWS && bytesToTransfer > 0: <-- mod this
	if bytesToTransfer > 1471:
		read in 1471 bytes for message
		bytesToTransfer -= 1471
	else
		read in bytesToTransfer
	add seq_num to start of message
	increment seq_num (w/ mod)
	send

wait_thread:
conditionally wait 100ms
	restart send_thread
if woken up, restart conditionally wait
**/