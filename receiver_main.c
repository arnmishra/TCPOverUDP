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
#define MAX_PACKET_SIZE 1472
#define MAX_DATA_SIZE 1471 // 1472B payload - 1B for sequence number

unsigned char seq_num;      // Sequence number
unsigned char NFE = 0;       // Next frame Expected
unsigned char LFA = 0;       // Last frame acceptable

typedef struct data {
    int seq_num;
    char *data;
    struct data *next;
    struct data *prev;
} data_t;
data_t *head;
data_t *tail;


// void signalHandler(int val) {
//  printf("Closing...\n");
//  close(udpPort);
//     exit(1);
// }


/**
receive message
get the sequence number
if seq_num==expected: increment expected and write to file
else: throw away
send ack + seq_num
**/

void insert_data(char buf[MAX_DATA_SIZE], int seq_num, ssize_t byte_count)
{
    data_t *node = malloc(sizeof(data_t));
    node->seq_num = seq_num;
    node->data = malloc(byte_count);
    memcpy(node->data, buf, byte_count);
    if(head)
    {
        int inserted = 0;
        data_t *temp = head;
        while(temp)
        {
            if(node->seq_num < temp->seq_num) //insert node before temp
            {
                node->next = temp;
                node->prev = temp->prev;
                if(temp->prev)
                {
                    temp->prev->next = node;
                }
                temp->prev = node;
                inserted = 1;
            }
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
}

void write_data(char buf[MAX_DATA_SIZE], int seq_num, ssize_t byte_count, FILE* output_file)
{
    int num_nodes = 1;
    int malloc_bytes = (int)(byte_count);
    printf("Byte Count:%i\n", (int)(byte_count));
    if(head && head->seq_num == seq_num+1)
    {
        malloc_bytes += sizeof(head->data);
        num_nodes++;
        data_t *temp = head;
        while(temp->next && temp->next->seq_num == temp->seq_num+1)
        {
            malloc_bytes += sizeof(temp->next->data);
            num_nodes++;
            temp = temp->next;
        }
        
    }
    char * batch_write = malloc(malloc_bytes);
    memcpy(batch_write, buf, byte_count);
    int i = 1;
    int num_bytes = (int)(byte_count);
    if(num_nodes > 1)
    {
        while(i<num_nodes)
        {
            memcpy(batch_write+num_bytes, head->data, sizeof(head->data));
            num_bytes += sizeof(head->data);
            i++;
            head = head->next;
            free(head->prev->data);
            free(head->prev);
            head->prev = NULL;
        }
    }
    //printf("Writing: %s\n", batch_write);
    fwrite(batch_write, 1, num_bytes, output_file); // Skip the sequence number and write the rest
    fflush(output_file);
}

void reliablyReceive(char * myUDPport, char* destinationFile) {
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
    char buf[MAX_PACKET_SIZE];

    FILE * output_file = fopen(destinationFile, "w+");

    while(1)
    {
        ssize_t byte_count = recvfrom(sockfd, buf, sizeof(buf), 0, (struct sockaddr *)&addr, &addrlen);
        printf("Received message of length %zi: %s\n", byte_count, buf+1);
        memcpy(&seq_num, &buf[0], 1);
        printf("Sequence number: %u\n", seq_num);

        int acknowledge_num;
        //printf("Seq num: %d, NFE: %d, LFA: %d\n", seq_num, NFE, LFA);
        if(seq_num >= NFE && seq_num <= LFA)
        {
            if(seq_num == NFE)
            {
                NFE++;
                LFA++;
                acknowledge_num = seq_num;
                write_data(buf+1, seq_num, byte_count-1, output_file);
            }
            else
            {
                acknowledge_num = NFE;
                insert_data(buf+1, seq_num, byte_count-1);
            }
        }
        char *buffer = malloc(4);
        sprintf(buffer, "ack%u", acknowledge_num);
        sendto(sockfd, buffer, strlen(buffer), 0, (struct sockaddr *)&addr, addrlen);
    }
    
}

int main(int argc, char** argv) {
    if (argc != 3)
    {
        fprintf(stderr, "usage: %s UDP_port filename_to_write\n\n", argv[0]);
        exit(1);
    }

    seq_num = malloc(1);
    head = NULL;
    LFA = NFE + RWS - 1;

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