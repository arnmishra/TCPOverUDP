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

#if 1
    #define PRINT(a) printf a
#else
    #define PRINT(a) (void)0
#endif

#define RWS 4
#define NUM_SEQ_NUM (2 * RWS)
#define MAX_PACKET_SIZE 1472
#define MAX_DATA_SIZE 1471 // 1472B payload - 1B for sequence number

char seq_num;      // Sequence number
char NFE = 0;       // Next frame Expected
char LFA = 0;       // Last frame acceptable

typedef struct data {
    int seq_num;
    char *data;
    struct data *next;
    struct data *prev;
} data_t;
data_t *head;
data_t *tail;


// void signalHandler(int val) {
//  PRINT(("Closing...\n"));
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
            if(node->seq_num < temp->seq_num && (temp->seq_num - 4) < node->seq_num) //insert node before temp
            {
                node->next = temp;
                node->prev = temp->prev;
                if(temp->prev)
                {
                    temp->prev->next = node;
                }
                temp->prev = node;
                inserted = 1;
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
}

void printPacketList() {
    data_t *node = head;
    while (node != NULL) {
        PRINT(("%d -- ", node->seq_num));
        node = node->next;
    }
    PRINT(("\n"));
}

int write_data(char buf[MAX_DATA_SIZE], int seq_num, ssize_t byte_count, FILE* output_file)
{
    printPacketList();
    int final_seq_num = seq_num;
    int num_nodes = 1;
    int malloc_bytes = (int)(byte_count);
    // PRINT(("Byte Count:%i\n", (int)(byte_count)));
    if(head && head->seq_num == (seq_num+1) % NUM_SEQ_NUM)
    {
        malloc_bytes += strlen(head->data);
        num_nodes++;
        data_t *temp = head->next;
        final_seq_num = head->seq_num;
        while(temp && temp->seq_num == (final_seq_num+1) % NUM_SEQ_NUM)
        {
            malloc_bytes += strlen(temp->data);
            num_nodes++;
            final_seq_num = temp->seq_num;
            temp = temp->next;
            // PRINT(("final_seq_num = %d\n", final_seq_num));
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
            memcpy(batch_write+num_bytes, head->data, strlen(head->data));
            num_bytes += strlen(head->data);
            i++;
            data_t *next = head->next;
            free(head->data);
            free(head);
            head = next;
        }
    }
    // PRINT(("Writing: %i\n", num_bytes));
    fwrite(batch_write, 1, num_bytes, output_file); // Skip the sequence number and write the rest
    fflush(output_file);
    return final_seq_num;
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
        // PRINT(("Received message of length %zi: %s\n", byte_count, buf+1));

        if (buf[0] == 'F') {
            char *buffer = malloc(4);
            sprintf(buffer, "F");
            PRINT(("Sending back: %s\n", buffer));
            sendto(sockfd, buffer, strlen(buffer), 0, (struct sockaddr *)&addr, addrlen);
            break;
        }

        memcpy(&seq_num, &buf[0], 1);
        // PRINT(("Sequence number: %u\n", seq_num));

        int acknowledge_num;
        PRINT(("Seq num: %d, NFE: %d, LFA: %d\n", seq_num, NFE, LFA));
        // If inside the window
        if((seq_num >= NFE && (seq_num <= LFA || seq_num > LFA + 4)) || (seq_num < (NFE-4) && seq_num <= LFA))
        {
            if(seq_num == NFE) //if the next expected packet arrives
            {
                //NFE = (NFE + 1) % NUM_SEQ_NUM;
                //LFA = (LFA + 1) % NUM_SEQ_NUM;
                // PRINT(("writing\n"));
                acknowledge_num = write_data(buf+1, seq_num, byte_count-1, output_file);
                NFE = (acknowledge_num + 1) % NUM_SEQ_NUM;
                LFA = (NFE + RWS - 1) % NUM_SEQ_NUM;
            }
            else //if a packet later than the next expected arrives
            {
                acknowledge_num = NFE - 1;
                insert_data(buf+1, seq_num, byte_count-1);
            }
        }
        else //if a packet that is not in the expected window arrives
        {
            acknowledge_num = NFE;
            if (NFE == 0)
                seq_num = 7;
            else
                seq_num = NFE-1;
        }

        PRINT(("ack_num: %d, NFE: %d, LFA: %d\n", acknowledge_num, NFE, LFA));

        char *buffer = malloc(2);
        char cum_ack;
        if(NFE == 0)
        {
            cum_ack = 7;
        }
        else
        {
            cum_ack = NFE - 1;
        }
        // If the sequence number is between cum_ack and cum_ack + RWS
        int end_cum_ack = (cum_ack + RWS) % NUM_SEQ_NUM;
        // if(!((seq_num >= cum_ack && (seq_num <= end_cum_ack || seq_num > end_cum_ack + 4)) || (seq_num < (cum_ack-4) && seq_num < end_cum_ack)))
        if(!((seq_num >= NFE && (seq_num <= LFA || seq_num > LFA + 4)) || (seq_num < (NFE-4) && seq_num <= LFA)))
        {
            seq_num = cum_ack;
        }
        // PRINT(("Seq num: %d, NFE: %d, LFA: %d\n", seq_num, NFE, LFA));
        sprintf(buffer, "%u%u", cum_ack, seq_num);
        PRINT(("Sending back: %s\n", buffer));
        sendto(sockfd, buffer, strlen(buffer), 0, (struct sockaddr *)&addr, addrlen);
    }

}

int main(int argc, char** argv) {
    if (argc != 3)
    {
        PRINT(("usage: %s UDP_port filename_to_write\n\n", argv[0]));
        exit(1);
    }

    seq_num = malloc(1);
    head = NULL;
    LFA = (NFE + RWS - 1) % NUM_SEQ_NUM;

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