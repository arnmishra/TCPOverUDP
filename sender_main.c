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

#if 1
    #define PRINT(a) printf a
#else
    #define PRINT(a) (void)0
#endif

#define MAX_PACKET_SIZE 1472
#define MAX_DATA_SIZE 1471 // 1472B payload - 1B for sequence number

#define SWS 4
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

// NOT USED YET
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

void SIGINT_handler(int val) {
	PRINT(("Closing...\n"));

	close(sockfd);

	pthread_cond_destroy(&cv);
	pthread_mutex_destroy(&m);
    pthread_cond_destroy(&cv_timeout);
    pthread_mutex_destroy(&m_timeout);

    exit(1);
}

void printPacketList() {
    packet_t *node = head;
    while (node != NULL) {
        PRINT(("%d -- ", node->seq_num));
        node = node->next;
    }
    PRINT(("\n"));
}

void awakenTimeoutThread(int val) {
    pthread_mutex_lock(&m_timeout);
    timeout_check = true;
    pthread_cond_signal(&cv_timeout);
    pthread_mutex_unlock(&m_timeout);
}

void *checkForTimeouts(void *ptr) {
	thread_arg_t *arg = ptr;
	unsigned short int udpPort = arg->udpPort;
	struct addrinfo *p = arg->p;

    while (true) {
        // Sleep until SIGALRM sets timeout_check flag
        pthread_mutex_lock(&m_timeout);
        while (!timeout_check) {
            pthread_cond_wait(&cv_timeout, &m_timeout);
        }

        timeout_check = false;
        pthread_mutex_unlock(&m_timeout);

        PRINT(("Checking for timeouts...\n"));
        printPacketList();


        // Iterate through all packets checking for timeouts
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
            	// PRINT(("Checking packet %d\n", packet->seq_num));
                struct timeval diff;
                timersub(&now, &packet->send_time, &diff);
                if (timercmp(&diff, &SRTT.it_value, >=)) {
                    PRINT(("Packet %d timed out! Resending all n buffers...\n", packet->seq_num));

                    // Resend everything
                    while (packet != NULL) {
                    	PRINT(("Retransmitting packet (%d)\n", packet->seq_num));
                        char bruh;
                        memcpy(&bruh, &packet->data[0], 1);
                        // PRINT(("FIRST BYTE = %d\n", bruh));
    					if ((sendto(sockfd, packet->data, packet->num_bytes, 0, p->ai_addr, p->ai_addrlen)) == -1) {
    				        perror("sendto");
    				        exit(1);
    				    }
                        gettimeofday(&packet->send_time, NULL);
    				    setitimer(ITIMER_REAL, &SRTT_sleep, NULL);
    				    packet = packet->next;
                    }

                    // pthread_cond_signal(&cv);
                    break;
                }
                else
                    break;  // All packets after this one are newer anyway
                packet = packet->next;
            }
        }
        pthread_mutex_unlock(&m_packets);
    }

}

void insert_data(char *buf, int packet_id, int sequence_num, ssize_t byte_count)
{
    // PRINT(("INSERTING DATA... buflen = %zu\n", strlen(buf)));
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
            if(node->seq_num < temp->seq_num && (temp->seq_num - 4) < node->seq_num) //insert node before temp BUT gotta account for wrap of seq nums (i.e. 5 - 6 - 7 - 0)
            {
                node->next = temp;
                node->prev = temp->prev;
                if(temp->prev)
                {
                    temp->prev->next = node;
                }
                temp->prev = node;
                inserted = true;
                break;
            }
            temp = temp->next;
        }
        if(!inserted) //insert node at tail
        {
            tail->next = node;
            node->prev = tail;
            node->next = NULL;
            tail = node;
        }
    }
    else //if head isn't initialized, initialize it
    {
        head = node;
        head->next = NULL;
        head->prev = NULL;
        tail = head;
    }

    // PRINT(("After inserting packet %d\n===============\n", packet_id));
    // printPacketList();
    // PRINT(("==============\n"));

    pthread_mutex_unlock(&m_packets);
}

/**
 * Returns the current time in microseconds.
 */
long long unsigned int getMicrotime(struct timeval time){
    return (time.tv_sec * (long long unsigned int)1000000) + (time.tv_usec);
}

void estimateNewRTT(packet_t *packet) {
    struct timeval now;
    gettimeofday(&now, NULL);

    timersub(&now, &packet->send_time, &RTT);
    RTT.tv_usec *= 2;
    // PRINT(("RTT = %ld s and %06ld microseconds\n", RTT.tv_sec, RTT.tv_usec));

    long long unsigned int SRTT_ms = getMicrotime(SRTT.it_value);
    long long unsigned int RTT_ms = getMicrotime(RTT);

    double new_SRTT_ms = (alpha * (double)SRTT_ms) + ((1 - alpha) * (double)RTT_ms);

    SRTT.it_value.tv_sec = (new_SRTT_ms / (int)1e6);
    SRTT.it_value.tv_usec = ((int)new_SRTT_ms % (int)1e6);

    SRTT_sleep.it_value.tv_sec = SRTT.it_value.tv_sec;
    SRTT_sleep.it_value.tv_usec = (SRTT.it_value.tv_usec) * 2;

    // PRINT(("SRTT = %ld s and %06ld microseconds\n", SRTT.it_value.tv_sec, SRTT.it_value.tv_usec));
    // PRINT(("SRTT_sleep = %ld s and %06ld microseconds\n", SRTT_sleep.it_value.tv_sec, SRTT_sleep.it_value.tv_usec));

    if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, (char *)&SRTT_sleep.it_value, sizeof(SRTT_sleep.it_value)) < 0)
        error("setsockopt failed\n");
}

/*
 * Given an ack number, mark the packet as inactive in the window list
 */
void markPacketAsInactive(int cum_ack) {
	packet_t *next;

    // printPacketList();

	// PRINT(("Removing packet %d!\n", cum_ack));

    // Delete everything in the linked list up until (and including) cum_ack
    // Essentially, delete head until seq_num >= cum_ack
    /////////////////

    // (head->seq_num <= cum_ack && (head->seq_num < (cum_ack - SWS)) || )



    // head + SWS > cum_ack

    while (head && ((head->seq_num <= cum_ack && (head->seq_num + SWS) > cum_ack) || (head->seq_num - SWS) > cum_ack))
	{
        if (head->seq_num == cum_ack)
            estimateNewRTT(head);

		PRINT(("Removing packet %d\n", head->seq_num));
		next = head->next;
        free(head->data);
        free(head);
        head = next;
        // PRINT(("markPacketAsInactive:\n"));
        // printPacketList();
        // PRINT(("====================\n"));
        if(head)
        {
        	head->prev = NULL;
        }
		// PRINT(("enter\n"));
		LAR = (LAR + 1) % NUM_SEQ_NUM;
	}

 //    if(head)
 //    {
 //        PRINT(("Second: Removing packet %d\n", head->seq_num));
	// 	next = head->next;

 //        estimateNewRTT(head);

	// 	free(head->data);
	// 	free(head);
	// 	head = next;
 //        // PRINT(("markPacketAsInactive:\n"));
 //        // printPacketList();
 //        // PRINT(("====================\n"));
	// 	if(head)
 //        {
 //        	head->prev = NULL;
 //        }
	// 	//LAR = (LAR + 1) % NUM_SEQ_NUM;
	// }

	LAR = cum_ack;


    // PRINT(("After removing...\n=========\n"));
    // printPacketList();
    // PRINT(("=========\n"));
}

/*
 * Thread for receiving acknowledgements, wakes up the sleeping transfer thread
 */
void *receiveAcknowledgements(void *ptr) {
	thread_arg_t *arg = ptr;
	unsigned short int udpPort = arg->udpPort;
	struct addrinfo *p = arg->p;

	while (1) {
		char buf[2];
	    ssize_t byte_count = recvfrom(sockfd, buf, sizeof(buf), 0, p->ai_addr, &p->ai_addrlen);

        if (byte_count > 0) {
    	    char cum_ack;
    	    memcpy(&cum_ack, &buf[0], 1);

    	    if (cum_ack == 'F') {
    	    	PRINT(("RECEIVED ACKNOWLEDGEMENT...\n"));
    	    	PRINT(("ENDING PROGRAM...\n"));
                // free(head->data);
                // free(head);
    	    	break;
    	    }

    	    char last_seq_recv;
    	    memcpy(&last_seq_recv, &buf[1], 1);

    	    cum_ack -= 48;
            last_seq_recv -= 48;
    	    //PRINT(("next: %d, LAR: %d\n", cum_ack, LAR));
            pthread_mutex_lock(&m_packets);

            // If the cumulative ack is in the window
            // DON'T MOD THE LAR + 4 BECAUSE LAR = 6 AND CUM_ACK = 7 BREAKS IT
    	    if ((cum_ack >= (LAR + 1) && (LAR + 4 >= cum_ack)) || cum_ack <= (LAR - 4)) {
    	    	PRINT(("1 Received ack %d.%d\n", cum_ack, last_seq_recv));
    	    	markPacketAsInactive(cum_ack);
                send_check = true;
                pthread_cond_broadcast(&cv);
                setitimer(ITIMER_REAL, &SRTT_sleep, NULL);
    	    }
            // If the select acknowledgement is later than the cumulative ack
    	    else if (last_seq_recv > cum_ack || last_seq_recv <= (cum_ack + 4) % NUM_SEQ_NUM)
    	    {
    	    	PRINT(("2 Received ack %d.%d\n", cum_ack, last_seq_recv));
    	    	packet_t *temp = head;
    	    	while (temp && temp->seq_num != last_seq_recv)
    			{
    				temp = temp->next;
    			}
    			if(temp)
    			{
    				if(temp->prev)
    				{
    					temp->prev->next = temp->next;
    				}
    				if(temp->next)
    				{
    					temp->next->prev = temp->prev;
    				}
    				estimateNewRTT(temp);
                    PRINT(("receiveAcknowledgements: Removing packet %d\n", temp->seq_num));
                    printPacketList();

                    if (temp == head)
                        head = NULL;

    				free(temp->data);
    				free(temp);
    			}
                pthread_cond_broadcast(&cv);
                setitimer(ITIMER_REAL, &SRTT_sleep, NULL);
    	    }
            else {
                PRINT(("Received random ack (%s)\n", buf));
            }

            pthread_mutex_unlock(&m_packets);
        }
        else {
            awakenTimeoutThread(0);
        }

	}

    PRINT(("ENDING\n"));

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
		PRINT(("ERR: File %s does not exist\n", filename));
		exit(1);
	}

	int id = 0;
	pthread_mutex_lock(&m);
    bool bruh = true;

    char seq_num = 0;

	while (bytesToTransfer > 0) {

		// PRINT(("Any packets to send?\n"));
		// PRINT(("seq_num = %d, LAR = %d, SWS = %d\n", seq_num, LAR, SWS));
		// printPacketList();

		while (( (seq_num > LAR && seq_num <= (LAR + SWS))  || (seq_num < LAR && LAR + SWS >= NUM_SEQ_NUM && seq_num <= (LAR + SWS) % NUM_SEQ_NUM)) && bytesToTransfer > 0) {
			char buf[MAX_PACKET_SIZE];
			char *ptr = buf + 1;
			size_t bytesRead;
			if (bytesToTransfer > MAX_DATA_SIZE) {
				bytesRead = fread(ptr, 1, MAX_DATA_SIZE, f);
				//PRINT(("1: %i\n", fileNotFinished));
				//bytesRead = MAX_DATA_SIZE;
				bytesToTransfer -= MAX_DATA_SIZE;
			}
			else {
				bytesRead = fread(ptr, 1, bytesToTransfer, f);
				//PRINT(("2: %i\n", fileNotFinished));
				//bytesRead = bytesToTransfer;
				bytesToTransfer = 0;
			}

			memcpy(buf, &seq_num, 1);

			// Update the window linked list
			insert_data(buf, id, seq_num, bytesRead+1);

			fflush(stdout);
			PRINT(("Sending out packet (%d)\n", seq_num));

			int numBytes;
			if ((numBytes = sendto(sockfd, buf, bytesRead+1, 0, p->ai_addr, p->ai_addrlen)) == -1) {
		        perror("sendto");
		        exit(1);
		    }

		    // if(seq_num == 0)
		    // {
		    // 	seq_num = 1;
		    // }
		    // else if(seq_num == 2)
		    // {
		    // 	seq_num = 0;
		    // }
		    // else if(seq_num == 1)
		    // {
		    // 	seq_num = 2;
		    // }
		    setitimer(ITIMER_REAL, &SRTT_sleep, NULL);

			seq_num = (seq_num + 1) % NUM_SEQ_NUM;

			id += 1;
			//PRINT(("bytes: %i\n", bytesToTransfer));
		}
		//PRINT(("seq_num = %d, LAR = %d, SWS = %d\n", seq_num, LAR, SWS));
        if (bytesToTransfer == 0)
            break;

		// Conditional wait here until an ACK is received, or something timesOut
		// PRINT(("rT: Sleeping with %lld to send...\n", bytesToTransfer));
        while (!send_check) {
            pthread_cond_wait(&cv, &m);
        }
        send_check = false;
		// PRINT(("rT: Woken up!...\n"));
	}
	pthread_mutex_unlock(&m);

	fclose(f);
}

int main(int argc, char** argv)
{
    if(argc != 5) {
		PRINT(("usage: %s receiver_hostname receiver_port filename_to_xfer bytes_to_xfer\n\n", argv[0]));
		exit(1);
	}
	signal(SIGINT, SIGINT_handler);
	// signal(SIGALRM, awakenTimeoutThread);

	char *hostname = argv[1];
	char *portNum  = argv[2];
	char *filename = argv[3];
	// PRINT(("hostname = %s; portnum = %s\n", hostname, portNum));

	// lol fuck that idk
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

	// loop through all the results and make a socket
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

	// // Create thread for waiting and timeouts
	pthread_t timeout_id;
    if (pthread_create(&timeout_id, NULL, checkForTimeouts, &arg) != 0) {
        perror("pthread_create");
        exit(1);
    }

    // Initialize SRTT(0) to 1 second
    SRTT.it_value.tv_sec = 1;
    SRTT.it_value.tv_usec = 0;
    SRTT.it_interval.tv_sec = 0;
    SRTT.it_interval.tv_usec = 0;
    // Initialize RTT(0) = 0.5 second
    RTT = (struct timeval){0};
    RTT.tv_usec = 50000;
    // Create signal handler
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = &awakenTimeoutThread;
    sigaction(SIGALRM, &sa, NULL);

	reliablyTransfer(hostname, udpPort, filename, numBytes, p);

    // printPacketList();

    // Wait for the linked list to empty before sending finished packet
    pthread_mutex_lock(&m);
    while (head) {
        pthread_cond_wait(&cv, &m);
        // PRINT(("I'm DONE WAITING\n"));
    }
    pthread_mutex_unlock(&m);

	// Done sending shit out, let the receiver know to stop
	char *buf = "F";
	insert_data(buf, 0, 0, 3);
	if ((numbytes = sendto(sockfd, buf, strlen(buf), 0, p->ai_addr, p->ai_addrlen)) == -1) {
		perror("SEND :(\n");
		exit(1);
	}
	setitimer(ITIMER_REAL, &SRTT_sleep, NULL);

	void *result;
	pthread_join(ack_id, &result);
	pthread_cancel(timeout_id);

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