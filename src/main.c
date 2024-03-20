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
#define SLIDING_WINDOW_SIZE 2
#define TIME_BETWEEN_GENERATED_PACKAGES 10000
#define TIME_BETWEEN_PRINTS 1

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

void handle_error(char * error_message) {
	perror(error_message);
	exit(EXIT_FAILURE);
}

// Errors in this program are sparcely handled.
// I would have liked to have used different files but I'm not sure how to appropiately modify the Dockerfile

void delete_list(node_package * node) {
	if (node != NULL) {
		delete_list(node->next);
		free(node);
	}
}

int count_sender_in_list(node_package * node, int sender_id) {
	if (node == NULL) return 0; 
	return ((node->sender_id == sender_id)?1:0) + count_sender_in_list(node->next, sender_id);
}

void traffic_analyzer(int* pipe) {
	int ret, sender_id;
	long int timer, time_of_last_print = time(NULL);
	message message;
	ret = close(pipe[1]);
	if (ret < 0) handle_error("close");
	
	node_package *head = NULL, *tail = NULL, *new_node, *old_tail;
	while (1) {
		// If enough time has passed, print the current status
		if (time(&timer) - time_of_last_print > TIME_BETWEEN_PRINTS) {
			for (int i = 0; i<NUMBER_OF_NODES; i++) {
				printf("%d: %03d ", i, count_sender_in_list(head, i));
			}
			printf("\n");
			fflush(stdout);
			
			time_of_last_print = timer;
		}
		ret = read(pipe[0], &sender_id, sizeof(sender_id));
		if (ret < 0) handle_error("read");
		if (ret == 0) {
			delete_list(head);
			_exit(EXIT_SUCCESS);
		}

		// Set up new node and add it as head
		new_node = malloc(sizeof(node_package));
		new_node->timestamp = time(&timer);
		new_node->sender_id = sender_id;
		new_node->next = head;
		new_node->previous = NULL;
		if (head == NULL) tail = new_node;
		else head->previous = new_node;
		head = new_node;

		// Delete old nodes from the list
		while (timer - tail->timestamp > SLIDING_WINDOW_SIZE) {
			old_tail = tail;
			tail->previous->next = NULL;
			tail = tail->previous;
			free(old_tail); 
		} 
			
	}
	
	// We should never get here
	handle_error("Unexpectedly got somewhere impossible");
}

void traffic_generator(int* pipe) {

	int ret; 
	srand(time(NULL));
	ret = close(pipe[0]);
	if (ret < 0) handle_error("close");

	while (1) {
		usleep(TIME_BETWEEN_GENERATED_PACKAGES);
		int data = rand();
		ret = write(pipe[1], &data, sizeof(data));
		if (ret < 0) handle_error("write");
		if (ret == 0) _exit(EXIT_SUCCESS); 
	}

	// We should never get here
	handle_error("Unexpectedly got somewhere impossible");
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

	fcntl(listening_fd, F_SETFL, O_NONBLOCK); //Set read as non blocking

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

		// We check whether we have a new message ready (the read is non-blocking);
		ret = read(generator_pipe[0], &data, sizeof(data));

		if (ret > 0) {

			// Tell the analyzer that we recieved some data
			ret = write(analyzer_pipe[1], &node_id, sizeof(node_id));
			if (ret <= 0) handle_error("write");
			
			sending_message.data = data;
			sending_message.sender_id = node_id;
			sending_message.sequence_number = 0;

			ret =sendto(writing_fd, &sending_message, sizeof(message), 0, (struct sockaddr*) &broadcast_addr, sizeof(broadcast_addr));
			if (ret < sizeof(message)) handle_error("sendto");
		}

		ret = read(listening_fd, &recieving_message, sizeof(message));
		if (ret < 0) continue;
		if (ret < sizeof(message)) handle_error("read");

		sender_id = recieving_message.sender_id;
		sequence_number = recieving_message.sequence_number;
		data = recieving_message.data;

		if (sender_id == node_id) continue;
		// If the package's sequence number is higher than the internal one, discard it.
		if (sequence_number>=internal_sequence_number && internal_sequence_number != -1) continue;

		// Tell the analyzer that we recieved some data
		ret = write(analyzer_pipe[1], &sender_id, sizeof(sender_id));
		if (ret <= 0) handle_error("write");

		internal_sequence_number = sequence_number;

		sending_message.data = data;
		sending_message.sender_id = node_id;
		sending_message.sequence_number = ++internal_sequence_number;

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

	ret = pipe(generator_pipe);
	if (ret < 0) handle_error("pipe");
	fcntl(generator_pipe[0], F_SETFL, O_NONBLOCK); //Set read as non blocking
	ret = pipe(analyzer_pipe);
	if (ret < 0) handle_error("pipe");

	ret = fork();
	if (ret < 0) handle_error("fork");
	if (ret > 0) traffic_analyzer(analyzer_pipe);
	ret = close(analyzer_pipe[0]);
	if (ret < 0) handle_error("close");

	ret = fork();
	if (ret < 0) handle_error("fork");
	if (ret > 0) traffic_generator(generator_pipe);
	ret = close(generator_pipe[1]);
	if (ret < 0) handle_error("close");

	broadcaster(atoi(argv[1]), generator_pipe, analyzer_pipe);
}