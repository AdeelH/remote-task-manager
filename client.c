#define _GNU_SOURCE
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <netdb.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <stdarg.h>
#include <fcntl.h>
#include <poll.h>
#include <errno.h>
#include <ctype.h> // tolower

#define TRUE 1
#define FALSE 0

#define READ_END 0
#define WRITE_END 1

#define BUFF_SIZE 100
#define LENGTH_BYTES 1

#define PROMPT ": "

void printify(const char* str, ...);
void lower(char* str);
void disconnect();
void exit_gracefully();
void exit_handler(int signo);
void sig_conn_closed_handler();

int sock;
int connection_open = FALSE;

int main(int argc, char *argv[])
{
	if (signal(SIGINT, exit_handler) == SIG_ERR)
	{
		perror("signal: SIGINT");
		return -1;
	}
	if (signal(SIGTERM, exit_handler) == SIG_ERR)
	{
		perror("signal: SIGTERM");
		return -1;
	}
	struct sockaddr_in server;
	struct hostent *hp;
	server.sin_family = AF_INET;

	while(TRUE)
	{
		printify("");
		char input[BUFF_SIZE];
		int c;
		if ((c = read(STDIN_FILENO, input, BUFF_SIZE)) < 0)
		{
			perror("Reading from stdin");
		}
		if (c == 1)
			continue;
		input[c-1] = '\0';
		lower(input);
		char* cmd = strtok(input, " ");
		if (!strcmp(cmd, "conn") || !strcmp(cmd, "connect"))
		{
			char* host = strtok(NULL, " ");
			char* port = strtok(NULL, " ");
			if (!(host && port))
			{
				printify("Usage: conn[ect] <host> <port>\n");
				continue;
			}
			hp = gethostbyname(host);
			if (!hp) 
			{
				printify("%s: unknown host\n", argv[1]);
				continue;
			}
			bcopy(hp->h_addr, &server.sin_addr, hp->h_length);
			server.sin_port = htons(atoi(port));

			if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) 
			{
				perror("opening stream socket");
				exit(EXIT_FAILURE);
			}
			if (connect(sock, (struct sockaddr *) &server, sizeof(server)) < 0) 
			{
				perror("connecting stream socket");
				continue;
			}
			connection_open = TRUE;
			printify("Connected.\n");

			struct pollfd rfds[2];
			rfds[0].fd = STDIN_FILENO;
			rfds[0].events = POLLIN;
			rfds[1].fd = sock;
			rfds[1].events = POLLIN;
			while(connection_open)
			{
				// printify("client waiting for input\n");
				int r, w;    
				if ((r = poll(rfds, 2, -1)) < 0)
				{
					if (errno == EINTR)
						continue;
					else
					{
						perror("poll");
						exit_gracefully();
					}
				}
				if (rfds[1].revents & POLLIN)
				{
					// printify("sock input detected\n");
					char buff[BUFF_SIZE];
					if ((r = read(sock, buff, BUFF_SIZE)) < 0)
					{
						perror("sock read");
						continue;
					}
					if (r == 0)
					{
						// printify("Socket closed.\n");
						connection_open = FALSE;
						// raise(SIG_CONN_CLOSED);
						break;
					}
					if (write(STDOUT_FILENO, buff, r) < 0)
					{
						perror("stdout write");
						continue;
					}
				}
				else if (rfds[0].revents & POLLIN)
				{
					// printify("stdin input detected\n");
					char buff[BUFF_SIZE];
					if ((r = read(STDIN_FILENO, buff, BUFF_SIZE)) < 0)
					{
						perror("stdin read");
						continue;
					}
					buff[r-1] = '\0';
					char tmp[BUFF_SIZE];
					sscanf(buff, "%s", tmp);
					lower(tmp);
					if (!strcmp(tmp, "q") || !strcmp(tmp, "ex") || !strcmp(tmp, "quit") || !strcmp(tmp, "exit"))
					{
						printify("exit cmd detected\n");
						exit_gracefully();
					}
					if ((w = write(sock, &r, LENGTH_BYTES)) < 0)
					{
						perror("Writing to socket");
						printify("Failed to send command size.\n");
						continue;
					}
					// printify("sent cmd length %d\n", r);
					if ((w = write(sock, buff, r)) < 0)
					{
						perror("Writing to socket");
						printify("Failed to send command.\n");
					}
					if (w == 0)
					{
						printify("Zero write.\n");
						break;
					}
					// printify("sent cmd %s\n", buff);
				}
			}
			disconnect();
		}
		else
		{
			printify("Unknown command.\n");
		}
	}
}

void disconnect()
{
	printify("Disconnected.\n");
	shutdown(sock, SHUT_RDWR);
	close(sock);
}

void printify(const char* str, ...)
{
	char buff[BUFF_SIZE];

	va_list args;
	va_start(args, str);

	write(STDOUT_FILENO, PROMPT, sizeof(PROMPT));
	if (write(STDOUT_FILENO, buff, vsprintf(buff, str, args)) == -1)
	{
		perror("printify: write");
	}
	fsync(STDOUT_FILENO);
	va_end(args);
}

void exit_gracefully()
{
	exit_handler(0);
}

void exit_handler(int signo)
{
	int len = 4;
	write(sock, &len, LENGTH_BYTES);
	write(sock, "exit", 4);
	shutdown(sock, SHUT_RDWR);
	close(sock);
	printify("Exiting.\n");
	exit(signo);
}

void lower(char* str)
{
	char* c;
	for(c = str; *c; c++)
		*c = tolower(*c);
}

void sig_conn_closed_handler()
{
	connection_open = FALSE;
}
