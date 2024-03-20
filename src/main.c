#include <stdio.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <time.h>

#define BUFFER_SIZE 64
#define MAXIMUM_QUEUE 8
#define PORT 2507
#define BROADCAST_ADDRESS "255.255.255.255"
#define NUMBER_OF_NODES 10

typedef struct message {
	int sender_id, data, sequence_number;
} message;

typedef struct node_package node_package;

struct node_package {
	int sender_id;
	time_t timestamp;
	node_package* next;
	node_package* previous;
} ;

typedef struct linked_list {
	node_package* head;
	node_package* tail;
} linked_list;

void handle_error(char * error_message) {
	perror(error_message);
	exit(EXIT_FAILURE);
}

// Errors in this program are sparcely handled.
// I would have liked to have used different files but I'm not sure how to appropiately modify the Docker file


void traffic_analyzer(int* pipe) {
	
	_exit(EXIT_SUCCESS);
}

void traffic_generator(int* pipe) {
	int ret; 
	
	srand(time(NULL));

	ret = close(pipe[0]);
	if (ret < 0) handle_error("close");

	while (1) {
		usleep(1000000);
		int data = rand();
		write(pipe[0], &data, sizeof(data));
	}

	_exit(EXIT_SUCCESS);
}

void broadcaster (int node_id, int* generator_pipe, int* analyzer_pipe) {

	int ret; 

	// OPEN LISTENING SOCKET

	int listening_fd;
	struct sockaddr_in listening_address;
	
	listening_fd = socket(AF_INET, SOCK_DGRAM, 0);
	
	int reuseAddressPermission = 1;
	ret = setsockopt(listening_fd, SOL_SOCKET, SO_REUSEADDR, (void *) &reuseAddressPermission, sizeof(reuseAddressPermission));
    if (ret) handle_error("setsockopt");
	
	memset((char*) &listening_address, 0, sizeof(listening_address));
	listening_address.sin_family = AF_INET;
	listening_address.sin_addr.s_addr = htonl(INADDR_ANY);
	listening_address.sin_port = htons(PORT);

	bind(listening_fd, (struct sockaddr *) &listening_address, sizeof(listening_address));

	// OPEN WRITING SOCKET
	
	int writing_fd, n;
	struct sockaddr_in broadcast_addr;
		
	memset(&broadcast_addr, 0, sizeof(broadcast_addr));
	writing_fd = socket(AF_INET, SOCK_DGRAM, 0);

	int broadcastPermission = 1;
 	ret = setsockopt(writing_fd, SOL_SOCKET, SO_BROADCAST, (void *) &broadcastPermission, sizeof(broadcastPermission));
    if (ret) handle_error("setsockopt");

	memset((char*) &broadcast_addr, 0, sizeof(broadcast_addr));
	
	broadcast_addr.sin_family = AF_INET;
	broadcast_addr.sin_addr.s_addr = inet_addr(BROADCAST_ADDRESS);
	broadcast_addr.sin_port = htons(PORT);
		
	int data, internal_sequence_number= -1;
	message sending_message;
	
	// LISTENING LOOP
	int sender_id, sequence_number;
	message recieving_message;

	printf("Node initialized\n");
	fflush(stdout);

	while(1) {

		printf("Ndebuggg\n");
		fflush(stdout);

		// We check whether we have a new message ready (the read is non-blocking);
		ret = read(generator_pipe[0], &data, sizeof(data));
		if (ret > 0) {
			printf("RICEVUTO MESSAGGIO DA INVIARE: %d\n", data);
			fflush(stdout);

			internal_sequence_number = 0;

			sending_message.data = data;
			sending_message.sender_id = node_id;
			sending_message.sequence_number = 0;

			ret =sendto(writing_fd, &sending_message, sizeof(message), 0, (struct sockaddr*) &broadcast_addr, sizeof(broadcast_addr));
			if (ret < sizeof(message)) handle_error("sendto");
		}

		ret = read(listening_fd, &recieving_message, sizeof(message));
		if (ret < sizeof(message)) handle_error("read");
	
		sender_id = recieving_message.sender_id;
		sequence_number = recieving_message.sequence_number;
		data = recieving_message.data;
		
		printf("sender_id == %d\nnode_id == %d\n", sender_id, node_id);
		if (sender_id == node_id) continue;
		// If the package's sequence number is higher than the internal one, discard it.
		if (sequence_number>=internal_sequence_number && internal_sequence_number != -1) continue;
				
		internal_sequence_number = sequence_number;

		sending_message.data = data;
		sending_message.sender_id = node_id;
		sending_message.sequence_number = ++internal_sequence_number;

		printf("Recieved data package: %d\n", data);
		fflush(stdout);

		ret =sendto(writing_fd, &sending_message, sizeof(message), 0, (struct sockaddr*) &broadcast_addr, sizeof(broadcast_addr));
		if (ret < 0) handle_error("sendto");
	}
}

int main(int argc, char* argv[]) {

	setvbuf(stdout, NULL, _IONBF, 0);

	if (argc < 2) {
		printf("You need to send the id of this node\n");
		exit(EXIT_FAILURE);
	}

	int ret, generator_pipe[2], analyzer_pipe[2];
	fcntl(generator_pipe[1], F_SETFL, O_NONBLOCK); //Set read as non blocking

	ret = pipe(generator_pipe);
	if (ret < 0) handle_error("pipe");
	ret = close(generator_pipe[1]);
	if (ret < 0) handle_error("close");

	ret = pipe(analyzer_pipe);
	if (ret < 0) handle_error("pipe");
	ret = close(analyzer_pipe[0]);
	if (ret < 0) handle_error("close");

	ret = fork();
	if (ret < 0) handle_error("fork");
	if (ret > 0) traffic_analyzer(analyzer_pipe);
	
	ret = fork();
	if (ret < 0) handle_error("fork");
	if (ret > 0) traffic_generator(generator_pipe);

	broadcaster(atoi(argv[1]), generator_pipe, analyzer_pipe);
}