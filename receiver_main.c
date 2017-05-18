/**
FILENAME: receiver_main.c

DESCRIPTION: Receiver for TCP Connection built over UDP protocol.

AUTHORS:
    Arnav Mishra
    Samir Chaudhry
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <signal.h>
#include <netdb.h>
#include <stdbool.h>


#define NUM_SEQ_NUM (2 * RWS)
#define MAX_PACKET_SIZE 1472
#define MAX_DATA_SIZE 1471 // 1472B payload - 1B for sequence number

int RWS = 60;       // Receive Window Size
char NFE = 0;       // Next frame Expected
char LFA = 0;       // Last frame acceptable

// Store the packets that have been arrived in order to write to file
typedef struct data {
    int seq_num;
    int length;
    char *data;
    struct data *next;
    struct data *prev;
} data_t;
data_t *head;
data_t *tail;

/**
Function: insert_data

Purpose: Insert new data that is received into the linked list

Parameters:
    buf -- The buffer that has been received from the sender.
    sequence_num -- The Sequence Number of this buffer (used to insert in order).
    byte_count -- The number of bytes in this buffer.
*/
void insert_data(char buf[MAX_DATA_SIZE], int sequence_num, ssize_t byte_count)
{
    data_t *node = malloc(sizeof(data_t));
    node->seq_num = sequence_num;
    node->data = malloc(byte_count);
    node->length = byte_count;
    memcpy(node->data, buf, byte_count);

    if(head)
    {
        int inserted = 0;
        data_t *temp = head;
        while(temp)
        {
            // Don't insert duplicate entries.
            if (node->seq_num == temp->seq_num)
                return;
            // Insert Node Before Temp.
            if((node->seq_num < temp->seq_num && (temp->seq_num - RWS) < node->seq_num) || (node->seq_num - RWS) > temp->seq_num)
            {
                node->next = temp;
                node->prev = temp->prev;
                if(temp->prev)
                {
                    temp->prev->next = node;
                }
                temp->prev = node;
                inserted = 1;
                if(temp == head)
                    head = node;
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
}

/**
Function: write_data

Purpose: Write consecutive data to the output file.

Parameters:
    buf -- The buffer that has been received from the sender.
    sequence_num -- The Sequence Number of this buffer (used to insert in order).
    byte_count -- The number of bytes in this buffer.
    output_file -- The file to which the content should be written.

Returns: Last ack that was written to the file. 
*/
int write_data(char buf[MAX_DATA_SIZE], int sequence_num, ssize_t byte_count, FILE* output_file)
{
    int final_seq_num = sequence_num;
    int num_nodes = 1;
    int malloc_bytes = (int)(byte_count);

    if(head && head->seq_num == (sequence_num+1) % NUM_SEQ_NUM)
    {
        malloc_bytes += head->length;
        num_nodes++;
        data_t *temp = head->next;
        final_seq_num = head->seq_num;
        while(temp && temp->seq_num == (final_seq_num+1) % NUM_SEQ_NUM)
        {
            malloc_bytes += temp->length;
            num_nodes++;
            final_seq_num = temp->seq_num;
            temp = temp->next;
        }
    }
    char * batch_write = malloc(malloc_bytes);
    memcpy(batch_write, buf, byte_count);
    int i = 1;
    int num_bytes = (int)(byte_count);

    if(num_nodes > 1)
    {
        while(i < num_nodes)
        {
            memcpy(batch_write+num_bytes, head->data, head->length);
            num_bytes += head->length;
            i++;
            data_t *next = head->next;
            if (next)
                next->prev = NULL;
            free(head->data);
            free(head);
            head = next;
        }
    }
    fwrite(batch_write, 1, num_bytes, output_file);
    fflush(output_file);
    return final_seq_num;
}

/**
Function: reliably_receive

Purpose: Receive data from the sender. If the next expected data is received, write it directly
    to the file. If data is received out of order, insert it into the linked list. Send back an
    acknowledgement of the last consecutive packet that has been received and the packet number
    that was just received.

Parameters:
    myUDPport -- The port on which to receive data from the sender.
    destinationFile -- The file to which the data will be written when received.
*/
void reliably_receive(char * myUDPport, char* destinationFile) {
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
    socklen_t addrlen = sizeof(addr);
    char buf[MAX_PACKET_SIZE];

    FILE * output_file = fopen(destinationFile, "w+");
    char seq_num;

    while(1)
    {
        ssize_t byte_count = recvfrom(sockfd, buf, sizeof(buf), 0, (struct sockaddr *)&addr, &addrlen);
        if (buf[0] == -1 && buf[1] == -1)
            break;

        memcpy(&seq_num, &buf[0], 1);

        int acknowledge_num;
        // For data that is received inside the expected window.
        if((seq_num >= NFE && (seq_num <= LFA || seq_num > LFA + RWS)) || (seq_num < (NFE-RWS) && seq_num <= LFA))
        {
            // If the next expected packet arrives, write the data.
            if(seq_num == NFE)
            {
                acknowledge_num = write_data(buf+1, seq_num, byte_count-1, output_file);
                NFE = (acknowledge_num + 1) % NUM_SEQ_NUM;
                LFA = (NFE + RWS - 1) % NUM_SEQ_NUM;
            }
            //If a later packet arrives, insert the data into the linked list. 
            else
            {
                acknowledge_num = NFE - 1;
                insert_data(buf+1, seq_num, byte_count-1);
            }
        }
        // If a packet outside the expected window arrives, ignore the data. 
        else
        {
            acknowledge_num = NFE;
            if (NFE == 0)
                seq_num = NUM_SEQ_NUM - 1;
            else
                seq_num = NFE-1;
        }

        char *buffer = malloc(10);
        char cum_ack;
        if(NFE == 0)
        {
            cum_ack = NUM_SEQ_NUM - 1;
        }
        else
        {
            cum_ack = NFE - 1;
        }
        // If the sequence number is between cum_ack and cum_ack + RWS.
        if(!((seq_num >= NFE && (seq_num <= LFA || seq_num > LFA + RWS)) || (seq_num < (NFE-RWS) && seq_num <= LFA)))
        {
            seq_num = cum_ack;
        }
        sprintf(buffer, "%u.%u", cum_ack, seq_num);
        printf("Sending back: %s\n", buffer);
        sendto(sockfd, buffer, strlen(buffer), 0, (struct sockaddr *)&addr, addrlen);
        free(buffer);
    }

    // Send back acknowledgment.
    char *buffer = malloc(4);
    sprintf(buffer, "FF");
    printf("Sending back: %s\n", buffer);
    sendto(sockfd, buffer, strlen(buffer), 0, (struct sockaddr *)&addr, addrlen);
}

/**
Function: main

Purpose: Main function to verify arguments are received and start the receiver.

Parameters:
    argc -- Number of arguments passed into the program.
    argv -- List of arguments that are passed in.

Returns: 0 for successful completion.
*/
int main(int argc, char** argv) {
    if (argc != 3)
    {
        printf("usage: %s UDP_port filename_to_write\n\n", argv[0]);
        exit(1);
    }

    head = NULL;
    LFA = (NFE + RWS - 1) % NUM_SEQ_NUM;
    reliably_receive(argv[1], argv[2]);

    return 0;
}