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
#include <errno.h>
#include <ifaddrs.h>
#include <sys/select.h>

#define PORT 2507
#define BROADCAST_ADDRESS "255.255.255.255"
#define NUMBER_OF_NODES 10
#define SLIDING_WINDOW_SIZE_S 2
#define TIME_BETWEEN_GENERATED_PACKAGES_MS 10
#define TIME_BETWEEN_PRINTS_S 1
#define SELECT_RATE_US 5

// Errors in this program are sparcely handled.

// I would have liked to have used different files but I'm not sure how to appropiately modify the Dockerfile

typedef struct message
{
	int sender_id, data, sequence_number;
} message;

/* We handle the list of recently recieved nodes as a doubly-linked-list, to easily add from head and
remove from tail. */
typedef struct node_recieved node_recieved;
struct node_recieved
{
	int sender_id;
	time_t timestamp;
	node_recieved *next;
	node_recieved *previous;
};

void handle_error(char *error_message)
{
	perror(error_message);
	exit(EXIT_FAILURE);
}

int msleep(long msec)
{
	struct timespec ts;
	int res;

	ts.tv_sec = msec / 1000;
	ts.tv_nsec = (msec % 1000) * 1000000;

	do
	{
		res = nanosleep(&ts, &ts);
	} while (res && errno == EINTR);

	return res;
}

void delete_list(node_recieved *node)
{
	if (node != NULL)
	{
		delete_list(node->next);
		free(node);
	}
}

int count_sender_in_list(node_recieved *node, int sender_id)
{
	if (node == NULL)
		return 0;
	return ((node->sender_id == sender_id) ? 1 : 0) + count_sender_in_list(node->next, sender_id);
}

void traffic_analyzer(int *pipe)
{
	int ret, sender_id;
	long int timer, time_of_last_print = time(NULL);
	message message;
	ret = close(pipe[1]);
	if (ret < 0)
		handle_error("close");

	node_recieved *head = NULL, *tail = NULL, *new_node, *old_tail;
	while (1)
	{
		// If enough time has passed, print the current status
		if (time(&timer) - time_of_last_print > TIME_BETWEEN_PRINTS_S)
		{
			for (int i = 0; i < NUMBER_OF_NODES; i++)
			{
				printf("%d: %03d ", i, count_sender_in_list(head, i));
			}
			printf("\n");
			fflush(stdout);

			time_of_last_print = timer;
		}

		// Check whether a new package has been generated
		ret = read(pipe[0], &sender_id, sizeof(sender_id));
		if (ret < 0)
			handle_error("read");
		if (ret == 0)
		{
			delete_list(head);
			_exit(EXIT_SUCCESS);
		}

		// Set up new node and add it as head
		new_node = malloc(sizeof(node_recieved));
		new_node->timestamp = time(&timer);
		new_node->sender_id = sender_id;
		new_node->next = head;
		new_node->previous = NULL;
		if (head == NULL)
			tail = new_node;
		else
			head->previous = new_node;
		head = new_node;

		// Delete old nodes from the list
		while (timer - tail->timestamp > SLIDING_WINDOW_SIZE_S)
		{
			old_tail = tail;
			tail->previous->next = NULL;
			tail = tail->previous;
			free(old_tail);
		}
	}

	// We should never get here
	handle_error("Unexpectedly got somewhere impossible");
}

void traffic_generator(int *pipe)
{
	int ret;
	srand(time(NULL));
	ret = close(pipe[0]);
	if (ret < 0)
		handle_error("close");

	while (1)
	{
		msleep(TIME_BETWEEN_GENERATED_PACKAGES_MS);
		int data = rand();
		ret = write(pipe[1], &data, sizeof(data));
		if (ret < 0)
			handle_error("write");
		if (ret == 0)
			_exit(EXIT_SUCCESS);
	}

	// We should never get here
	handle_error("Unexpectedly got somewhere impossible");
}

void broadcaster(int node_id, int *generator_pipe, int *analyzer_pipe)
{

	int ret;

	// OPEN AND SETUP LISTENING SOCKET

	int listening_fd;
	struct sockaddr_in listening_address;

	listening_fd = socket(AF_INET, SOCK_DGRAM, 0);

	int reuseAddressPermission = 1;
	ret = setsockopt(listening_fd, SOL_SOCKET, SO_REUSEADDR, (void *)&reuseAddressPermission, sizeof(reuseAddressPermission));
	if (ret)
		handle_error("setsockopt");

	memset((char *)&listening_address, 0, sizeof(listening_address));
	listening_address.sin_family = AF_INET;
	listening_address.sin_addr.s_addr = htonl(INADDR_ANY);
	listening_address.sin_port = htons(PORT);

	bind(listening_fd, (struct sockaddr *)&listening_address, sizeof(listening_address));

	// OPEN AND SETUP WRITING SOCKET

	int writing_fd, n;
	struct sockaddr_in broadcast_addr;

	memset(&broadcast_addr, 0, sizeof(broadcast_addr));
	writing_fd = socket(AF_INET, SOCK_DGRAM, 0);

	int broadcastPermission = 1;
	ret = setsockopt(writing_fd, SOL_SOCKET, SO_BROADCAST, (void *)&broadcastPermission, sizeof(broadcastPermission));
	if (ret)
		handle_error("setsockopt");

	memset((char *)&broadcast_addr, 0, sizeof(broadcast_addr));

	broadcast_addr.sin_family = AF_INET;
	broadcast_addr.sin_addr.s_addr = inet_addr(BROADCAST_ADDRESS);
	broadcast_addr.sin_port = htons(PORT);

	struct ifaddrs *addrs, *tmp;
	getifaddrs(&addrs);
	tmp = addrs;
	while (tmp)
	{
		if (tmp->ifa_addr && tmp->ifa_addr->sa_family == AF_PACKET)
		{
			setsockopt(writing_fd, SOL_SOCKET, SO_BINDTODEVICE, tmp->ifa_name, sizeof(tmp->ifa_name));
		}
		tmp = tmp->ifa_next;
	}
	freeifaddrs(addrs);

	int data, internal_sequence_number[NUMBER_OF_NODES];
	for (int i = 0; i < NUMBER_OF_NODES; i++)
		internal_sequence_number[i] = -1;
	message sending_message;

	// LISTENING LOOP
	int sender_id, sequence_number, ready_to_send;
	message recieving_message;
	fd_set read_fds;
	struct timeval timeout;

	printf("Node initialized\n");
	fflush(stdout);

	while (1)
	{
		// Handle select parameters
		FD_ZERO(&read_fds);
		FD_SET(listening_fd, &read_fds);
		FD_SET(generator_pipe[0], &read_fds);
		timeout.tv_sec = 0;
		timeout.tv_usec = SELECT_RATE_US;

		ready_to_send = select(listening_fd + 1, &read_fds, NULL, NULL, &timeout);
		if (ready_to_send > 0)
		{
			if (FD_ISSET(generator_pipe[0], &read_fds))
			{ // The pipe has a message ready

				ret = read(generator_pipe[0], &data, sizeof(data));
				if (ret < sizeof(data))
					handle_error("read");
				
				// Tell the analyzer that we recieved some data
				ret = write(analyzer_pipe[1], &node_id, sizeof(node_id));
				if (ret <= 0)
					handle_error("write");

				sending_message.data = data;
				sending_message.sender_id = node_id;
				sending_message.sequence_number = 0;

				ret = sendto(writing_fd, &sending_message, sizeof(message), 0, (struct sockaddr *)&broadcast_addr, sizeof(broadcast_addr));
				if (ret < sizeof(data))
					handle_error("sendto");
			}
			if (FD_ISSET(listening_fd, &read_fds))
			{ // The socket has a message ready
				ret = read(listening_fd, &recieving_message, sizeof(message));

				sender_id = recieving_message.sender_id;
				sequence_number = recieving_message.sequence_number;
				data = recieving_message.data;

				if (sender_id == node_id)
					continue;
				// If the package's sequence number is higher than the internal one, discard it.
				if (sequence_number >= internal_sequence_number[sender_id] && internal_sequence_number[sender_id] != -1)
					continue;

				// Tell the analyzer that we recieved some data
				ret = write(analyzer_pipe[1], &sender_id, sizeof(sender_id));
				if (ret <= 0)
					handle_error("write");

				internal_sequence_number[sender_id] = sequence_number;

				sending_message.data = data;
				sending_message.sender_id = node_id;
				sending_message.sequence_number = ++internal_sequence_number[sender_id];

				ret = sendto(writing_fd, &sending_message, sizeof(message), 0, (struct sockaddr *)&broadcast_addr, sizeof(broadcast_addr));
				if (ret < 0)
					handle_error("sendto");
			}
		}
	}
}

int main(int argc, char *argv[])
{
	setvbuf(stdout, NULL, _IONBF, 0);

	if (argc < 2)
	{
		printf("You need to send the id of this node\n");
		exit(EXIT_FAILURE);
	}

	int ret, generator_pipe[2], analyzer_pipe[2];

	ret = pipe(generator_pipe);
	if (ret < 0)
		handle_error("pipe");
	ret = pipe(analyzer_pipe);
	if (ret < 0)
		handle_error("pipe");

	ret = fork();
	if (ret < 0)
		handle_error("fork");
	if (ret > 0)
	{
		traffic_analyzer(analyzer_pipe);
		// We should never get here
		_exit(EXIT_SUCCESS);
	}
	ret = close(analyzer_pipe[0]);
	if (ret < 0)
		handle_error("close");

	ret = fork();
	if (ret < 0)
		handle_error("fork");
	if (ret > 0)
	{
		traffic_generator(generator_pipe);
		// We should never get here
		_exit(EXIT_SUCCESS);
	}
	ret = close(generator_pipe[1]);
	if (ret < 0)
		handle_error("close");

	broadcaster(atoi(argv[1]), generator_pipe, analyzer_pipe);
}