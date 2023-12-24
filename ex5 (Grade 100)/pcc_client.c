#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <arpa/inet.h>
#include <errno.h>
#include <stdint.h>
#include <sys/stat.h>
#include <fcntl.h>

#define BUFFER_SIZE 1024

// send exactly count bytes from buffer over the socket represented by sockfd
// returns negative value on error and errno will be set (by the write syscall), returns 0 otherwise
int sendall(int sockfd, char * buffer, size_t count)
{
	char * remaining_start = buffer;
	size_t remaining_count = count;

	while (remaining_count > 0)
	{
		ssize_t written = write(sockfd, remaining_start, remaining_count);
		if (written < 0) // propagate error return value (should be -1)
		{
			return written;
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
			return bytes_read;
		}

		remaining_start += bytes_read;
		remaining_count -= bytes_read;
	}
	return 0;
}

// code partially based on networks recitation code
int main(int argc, char *argv[])
{
	if (argc != 4)
	{
		fprintf(stderr, "wrong number of arguments\n");
		return 1;
	}

	struct sockaddr_in serv_addr;
	int	sockfd = -1;
	int filefd = -1;
	char sendbuf[BUFFER_SIZE];
	struct stat file_stats;
	uint32_t N_network;
	int finished_reading = 0;
	uint32_t C_network;

	if( (filefd = open(argv[3], O_RDONLY)) < 0)
	{
		perror("error opening file");
		return 1;
	}

	if( (sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
	{
		perror("error creating socket");
		return 1;
	}

	memset(&serv_addr, 0, sizeof(serv_addr));
	serv_addr.sin_family = AF_INET;
	serv_addr.sin_port = htons(atoi(argv[2]));
	inet_pton(AF_INET, argv[1], (void *) &serv_addr.sin_addr.s_addr);

	if(connect(sockfd,
				(struct sockaddr*) &serv_addr,
				sizeof(serv_addr)) < 0)
	{
		perror("error connecting to server");
		return 1;
	}

	// read the file stats to find out its size
	if(fstat(filefd, &file_stats) < 0)
	{
		perror("error in reading file stats");
		return 1;
	}

	// set N to the size of the file, in network byte order and send it to server
	N_network = htonl((uint32_t)file_stats.st_size);
	if (sendall(sockfd, (char *) &N_network, sizeof(N_network)) < 0)
	{
		perror("error in sending file size N");
		return 1;
	}

	while (!finished_reading)
	{
		ssize_t bytes_read = read(filefd, sendbuf, BUFFER_SIZE);
		if (bytes_read < 0)
		{
			perror("error reading from file");
			return 1;
		}
		else if (bytes_read == 0)
		{
			// no more bytes to send
			finished_reading = 1;
		}
		else
		{
			// send the bytes that were read
			if (sendall(sockfd, sendbuf, bytes_read) < 0)
			{
				perror("error in sending file data");
				return 1;
			}
		}
	}

	// read the amount of printable characters as counted by the server (in network byte order) and print it
	if (readall(sockfd, (char *) &C_network, sizeof(C_network)) < 0)
	{
		perror("error in reading printable characters count C");
		return 1;
	}
	printf("# of printable characters: %u\n", ntohl(C_network));

	close(sockfd);
	close(filefd);
	return 0;
}
