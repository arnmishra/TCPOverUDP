#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <signal.h>
#include <netdb.h>
#include <pthread.h>
#include <time.h>

#define MAX_PACKET_SIZE 1472
#define MAX_DATA_SIZE 1471 // 1472B payload - 1B for sequence number

#define SWS 4
#define NUM_SEQ_NUM (2 * SWS)

int sockfd;

char LAR = -1;	// Last acknowledged frame
char LFS = 0;	// Last frame sent
char seq_num;	// Sequence number

// Synchronization
pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;

// NOT USED YET
// Store the last packets sent in the window (if resend needed)
typedef struct packet {
	int packet_id;
	char seq_num;
	char data[MAX_DATA_SIZE];
	time_t send_time;
	bool active;
	struct packet *next;
} packet_t;
packet_t *head;
packet_t *tail;

// Argument for the pthread function receiveAcknowledgements
typedef struct acknowledgementThreadArg {
	unsigned short int udpPort;
	struct addrinfo *p;
} acknowledgementThreadArg;

typedef struct thread {
	pthread_t pid;
	int id;
} thread_t;

void SIGINT_handler(int val) {
	printf("Closing...\n");

	close(sockfd);

	pthread_cond_destroy(&cv);
	pthread_mutex_destroy(&m);

	// Free the linked list
	while (head != NULL) {
		packet_t *ptr = head->next;
		free(head);
		head = ptr;
	}

    exit(1);
}

void checkForTimeouts(int val) {
	printf("Checking for timeouts...\n");
	packet_t *packet = head;

	time_t now = time(0);
	while (packet != NULL) {
		if (packet->active) {
			double diff = difftime(now, packet->send_time) * 1000; // x1000 for ms
			if (diff > 100) {
				printf("Packet %u timed out! Resending all n buffers...\n", packet->packet_id);
				pthread_cond_broadcast(&cv);
				break;
			}
			else
				break;
		}
		packet = packet->next;
	}
}

/*
 * Given an ack number, mark the packet as inactive in the window list
 */
void markPacketAsInactive(int ack_num) {
	packet_t *packet = head;

	while (packet != NULL) {
		if (packet->seq_num == ack_num) {
			packet->active = false;
			break;
		}
		packet = packet->next;
	}
}

/*
 * Thread for receiving acknowledgements, wakes up the sleeping transfer thread
 */
void *receiveAcknowledgements(void *ptr) {
	acknowledgementThreadArg *arg = ptr;
	unsigned short int udpPort = arg->udpPort;
	struct addrinfo *p = arg->p;

	while (1) {
		char buf[10];
	    ssize_t byte_count = recvfrom(sockfd, buf, sizeof(buf), 0, p->ai_addr, &p->ai_addrlen);
	    buf[byte_count] = '\0';

	    char ack_num;
	    memcpy(&ack_num, &buf[3], 1);
	    ack_num -= 48;

	    if (ack_num == (LAR + 1) % NUM_SEQ_NUM) {
	    	printf("Received valid packet (%s)\n", buf);
	    	markPacketAsInactive(ack_num);

	    	LAR = (LAR + 1) % NUM_SEQ_NUM;
	    	pthread_cond_broadcast(&cv);
	    }
	    else {
	    	printf("Received random ack (%s)\n", buf);
	    }

	}

	return NULL;
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
	FILE *f = fopen(filename, "rb");

	if (f == NULL) {
		printf("ERR: File %s does not exist\n", filename);
		exit(1);
	}

	int id = 0;
	while (bytesToTransfer > 0) {
		pthread_mutex_lock(&m);

		packet_t *packet = head;
		while (( (seq_num > LAR && seq_num <= (LAR + SWS) % NUM_SEQ_NUM)  || (seq_num < LAR && LAR + SWS >= NUM_SEQ_NUM && seq_num <= (LAR + SWS) % NUM_SEQ_NUM)) && bytesToTransfer > 0) {
			// printf("seq_num = %d, LAR = %d, SWS = %d\n", seq_num, LAR, SWS);
			char buf[MAX_PACKET_SIZE];
			char *ptr = buf + 1;
			size_t bytesRead;
			if (bytesToTransfer > MAX_DATA_SIZE) {
				bytesRead = fread(ptr, 1, MAX_DATA_SIZE, f);
				//printf("1: %i\n", fileNotFinished);
				//bytesRead = MAX_DATA_SIZE;
				bytesToTransfer -= MAX_DATA_SIZE;
			}
			else {
				bytesRead = fread(ptr, 1, bytesToTransfer, f);
				//printf("2: %i\n", fileNotFinished);
				//bytesRead = bytesToTransfer;
				bytesToTransfer = 0;
			}

			memcpy(buf, &seq_num, 1);

			// Update the window linked list
			packet->packet_id = id;
			packet->seq_num = seq_num;
			memcpy(packet->data, ptr, bytesRead);
			packet->send_time = time(0);
			packet->active = true;
			packet = packet->next;

			int numBytes;
			fflush(stdout);
			// printf("Sending out packet (%d)\n", seq_num);
			if ((numBytes = sendto(sockfd, buf, bytesRead+1, 0, p->ai_addr, p->ai_addrlen)) == -1) {
		        perror("sendto");
		        exit(1);
		    }

		    // ualarm(5000, 0);
		    // alarm(1);

			seq_num = (seq_num + 1) % NUM_SEQ_NUM;

			id += 1;
		}
		// printf("seq_num = %d, LAR = %d, SWS = %d\n", seq_num, LAR, SWS);

		// Conditional wait here until an ACK is received, or something timesOut
		// printf("rT: Sleeping with %lld to send...\n", bytesToTransfer);
		pthread_cond_wait(&cv, &m);
		// printf("rT: Woken up!...\n");
		pthread_mutex_unlock(&m);
	}

	fclose(f);
}

int main(int argc, char** argv)
{
	if(argc != 5) {
		fprintf(stderr, "usage: %s receiver_hostname receiver_port filename_to_xfer bytes_to_xfer\n\n", argv[0]);
		exit(1);
	}
	signal(SIGINT, SIGINT_handler);
	signal(SIGALRM, checkForTimeouts);

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

    seq_num = 0;

    unsigned short int udpPort = (unsigned short int)atoi(portNum);
	unsigned long long int numBytes = atoll(argv[4]);

	// Initialize the global linked list of the send window
	int i;
	head = malloc(sizeof(packet_t));
	packet_t *prev = head;
	packet_t *cur;
	for (i = 0; i < SWS; i++) {
		cur = malloc(sizeof(packet_t));
		prev->next = cur;
		prev = cur;
	}
	tail = prev;

	// Create thread for receiving ACKs
	acknowledgementThreadArg arg = { .udpPort = udpPort, .p = p };
	pthread_t ack_id;
	if (pthread_create(&ack_id, NULL, receiveAcknowledgements, &arg) != 0) {
		perror("pthread_create");
		exit(1);
	}

	// // Create thread for waiting and timeouts
	// thread_t *tid = calloc(SWS, sizeof(thread_t));
	// for (i = 0; i < SWS; i++) {
	// 	tid[i].id = i;
	// 	pthread_create(&tid[i].pid, NULL, waitAndTimeout, &tid[i]);
	// }

	reliablyTransfer(hostname, udpPort, filename, numBytes, p);

	// Done sending shit out, let the receiver know to stop
	char *buf = "FIN";
	if ((numbytes = sendto(sockfd, buf, strlen(buf), 0, p->ai_addr, p->ai_addrlen)) == -1) {
		perror("SEND :(\n");
		exit(1);
	}

    freeaddrinfo(servinfo);
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