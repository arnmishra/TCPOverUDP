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

#define RWS 10
#define NUM_SEQ_NUM (2 * RWS)
#define MAX_PACKET_SIZE 1472
#define MAX_DATA_SIZE 1471 // 1472B payload - 1B for sequence number

char NFE = 0;       // Next frame Expected
char LFA = 0;       // Last frame acceptable

typedef struct data {
    int seq_num;
    int length;
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

        // Seq num: 5, NFE: 4, LFA: 7
        // ack_num: 3, NFE: 4, LFA: 7
        // Sending back: 35

void printPacketList() {
    data_t *node = head;
    while (node != NULL) {
        PRINT(("%d -- ", node->seq_num));
        node = node->next;
    }
    PRINT(("\n"));
}
void insert_data(char buf[MAX_DATA_SIZE], int sequence_num, ssize_t byte_count)
{
    data_t *node = malloc(sizeof(data_t));
    node->seq_num = sequence_num;
    node->data = malloc(byte_count);
    node->length = byte_count;
    memcpy(node->data, buf, byte_count);

    // PRINT(("==============\n"));
    // PRINT(("byte_count: %lu\n", byte_count));
    // PRINT(("strlen: %lu\n", strlen(node->data)));
    // PRINT(("==============\n"));

    // PRINT(("Inserting packet %d\n", sequence_num));

    if(head)
    {
        // PRINT(("head: %d\n", head->seq_num));
        // if (head->next)
        //     PRINT(("head->next = %d\n", head->next->seq_num));
        // if (head->prev)
        //     PRINT(("head->prev = %d\n", head->prev->seq_num));

        // printPacketList();
        int inserted = 0;
        data_t *temp = head;
        while(temp)
        {
            if (node->seq_num == temp->seq_num)
                return;

            // PRINT(("temp seq_num: %i\n", temp->seq_num));
            if((node->seq_num < temp->seq_num && (temp->seq_num - RWS) < node->seq_num) || (node->seq_num - RWS) > temp->seq_num) //insert node before temp
            {
                // PRINT(("Inserting new packet %d before existing packet %d\n", node->seq_num, temp->seq_num));
                // if (temp->next)
                //     PRINT(("Temp->next = %d\n", temp->next->seq_num));
                // if (temp->prev)
                //     PRINT(("Temp->prev = %d\n", temp->prev->seq_num));

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
                // if (node->next)
                //     PRINT(("Node->next = %d\n", node->next->seq_num));
                // if (node->prev)
                //     PRINT(("Node->prev = %d\n", node->prev->seq_num));
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


int write_data(char buf[MAX_DATA_SIZE], int sequence_num, ssize_t byte_count, FILE* output_file)
{
    // PRINT(("write_data:\n"));
    // printPacketList();
    // PRINT(("===========\n"));
    int final_seq_num = sequence_num;
    int num_nodes = 1;
    // PRINT(("===========\n"));
    int malloc_bytes = (int)(byte_count);
    // PRINT(("malloc_bytes: %i\n", malloc_bytes));
    // PRINT(("Byte Count:%i\n", (int)(byte_count)));
    if(head && head->seq_num == (sequence_num+1) % NUM_SEQ_NUM)
    {
        malloc_bytes += head->length;
        // PRINT(("malloc_bytes: %i\n", malloc_bytes));
        num_nodes++;
        data_t *temp = head->next;
        final_seq_num = head->seq_num;
        while(temp && temp->seq_num == (final_seq_num+1) % NUM_SEQ_NUM)
        {
            malloc_bytes += temp->length;
            // PRINT(("malloc_bytes: %i\n", malloc_bytes));
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

    // PRINT(("malloc_bytes: %i\n", malloc_bytes));
    // PRINT(("===========\n"));
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
    char seq_num;

    while(1)
    {
        ssize_t byte_count = recvfrom(sockfd, buf, sizeof(buf), 0, (struct sockaddr *)&addr, &addrlen);
        // PRINT(("Received message of length %zi: %s\n", byte_count, buf+1));
        if (buf[0] == 'F' && buf[1] == 'F') {
            char *buffer = malloc(4);
            sprintf(buffer, "FF");
            PRINT(("Sending back: %s\n", buffer));
            sendto(sockfd, buffer, strlen(buffer), 0, (struct sockaddr *)&addr, addrlen);
            break;
        }

        memcpy(&seq_num, &buf[0], 1);


        int acknowledge_num;
        PRINT(("Seq num: %d, NFE: %d, LFA: %d\n", seq_num, NFE, LFA));
        // If inside the window
        if((seq_num >= NFE && (seq_num <= LFA || seq_num > LFA + RWS)) || (seq_num < (NFE-RWS) && seq_num <= LFA))
        {
            if(seq_num == NFE) //if the next expected packet arrives
            {
                //NFE = (NFE + 1) % NUM_SEQ_NUM;
                //LFA = (LFA + 1) % NUM_SEQ_NUM;
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
                seq_num = NUM_SEQ_NUM - 1;
            else
                seq_num = NFE-1;
        }

        //PRINT(("ack_num: %d, NFE: %d, LFA: %d\n", acknowledge_num, NFE, LFA));

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
        // If the sequence number is between cum_ack and cum_ack + RWS
        int end_cum_ack = (cum_ack + RWS) % NUM_SEQ_NUM;
        // if(!((seq_num >= cum_ack && (seq_num <= end_cum_ack || seq_num > end_cum_ack + 4)) || (seq_num < (cum_ack-4) && seq_num < end_cum_ack)))
        if(!((seq_num >= NFE && (seq_num <= LFA || seq_num > LFA + RWS)) || (seq_num < (NFE-RWS) && seq_num <= LFA)))
        {
            seq_num = cum_ack;
        }
        // PRINT(("Seq num: %d, NFE: %d, LFA: %d\n", seq_num, NFE, LFA));
        sprintf(buffer, "%u.%u", cum_ack, seq_num);
        PRINT(("Sending back: %s\n", buffer));
        sendto(sockfd, buffer, strlen(buffer), 0, (struct sockaddr *)&addr, addrlen);
        free(buffer);
    }

}

int main(int argc, char** argv) {
    if (argc != 3)
    {
        PRINT(("usage: %s UDP_port filename_to_write\n\n", argv[0]));
        exit(1);
    }

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