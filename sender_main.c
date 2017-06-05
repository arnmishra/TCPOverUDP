/**
FILENAME: sender_main.c

DESCRIPTION: Sender for TCP Connection built over UDP protocol.

AUTHORS:
    Arnav Mishra
    Samir Chaudhry
*/

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
#include <sys/time.h>

#define MAX_PACKET_SIZE 1472
#define MAX_DATA_SIZE 1471 // 1472B payload - 1B for sequence number

int SWS = 60;
#define NUM_SEQ_NUM (2 * SWS)

int sockfd;

char LAR = -1;	// Last acknowledged frame
char LFS = 0;	// Last frame sent

// RTT Estimation
struct sigaction sa;
struct itimerval SRTT;
struct itimerval SRTT_sleep;
struct timeval RTT;
double alpha = 0.125;

// Synchronization
pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
bool send_check = false;

pthread_cond_t cv_timeout = PTHREAD_COND_INITIALIZER;
pthread_mutex_t m_timeout = PTHREAD_MUTEX_INITIALIZER;
bool timeout_check = false;

pthread_cond_t cv_packets = PTHREAD_COND_INITIALIZER;
pthread_mutex_t m_packets = PTHREAD_MUTEX_INITIALIZER;

// Store the last packets sent in the window (if resend needed)
typedef struct packet {
	int packet_id;
	char seq_num;
	char *data;
	int num_bytes;
	struct timeval send_time;
	struct packet *next;
	struct packet *prev;
} packet_t;
packet_t *head;
packet_t *tail;

// Argument for the pthread function receiveAcknowledgements
typedef struct thread_arg {
	unsigned short int udpPort;
	struct addrinfo *p;
} thread_arg_t;

typedef struct thread {
	pthread_t pid;
	int id;
} thread_t;

/**
Function: SIGINT_handler

Purpose: Signal Handler for SIGINT that destroys the condition variables and mutexes.
*/
void SIGINT_handler() {
	PRINT(("Closing...\n"));

	close(sockfd);

	pthread_cond_destroy(&cv);
	pthread_mutex_destroy(&m);
    pthread_cond_destroy(&cv_timeout);
    pthread_mutex_destroy(&m_timeout);

    exit(1);
}

/**
Function: awakenTimeoutThread

Purpose: Function to signal the timeout thread to wake it up and resend relevant packets.
*/
void awakenTimeoutThread() {
    pthread_mutex_lock(&m_timeout);
    timeout_check = true;
    pthread_cond_signal(&cv_timeout);
    pthread_mutex_unlock(&m_timeout);
}

/**
Function: checkForTimeouts

Purpose: Check for timeouts in the linked list to resend relevant packets.

Parameters:
    ptr -- UDP Port and address information of what receiver to resend data to. 
*/
void *checkForTimeouts(void *ptr) {
	thread_arg_t *arg = ptr;
	struct addrinfo *p = arg->p;

    while (true) {
        // Sleep until SIGALRM sets timeout_check flag.
        pthread_mutex_lock(&m_timeout);
        while (!timeout_check) {
            pthread_cond_wait(&cv_timeout, &m_timeout);
        }

        timeout_check = false;
        pthread_mutex_unlock(&m_timeout);

        printf("Checking for timeouts...\n");

        // Iterate through all packets checking for timeouts.
        pthread_mutex_lock(&m_packets);
        packet_t *packet = head;

        if (!packet) {
            send_check = true;
            pthread_cond_broadcast(&cv);
        }
        else {
            struct timeval now;
            gettimeofday(&now, NULL);

            while (packet != NULL) {
                struct timeval diff;
                timersub(&now, &packet->send_time, &diff);
                if (timercmp(&diff, &SRTT.it_value, >=)) {
                    while (packet != NULL) {
    					if ((sendto(sockfd, packet->data, packet->num_bytes, 0, p->ai_addr, p->ai_addrlen)) == -1) {
    				        perror("sendto");
    				        exit(1);
    				    }
                        gettimeofday(&packet->send_time, NULL);
    				    setitimer(ITIMER_REAL, &SRTT_sleep, NULL);
    				    packet = packet->next;
                    }
                    break;
                }
                // Keep resending till a packet that is not timed out since everything after that packet cannot be timed out yet.
                else
                {
                    break;
                }
                packet = packet->next;
            }
        }
        pthread_mutex_unlock(&m_packets);
    }

}

/**
Function: insert_data

Purpose: Insert new data that is received into the linked list

Parameters:
    buf -- The buffer that has been received from the sender.
    packet_id -- ID of the packet that was sent out. 
    sequence_num -- The Sequence Number of this buffer (used to insert in order).
    byte_count -- The number of bytes in this buffer.
*/
void insert_data(char *buf, int packet_id, int sequence_num, ssize_t byte_count)
{
	pthread_mutex_lock(&m_packets);

    packet_t *node = malloc(sizeof(packet_t));
    node->seq_num = sequence_num;
    node->packet_id = packet_id;
    gettimeofday(&node->send_time, NULL);
    node->data = malloc(byte_count);
    node->num_bytes = byte_count;
    memcpy(node->data, buf, byte_count);
    if(head)
    {
        bool inserted = false;
        packet_t *temp = head;
        while(temp)
        {
            // Insert Node Before Temp.
            if(node->seq_num < temp->seq_num && (temp->seq_num - SWS) < node->seq_num)
            {
                node->next = temp;
                node->prev = temp->prev;
                if(temp->prev)
                {
                    temp->prev->next = node;
                }
                temp->prev = node;
                inserted = true;
                if(temp == head)
                {
                    head = node;
                }
                break;
            }
            temp = temp->next;
        }
        // Insert Node at tail.
        if(!inserted)
        {
            tail->next = node;
            node->prev = tail;
            node->next = NULL;
            tail = node;
        }
    }
    // Insert into empty list.
    else
    {
        head = node;
        head->next = NULL;
        head->prev = NULL;
        tail = head;
    }

    pthread_mutex_unlock(&m_packets);
}

/**
Function: getMicrotime

Purpose: Function to get the current time in microseconds.

Parameters:
    time -- The current time in a timeval struct.

Returns: Time in microseconds. 
*/
long long unsigned int getMicrotime(struct timeval time){
    return (time.tv_sec * (long long unsigned int)1000000) + (time.tv_usec);
}

/**
Function: estimateNewRTT

Purpose: Function to estimate the new RTT using SRTT equations. This SRTT is used to
    approximate the timeout length. 

Parameters:
    packet -- The packet who's time of flight is being used to calculate the new SRTT.
*/
void estimateNewRTT(packet_t *packet) {
    struct timeval now;
    gettimeofday(&now, NULL);

    timersub(&now, &packet->send_time, &RTT);
    RTT.tv_usec *= 2;

    long long unsigned int SRTT_ms = getMicrotime(SRTT.it_value);
    long long unsigned int RTT_ms = getMicrotime(RTT);

    double new_SRTT_ms = (alpha * (double)SRTT_ms) + ((1 - alpha) * (double)RTT_ms);

    SRTT.it_value.tv_sec = (new_SRTT_ms / (int)1e6);
    SRTT.it_value.tv_usec = ((int)new_SRTT_ms % (int)1e6);

    SRTT_sleep.it_value.tv_sec = SRTT.it_value.tv_sec;
    SRTT_sleep.it_value.tv_usec = (SRTT.it_value.tv_usec) * 2;

    if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &SRTT_sleep.it_value, sizeof(SRTT_sleep.it_value)) < 0) {
        perror("setsockopt");
    }
}

/**
Function: markPacketAsInactive

Purpose: Given an ack number, mark all packets up to that ack as inactive in the window list.

Parameters:
    cum_ack -- Acknowledgement number up till which everything should be marked inactive.
*/
void markPacketAsInactive(int cum_ack) {
	packet_t *next;
    while (head && ((head->seq_num <= cum_ack && (head->seq_num + SWS) > cum_ack) || (head->seq_num - SWS) > cum_ack))
	{
        if (head->seq_num == cum_ack)
            estimateNewRTT(head);
		
        next = head->next;
        free(head->data);
        free(head);
        head = next;

        if(head)
        {
        	head->prev = NULL;
        }
		LAR = (LAR + 1) % NUM_SEQ_NUM;
	}

    if (!head)
        tail = NULL;

	LAR = cum_ack;
}

/**
Function: receiveAcknowledgements

Purpose: Thread for receiving acknowledgements and waking up the sleeping transfer thread.

Parameters:
     ptr -- UDP Port and address information from which acknowledgements are received.

Returns: Null for success.
*/
void *receiveAcknowledgements(void *ptr) {
	thread_arg_t *arg = ptr;
	struct addrinfo *p = arg->p;

	while (1) {
		char buf[10];
	    ssize_t byte_count = recvfrom(sockfd, buf, sizeof(buf), 0, p->ai_addr, &p->ai_addrlen);

        if (byte_count > 0) {
            buf[byte_count] = '\0';

            if (buf[0] == 'F' && buf[1] == 'F') {
                PRINT(("RECEIVED ACKNOWLEDGEMENT...\n"));
                PRINT(("ENDING PROGRAM...\n"));
                break;
            }

            char *token = buf;
            char *end   = buf;

            char cum_ack;
    	    char last_seq_recv;

            bool first = true;
            while (token != NULL) {
                strsep(&end, ".");

                if (first) {
                    cum_ack = (char)atoi(token);
                    first = false;
                }
                else
                    last_seq_recv = (char)atoi(token);

                token = end;
            }
            pthread_mutex_lock(&m_packets);

            // If the cumulative ack is in the window
    	    if ((cum_ack >= (LAR + 1) && (LAR + SWS >= cum_ack)) || cum_ack <= (LAR - SWS)) {
    	    	PRINT(("1 Received ack %d.%d\n", cum_ack, last_seq_recv));
    	    	markPacketAsInactive(cum_ack);
                send_check = true;
                pthread_cond_broadcast(&cv);
                setitimer(ITIMER_REAL, &SRTT_sleep, NULL);
    	    }
            // If the select acknowledgement is later than the cumulative ack
    	    else if (last_seq_recv > cum_ack || last_seq_recv <= (cum_ack + SWS) % NUM_SEQ_NUM)
    	    {
    	    	PRINT(("2 Received ack %d.%d\n", cum_ack, last_seq_recv));
    	    	packet_t *temp = head;
    	    	while (temp && temp->seq_num != last_seq_recv)
    			{
    				temp = temp->next;
    			}
    			if(temp)
    			{
    				PRINT(("receiveAcknowledgements: Removing packet %d\n", temp->seq_num));
    				if(temp->prev)
    				{
    					temp->prev->next = temp->next;
    				}
    				if(temp->next)
    				{
    					temp->next->prev = temp->prev;
    				}
    				estimateNewRTT(temp);

                    if (temp == head)
                        head = temp->next;
                    if (temp == tail)
                    	tail = tail->prev;

    				free(temp->data);
    				free(temp);
    			}
    			send_check = true;
                pthread_cond_broadcast(&cv);
                setitimer(ITIMER_REAL, &SRTT_sleep, NULL);
    	    }
            else {
                PRINT(("Received random ack (%s)\n", buf));
            }

            pthread_mutex_unlock(&m_packets);
        }
        else {
            awakenTimeoutThread();
        }

	}

    PRINT(("ENDING\n"));

	return NULL;
}

/**
Function: reliablyTransfer

Purpose: Thread to read in data from input file and trasfer data to receiver. A sequence number is assigned to 
    each packet and the data is inserted into the linked list until an ack for it is received.

Parameters:
     filename -- Name of the file from which data is being transfered.
     bytesToTransfer -- The total number of bytes to be transmitted from the file.
     p -- The UDP Port and Address Information for the receiver. 
*/
void reliablyTransfer(char* filename, unsigned long long int bytesToTransfer, struct addrinfo *p) {
	FILE *f = fopen(filename, "rb");

	if (f == NULL) {
		PRINT(("ERR: File %s does not exist\n", filename));
		exit(1);
	}

	int id = 0;
	pthread_mutex_lock(&m);

    char seq_num = 0;

	while (bytesToTransfer > 0 || head || tail) {

		while (( (seq_num > LAR && seq_num <= (LAR + SWS))  || (seq_num < LAR && LAR + SWS >= NUM_SEQ_NUM && seq_num <= (LAR + SWS) % NUM_SEQ_NUM)) && bytesToTransfer > 0) {
			char buf[MAX_PACKET_SIZE];
			char *ptr = buf + 1;
			size_t bytesRead;
			if (bytesToTransfer > MAX_DATA_SIZE) {
				bytesRead = fread(ptr, 1, MAX_DATA_SIZE, f);
				bytesToTransfer -= MAX_DATA_SIZE;
			}
			else {
				bytesRead = fread(ptr, 1, bytesToTransfer, f);
				bytesToTransfer = 0;
			}

			memcpy(buf, &seq_num, 1);

			// Update the linked list with the latest packet.
			insert_data(buf, id, seq_num, bytesRead+1);

			PRINT(("Sending out packet (%d)\n", seq_num));

			int numBytes;
			if ((numBytes = sendto(sockfd, buf, bytesRead+1, 0, p->ai_addr, p->ai_addrlen)) == -1) {
		        perror("sendto");
		        exit(1);
		    }

		    setitimer(ITIMER_REAL, &SRTT_sleep, NULL);
			seq_num = (seq_num + 1) % NUM_SEQ_NUM;
			id += 1;
		}
		// Conditional wait here until an ACK is received, or something times out.
        while (!send_check) {
            pthread_cond_wait(&cv, &m);
        }
        send_check = false;
	}
	pthread_mutex_unlock(&m);

	fclose(f);
}

/**
Function: main

Purpose: Main thread to initialize relevant variables and start transmitting data and receiving 
    acknowledgements. 

Parameters:
    argc -- Number of arguments passed into the program.
    argv -- List of arguments that are passed in. 

Returns: 0 for successful completion. Anything else for errors.
*/
int main(int argc, char** argv)
{
    if(argc != 5) {
		PRINT(("usage: %s receiver_hostname receiver_port filename_to_xfer bytes_to_xfer\n\n", argv[0]));
		exit(1);
	}
	signal(SIGINT, SIGINT_handler);

	char *hostname = argv[1];
	char *portNum  = argv[2];
	char *filename = argv[3];

	struct addrinfo hints, *servinfo, *p;
	int rv;
	int numbytes;

	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_DGRAM;

	if ((rv = getaddrinfo(hostname, portNum, &hints, &servinfo)) != 0) {
		PRINT(("getaddrinfo: %s\n", gai_strerror(rv)));
		return 1;
	}

	// Loop through all the results and make a socket.
	for (p = servinfo; p != NULL; p = p->ai_next) {
		if ((sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) == -1) {
			perror("talker: socket");
			continue;
		}
		break;
	}

	if (p == NULL) {
		PRINT(("talker: failed to create socket\n"));
		return 2;
	}

    unsigned short int udpPort = (unsigned short int)atoi(portNum);
	unsigned long long int numBytes = atoll(argv[4]);

	// Create thread for receiving ACKs
	thread_arg_t arg = { .udpPort = udpPort, .p = p };
	pthread_t ack_id;
	if (pthread_create(&ack_id, NULL, receiveAcknowledgements, &arg) != 0) {
		perror("pthread_create");
		exit(1);
	}

	// Create thread for waiting and timeouts
	pthread_t timeout_id;
    if (pthread_create(&timeout_id, NULL, checkForTimeouts, &arg) != 0) {
        perror("pthread_create");
        exit(1);
    }

    // Initialize SRTT(0) to 1 second.
    SRTT.it_value.tv_sec = 1;
    SRTT.it_value.tv_usec = 0;
    SRTT.it_interval.tv_sec = 0;
    SRTT.it_interval.tv_usec = 0;
    // Initialize RTT(0) = 0.5 second.
    RTT = (struct timeval){0};
    RTT.tv_usec = 50000;

    // Create signal handler.
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = &awakenTimeoutThread;
    sigaction(SIGALRM, &sa, NULL);

	reliablyTransfer(filename, numBytes, p);

    // Wait for the linked list to empty before sending finished packet.
    pthread_mutex_lock(&m);
    while (head || tail) {
        pthread_cond_wait(&cv, &m);
    }
    pthread_mutex_unlock(&m);

    PRINT(("Sending FINISHED\n"));

	// Done sending file data out, let the receiver know to stop.
	char val = -1;
    char buf[2];
    buf[0] = val;
    buf[1] = val;
    insert_data(buf, 0, 0, 2);
    if ((numbytes = sendto(sockfd, buf, 2, 0, p->ai_addr, p->ai_addrlen)) == -1) {
        perror("SEND :(\n");
        exit(1);
    }
	setitimer(ITIMER_REAL, &SRTT_sleep, NULL);

	void *result;
	pthread_join(ack_id, &result);
	pthread_cancel(timeout_id);

    freeaddrinfo(servinfo);

    return 0;
}
