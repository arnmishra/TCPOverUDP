# TCPOverUDP

TCPOverUDP is an implementation of TCP Protocols over a UDP base. The receiver and sender are separated into two files. The protocol is built for an MTU of 1500 bytes. This allocates for a 20 byte IPV4 Header, 8 byte UDP Header, and 1472 byte payload. 

### Usage:
>$ make

For the sender, you must provide the hostname of the receiver, the name of the file to transmit, and how many bytes to send. 

>$ ./sender_main receiver_hostname receiver_port filename_to_transfer bytes_to_transfer

For the receiver, you must provide the port at which to listen and the name of the output file to which all data to will be written.

>$ ./receiver_main port filename_to_write