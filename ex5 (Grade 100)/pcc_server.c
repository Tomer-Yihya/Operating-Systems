#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <stdint.h>
#include <signal.h>

#define BUFFER_SIZE 1024
#define PRINTABLE_START 32
#define PRINTABLE_END 126
#define PRINTABLE_COUNT (PRINTABLE_END - PRINTABLE_START + 1)

int sigint_happened = 0; // global flag to signify a SIGINT happened and the server needs to be stopped

void sigint_handler(int signum)
{
	sigint_happened = 1;
}

// send exactly count bytes from buffer over the socket represented by sockfd
// returns negative value on error and errno will be set (by the write syscall), returns 0 otherwise
int sendall(int sockfd, char * buffer, size_t count)
{
	char * remaining_start = buffer;
	size_t remaining_count = count;

	while (remaining_count > 0)
	{
		ssize_t written = write(sockfd, remaining_start, remaining_count);
		if (written < 0) 
		{
			if (errno == EINTR) // if we were interrupted by a signal handler we still need to continue writing
			{
				continue;
			}
			return written; // propagate error return value (should be -1)
		}

		remaining_start += written;
		remaining_count -= written;
	}
	return 0;
}

// read exactly count bytes into buffer from the socket represented by sockfd
// returns negative value on error and errno will be set (by the read syscall), returns 0 otherwise
int readall(int sockfd, char * buffer, size_t count)
{
	char * remaining_start = buffer;
	size_t remaining_count = count;

	while (remaining_count > 0)
	{
		ssize_t bytes_read = read(sockfd, remaining_start, remaining_count);
		if (bytes_read < 0) // propagate error return value (should be -1)
		{
			if (errno == EINTR) // if we were interrupted by a signal handler we still need to continue reading
			{
				continue;
			}
			return bytes_read;
		}

		remaining_start += bytes_read;
		remaining_count -= bytes_read;
	}
	return 0;
}

// serve a specific client through connfd, updating pcc_total as needed
// returns 0 for success or non-fatal (TCP connection) errors, -1 for fatal errors
// in both cases writes error descriptions to stderr
int serve_client(int connfd, uint32_t *pcc_total)
{
	char data_buff[BUFFER_SIZE];
	uint32_t C_host = 0; // count of printable characters in host order
	uint32_t C_network; // count of printable characters in network order
	uint32_t N_network; // will contain the number sent from client in network order
	uint32_t remaining; // remaining bytes to read
	uint32_t pcc_total_local[PRINTABLE_COUNT] = {0}; // init local counts with zeroe

	// read 4 bytes representing N from the socket
	if (readall(connfd, (char *) &N_network, sizeof(N_network)) < 0)
	{
		// TCP error - print and return "success"
		if (errno == ETIMEDOUT || errno == ECONNRESET || errno == EPIPE)
		{
			perror("TCP error in client connection while reading N");
			return 0;
		}
		else // exit the server for other errors
		{
			perror("error in reading N");
			return -1;
		}
	}

	remaining = ntohl(N_network); // set the amount of remaining bytes to read to what the client said (in host order)

	while (remaining > 0)
	{
		// try to read either BUFFER_SIZE bytes or all remaining, whichever is smaller
		ssize_t bytes_read = read(connfd, data_buff, remaining < BUFFER_SIZE ? remaining : BUFFER_SIZE);
		if (bytes_read < 0)
		{
			// TCP error - print and return "success"
			if (errno == ETIMEDOUT || errno == ECONNRESET || errno == EPIPE)
			{
				perror("TCP error in client connection while reading file data");
				return 0;
			}
			else if (errno == EINTR) // if we were interrupted by a signal handler we still need to continue reading
			{
				continue;
			}
			else // exit the server for other errors
			{
				perror("error in reading file data");
				return -1;
			}
		}

		else if (bytes_read == 0) // we didn't read anything while still expecting data, this means unexpectedly closed connection
		{
			fprintf(stderr, "client unexpectedly closed connection\n");
			return 0; // no need to exit the server
		}

		else // if we actually read something, count printable characters and decrease remaining counter
		{
			for (uint32_t i = 0; i < bytes_read; i++)
			{
				if (data_buff[i] >= PRINTABLE_START && data_buff[i] <= PRINTABLE_END) // data_buff[i] is printable
				{
					C_host++; // update count of all printable characters
					pcc_total_local[data_buff[i] - PRINTABLE_START]++; // update count of the specific character read
				}
			}

			remaining -= bytes_read;
		}
	}

	// convert C to network order and send it
	C_network = htonl(C_host);
	if (sendall(connfd, (char *) &C_network, sizeof(C_network)) < 0)
	{
		// TCP error - print and return "success"
		if (errno == ETIMEDOUT || errno == ECONNRESET || errno == EPIPE)
		{
			perror("TCP error in client connection while sending C");
			return 0;
		}
		else // exit the server for other errors
		{
			perror("error in sending C");
			return -1;
		}
	}

	// update global pcc_total counts
	for (int i = 0; i < PRINTABLE_COUNT; i++)
	{
		pcc_total[i] += pcc_total_local[i];
	}

	return 0;
}

// code partially based on networks and signal recitations code
int main(int argc, char *argv[])
{
	if (argc != 2)
	{
		fprintf(stderr, "wrong number of arguments\n");
		return 1;
	}

	struct sigaction newAction = {.sa_handler = sigint_handler};
	if (sigaction(SIGINT, &newAction, NULL) == -1) {
		perror("signal handle registration failed");
		return 1;
	}

	int listenfd	= -1;
	struct sockaddr_in serv_addr;
	uint32_t pcc_total[PRINTABLE_COUNT] = {0}; // init pcc_total as an array with the needed size filled with zeroes
	int reuse_addr_value = 1; // value of boolean flag for SO_REUSEADDR

	if( (listenfd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
	{
		perror("error creating socket");
		return 1;
	}

	// set SO_REUSEADDR value to be able to quickly reuse the port
	if( setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &reuse_addr_value, sizeof(reuse_addr_value)) < 0)
	{
		perror("error setting SO_REUSEADDR");
		return 1;
	}

	memset( &serv_addr, 0, sizeof(serv_addr) );
	serv_addr.sin_family = AF_INET;
	serv_addr.sin_addr.s_addr = htonl(INADDR_ANY); // bind to all network addresses
	serv_addr.sin_port = htons(atoi(argv[1]));

	if( bind( listenfd,
					 (struct sockaddr*) &serv_addr,
					 sizeof(serv_addr) ) < 0)
	{
		perror("bind failed");
		return 1;
	}

	if( listen( listenfd, 10 ) < 0 )
	{
		perror("listen failed");
		return 1;
	}

	while( !sigint_happened ) // run the server until a SIGINT was detected
	{
		int connfd = accept( listenfd, NULL, NULL );

		if( connfd < 0 )
		{
			if (errno == EINTR) // if we were interrupted by a signal handler continue to next iteration to try again (or exit for SIGINT)
			{
				continue;
			}
			else
			{
				perror("accept failed");
				return 1;
			}
		}

		// serve the client, updating pcc_total as needed. exit if a fatal error occured (serve_client prints the error message)
		if (serve_client(connfd, pcc_total) < 0)
		{
			return 1;
		}

		close(connfd);
	}

	for (char c = PRINTABLE_START; c <= PRINTABLE_END; c++)
	{
		printf("char '%c' : %u times\n", c, pcc_total[c - PRINTABLE_START]);
	}

	return 0;
}
