all: reliable_sender reliable_receiver

reliable_sender:
	gcc -o reliable_sender sender_main.c common.c

reliable_receiver:
	gcc -o reliable_receiver receiver_main.c common.c

clean:
	rm reliable_sender reliable_receiver
