#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/wait.h> // waitpid
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <string.h>
#include <assert.h>
#include <ctype.h> // tolower

#define TRUE 1
#define FALSE 0

#define READ_END 0
#define WRITE_END 1
#define EXEC_FAILED 'F'

#define BUFF_SIZE 100
#define LENGTH_BYTES 1

// i/o multiplexing
#define CL_IN           3 // sock
#define CL_OUT          4 // sock
#define SV_TO_TM_CMD    5 // pipe
#define SV_TO_TM_RESULT 6 // pipe
#define TM_TO_SV_CMD    7 // pipe
#define TM_TO_SV_RESULT 8 // pipe

#define MAX_CLIENTS 5


#define VERTICAL_LINE "\u2502"
#define HORIZONTAL_LINE "\u2500"

typedef struct
{
	pid_t pid;
    int cmd_from;
    int result_from;
    int cmd_to;
    int result_to;
} task_manager;

typedef struct
{
	task_manager* tm;
    int msgsock;
    // in_addr_t ip;
    char* ip_str;
    int port;
    struct sockaddr_in* info;
} client;

typedef struct clnode
{
	client* cl;
	struct clnode* next;
} clnode;
static clnode* clist_head = NULL;
static int client_count = 0;

int sock;
struct sockaddr_in server;

#define MAX_EVENTS (MAX_CLIENTS + 2)
static struct epoll_event events[MAX_EVENTS];
int epfd;

void handle_client_input(client* cl);
void handle_stdin_input();
void list_clients();
void register_signal_handlers();
void add_connection_listener();
void add_stdin_listener();
void add_client();
client* make_client(task_manager* tm, int msgsock, struct sockaddr_in* cl_info);
task_manager* make_TM(int msgsock);
int add_client_listeners(client* cl);
void add_to_client_list(client* cl);
void initialize_server();
void make_socket();
void bind_socket();
void print_port();
void free_client(client* cl);
void disconnect_client(client* cl);
void search_and_disconnect(char* ip_str, int port);
void disconnect_all();
void rm_client(clnode* node);
void rm_all_clients();
void rm_recurse(clnode* node);
void printify(const char* str, ...);
void sigchld_handler(int signo);
void exit_gracefully();
void exit_handler(int signo);
void lower(char* str);
void hr();

int main()
{
	register_signal_handlers();

	initialize_server();
	epfd = epoll_create1(0);
	if (epfd == -1)
	{
		perror("epoll_create1");
		exit(EXIT_FAILURE);
	}
	add_connection_listener();
	add_stdin_listener();
	while (TRUE)
	{
		int nr = epoll_wait(epfd, events, MAX_EVENTS, -1);
		if (nr < 0)
		{
			if (errno == EINTR)
				continue;
			else
			{
				perror("epoll_wait");
				exit_gracefully();
			}
		}
		for (int i = 0; i < nr; ++i)
		{
			struct epoll_event e = events[i];
			if (e.events == EPOLLIN)
			{
				if (e.data.fd == STDIN_FILENO) //TODO
				{
					// printify("Such interactivity. Much wow.\n");
					handle_stdin_input();
				}
				else if ((e.data.fd == sock) && (client_count < MAX_CLIENTS))
				{
					add_client();
			        // printify("Client added.\n");
				}
				else //TODO
				{
					// printify("Client input detected.\n");
					client* cl = (client*) e.data.ptr;
					// printify("Client %d.\n", cl->tm->pid);
					handle_client_input(cl);
				}
			}
		}
	}
	exit_gracefully();
	return 0;
}

void handle_client_input(client* cl)
{
	assert(cl != NULL);
	assert(cl->tm != NULL);
	char buff[BUFF_SIZE];
	int r;
	// printify("Reading result pipe\n");
	while ((r = read(cl->tm->result_from, buff, BUFF_SIZE)) > 0)
	{
		write(STDOUT_FILENO, buff, r);
	}
	// printify("Reading cmd pipe\n");
    // input length
	int len = 0;
	r = read(cl->tm->cmd_from, &len, LENGTH_BYTES);
    if (r <= 0) // no input at the moment
    {
        return;
    }
    assert(len <= BUFF_SIZE);
    // actual input
	char input[BUFF_SIZE];
	r = read(cl->tm->cmd_from, input, len);
    if (r == -1)
    {
        perror("SV read cmd");
        return;
    }
    if (r < len)
    {
        printify("Incomplete read. len: %d, r: %d\n", len, r);
        return;
    }
	char* cmd = strtok(input, " ");
	if (!strcmp(cmd, "msg"))
	{
		char* msg = strtok(NULL, "");
		if (!msg)
			return;
		printify("Client %d says: %s\n", cl->tm->pid, msg);
	}
	// printify("Done reading from client\n");
}

void handle_stdin_input()
{
	char input[BUFF_SIZE];
	int r;
	// printify("Reading stdin\n");
	r = read(STDIN_FILENO, input, BUFF_SIZE);
	if (r <= 0)
	{
		perror("stdin read");
		return;
	}
	input[r-1] = '\0';
    lower(input);
    // keep a copy of the original input
    int original_len = strlen(input) + 1;
    char original[original_len];
    memcpy(original, input, original_len);

	char* cmd = strtok(input, " ");
	if (!strcmp(cmd, "broadcast")) // broadcast
	{
		clnode* clptr;
		for (clptr = clist_head; clptr; clptr = clptr->next)
		{
			write(clptr->cl->tm->cmd_to, &original_len, LENGTH_BYTES);
			write(clptr->cl->tm->cmd_to, original, original_len);
		}
	}
	else if (!strcmp(cmd, "q") || !strcmp(cmd, "ex") || !strcmp(cmd, "quit") || !strcmp(cmd, "exit"))
    {
        exit_gracefully();
    }
	else if (!strcmp(cmd, "list"))
	{
		list_clients();
	}
	else if (!strcmp(cmd, "cl"))
	{
		// printify("cl-ing\n");
		char* ip_str = strtok(NULL, " :");
		int port = atoi(strtok(NULL, " :"));
		if (!ip_str || !port)
		{
			printify("Usage: disconnect <client-ip>:<client-port>");
			return;
		}
		char* cmd_to_fwd = strtok(NULL, "");
		int len = strlen(cmd_to_fwd) + 1;

		clnode* clptr;
		for (clptr = clist_head; clptr; clptr = clptr->next)
		{
			client* cl = clptr->cl;
			if (!strcmp(cl->ip_str, ip_str) && (cl->port == port))
			{
				write(cl->tm->cmd_to, &len, LENGTH_BYTES);
				write(cl->tm->cmd_to, cmd_to_fwd, len);
			}
		}
		return;
	}
	else if (!strcmp(cmd, "disconnect"))
	{
		// printify("disconnecting\n");
		char* arg1 = strtok(NULL, " :");
		if (!arg1)
		{
			printify("Usage: disconnect all %s * %s <client-ip>:<client-port>", VERTICAL_LINE);
			return;
		}
		if (!strcmp(arg1, "all") || !strcmp(arg1, "*"))
		{
			disconnect_all();
			return;
		}
		char* ip_str = arg1;
		int port = atoi(strtok(NULL, " :"));
		if (!ip_str || !port)
		{
			printify("Usage: disconnect <client-ip>:<client-port>");
			return;
		}
		search_and_disconnect(ip_str, port);
	}
	// printify("Done reading from client\n");
}

void list_clients()
{
	hr();
	printify(" %-6s %s %-11s %s %-5s\n", "PID", VERTICAL_LINE, "IP", VERTICAL_LINE, "PORT");
	hr();
	clnode* clptr;
	for(clptr = clist_head; clptr; clptr = clptr->next)
	{
		client* cl = clptr->cl;
		printify(" %-6d %s %-11s %s %-5d\n", cl->tm->pid, VERTICAL_LINE, cl->ip_str, VERTICAL_LINE, cl->port);
	}
	hr();
}

void register_signal_handlers()
{
	if (signal(SIGCHLD, sigchld_handler) == SIG_ERR)
    {
        perror("signal: SIGCHLD");
		exit(EXIT_FAILURE);
    }
    if (signal(SIGINT, exit_handler) == SIG_ERR)
    {
        perror("signal: SIGINT");
		exit(EXIT_FAILURE);
    }
    if (signal(SIGTERM, exit_handler) == SIG_ERR)
    {
        perror("signal: SIGTERM");
		exit(EXIT_FAILURE);
    }
}

void add_connection_listener()
{
	struct epoll_event incoming_connection_event;
	incoming_connection_event.data.fd = sock;
	incoming_connection_event.events = EPOLLIN;
	int r = epoll_ctl(epfd, EPOLL_CTL_ADD, sock, &incoming_connection_event);
	if (r == -1)
	{
		perror("add_connection_listener: epoll_ctl");
		exit(EXIT_FAILURE);
	}
}

void add_stdin_listener()
{
	struct epoll_event user_input;
	user_input.data.fd = STDIN_FILENO;
	user_input.events = EPOLLIN;
	int r = epoll_ctl(epfd, EPOLL_CTL_ADD, STDIN_FILENO, &user_input);
	if (r == -1)
	{
		perror("add_stdin_listener: epoll_ctl");
		exit(EXIT_FAILURE);
	}
}

void add_client()
{
	struct sockaddr_in* cl_info = malloc(sizeof(*cl_info));
	socklen_t length = sizeof(*cl_info);

	int msgsock = accept(sock, (struct sockaddr *) cl_info, &length);
	if (msgsock == -1)
	{
		perror("accept");
		free(cl_info);
		return;
	}

	task_manager* tm = make_TM(msgsock);
    if (!tm)
    {
    	printify("Failed to start TM\n");
    	return;
    }
	// printify("TM started.\n");
	
	client* cl = make_client(tm, msgsock, cl_info);
    // printify("Client created.\n");

	if (add_client_listeners(cl) != 0)
	{
		perror("cl_input: epoll_ctl");
		free_client(cl);
		return;
	}
    // printify("Client-listener added.\n");

	add_to_client_list(cl);
}

client* make_client(task_manager* tm, int msgsock, struct sockaddr_in* cl_info)
{
	client* cl = malloc(sizeof(*cl));

	char* ipbuf = malloc(INET_ADDRSTRLEN);
	inet_ntop(AF_INET, &((cl_info->sin_addr).s_addr), ipbuf, INET_ADDRSTRLEN);
	cl->tm = tm;
	cl->msgsock = msgsock;
	cl->ip_str = ipbuf;
	// cl->ip = ((cl_info->sin_addr).s_addr);
	cl->port = ntohs(cl_info->sin_port);
	cl->info = cl_info;
	printf("%s:%d has connected.\n", cl->ip_str, cl->port);
	return cl;
}

task_manager* make_TM(int msgsock)
{
	int p2c_cmd[2];
	int p2c_res[2];
	int c2p_cmd[2];
	int c2p_res[2];
    int exec_check_pipe[2];
    if ((pipe2(exec_check_pipe, O_CLOEXEC) == -1) || (pipe2(p2c_cmd, O_NONBLOCK) == -1) || (pipe2(p2c_res, O_NONBLOCK) == -1) ||
    	(pipe2(c2p_cmd, O_NONBLOCK) == -1) || (pipe2(c2p_res, O_NONBLOCK) == -1))
    {
    	perror("pipe");
    	return NULL;
    }
    pid_t pid = fork();
    if (pid == -1)
    {
        perror("add_process: fork");
		shutdown(msgsock, SHUT_RDWR);
		close(msgsock);
    	return NULL;
    }
    if (pid > 0) // parent
    {
    	close(exec_check_pipe[WRITE_END]);
		close(p2c_cmd[READ_END]);
		close(p2c_res[READ_END]);
		close(c2p_cmd[WRITE_END]);
		close(c2p_res[WRITE_END]);
    	char c;
    	if (read(exec_check_pipe[READ_END], &c, 1) == -1)
    	{
			perror("pipe read");
    		close(exec_check_pipe[READ_END]);
			close(p2c_cmd[WRITE_END]);
			close(p2c_res[WRITE_END]);
			close(c2p_cmd[READ_END]);
			close(c2p_res[READ_END]);
            return NULL;
    	}
        if (c == EXEC_FAILED)
        {
        	printify("exec failed\n");
            waitpid(pid, NULL, 0);
	    	close(exec_check_pipe[READ_END]);
			close(p2c_cmd[WRITE_END]);
			close(p2c_res[WRITE_END]);
			close(c2p_cmd[READ_END]);
			close(c2p_res[READ_END]);
            return NULL;
        }
    	close(exec_check_pipe[READ_END]);
    }
	else // child
	{
    	close(exec_check_pipe[READ_END]);
		close(p2c_cmd[WRITE_END]);
		close(p2c_res[WRITE_END]);
		close(c2p_cmd[READ_END]);
		close(c2p_res[READ_END]);
		char c = EXEC_FAILED;
		// replace fds
		close(STDIN_FILENO);
		if ((dup2(msgsock, CL_IN) == -1) || (dup2(msgsock, CL_OUT) == -1) || 
			(dup2(p2c_cmd[READ_END], SV_TO_TM_CMD) == -1) || (dup2(c2p_cmd[WRITE_END], TM_TO_SV_CMD) == -1) || 
			(dup2(p2c_res[READ_END], SV_TO_TM_RESULT) == -1) || (dup2(c2p_res[WRITE_END], TM_TO_SV_RESULT) == -1))
		{
			perror("dup2");
			write(exec_check_pipe[WRITE_END], &c, 1);
			exit(EXIT_FAILURE);
		}
		// launch the task manager
		if (execl("./tm", "tm", NULL) == -1)
		{
			perror("exec");
			write(exec_check_pipe[WRITE_END], &c, 1);
		}
		exit(EXIT_FAILURE);
	}
	task_manager* tm = malloc(sizeof(*tm));
	tm->pid = pid;
	tm->cmd_from = c2p_cmd[READ_END];
	tm->result_from = c2p_res[READ_END];
	tm->cmd_to = p2c_cmd[WRITE_END];
	tm->result_to = p2c_res[WRITE_END];
	return tm;
}

int add_client_listeners(client* cl)
{
	struct epoll_event cl_input;
	cl_input.data.ptr = cl;
	cl_input.events = EPOLLIN;
	return  epoll_ctl(epfd, EPOLL_CTL_ADD, cl->tm->cmd_from, &cl_input) +
			epoll_ctl(epfd, EPOLL_CTL_ADD, cl->tm->result_from, &cl_input);
}

void rm_client_listeners(client* cl)
{
	epoll_ctl(epfd, EPOLL_CTL_DEL, cl->tm->cmd_from, NULL);
	epoll_ctl(epfd, EPOLL_CTL_DEL, cl->tm->result_from, NULL);
    // printify("listener removed\n");
}

void add_to_client_list(client* cl)
{
	clnode* new_node = malloc(sizeof(*new_node));
	new_node->cl = cl;
	new_node->next = clist_head;
	clist_head = new_node;
	client_count++;
}

void initialize_server()
{
	make_socket();
	bind_socket();
	print_port();
	listen(sock, MAX_CLIENTS);
}

void make_socket()
{
	sock = socket(AF_INET, SOCK_STREAM, 0);
	if (sock < 0) 
	{
		perror("opening stream socket");
		exit(EXIT_FAILURE);
	}
}

void print_port()
{
	socklen_t length = sizeof(server);
	if (getsockname(sock, (struct sockaddr *) &server, &length)) 
	{
		perror("getting socket name");
		exit(1);
	}
	printify("Socket has port #%d\n", ntohs(server.sin_port));
}

void bind_socket()
{
	server.sin_family = AF_INET;
	server.sin_addr.s_addr = INADDR_ANY;
	server.sin_port = 0;
	if (bind(sock, (struct sockaddr *) &server, sizeof(server))) 
	{
		perror("binding stream socket");
		exit(EXIT_FAILURE);
	}	
}

void free_client(client* cl)
{
	assert(cl != NULL);
	free(cl->tm);
	free(cl->info);
	free(cl->ip_str);
	free(cl);
}

void disconnect_client(client* cl)
{
	assert(cl != NULL);
	assert(cl->tm != NULL);
	shutdown(cl->msgsock, SHUT_RDWR);
	close(cl->msgsock);
	close(cl->tm->cmd_from);
	close(cl->tm->cmd_to);
	close(cl->tm->result_from);
	close(cl->tm->result_to);
	kill(cl->tm->pid, SIGTERM);
	waitpid(cl->tm->pid, NULL, 0);
	printf("%s:%d disconnected.\n", cl->ip_str, cl->port);
}

void search_and_disconnect(char* ip_str, int port)
{	
	clnode* clptr1;
    clnode* clptr2;
    clptr1 = clptr2 = clist_head;
	for (; clptr2; clptr2 = clptr2->next, clptr1 = clptr2)
    {
    	client* cl = clptr2->cl;
    	assert(cl != NULL);
    	if (!strcmp(cl->ip_str, ip_str) && (cl->port == port))
    	{
		    // printify("found child\n");
    		if ((clptr1 == clptr2) && (clptr1 == clist_head))
    		{
    			if (client_count == 1)
	    			clist_head = NULL;
	    		else
		    		clist_head = clptr2->next;
    		}
	    	rm_client(clptr2);
			printf("%s:%d disconnected.\n", cl->ip_str, cl->port);
    		return;
    	}
    }
    printify("Couldn't find client %s:%d\n", ip_str, port);
}

void disconnect_all()
{
    rm_all_clients();
}

void rm_client(clnode* node)
{
	assert(node != NULL);
	client* cl = node->cl;
	rm_client_listeners(cl);
	disconnect_client(cl);
	free_client(cl);
	free(node);
	client_count--;
}

void rm_all_clients()
{
	if (signal(SIGCHLD, SIG_IGN) == SIG_ERR)
    {
        perror("rm_all_clients: signal: SIGCHLD(SIG_IGN)");
		exit(EXIT_FAILURE);
    }
    // assert(signal(SIGCHLD, SIG_IGN) == SIG_IGN);
	rm_recurse(clist_head);

	if (signal(SIGCHLD, sigchld_handler) == SIG_ERR)
    {
        perror("rm_all_clients: signal: SIGCHLD");
		exit(EXIT_FAILURE);
    }
    clist_head = NULL;
}

void rm_recurse(clnode* node)
{
	// printify("recurse\n");
	if (node == NULL)
		return;
	clnode* nxt = node->next;
	rm_client(node);
	rm_recurse(nxt);
}

void sigchld_handler(int signo)
{
    pid_t pid;
    pid = waitpid(-1, NULL, 0);
    if (pid < 0)
        return;
    // printify("killing child %d\n", pid);
    clnode* clptr1;
    clnode* clptr2;
    clptr1 = clptr2 = clist_head;
    for (; clptr2; clptr2 = clptr2->next, clptr1 = clptr2)
    {
    	assert(clptr2->cl != NULL);
    	assert(clptr2->cl->tm != NULL);
    	if (clptr2->cl->tm->pid == pid)
    	{
		    // printify("found child\n");
    		if ((clptr1 == clptr2) && (clptr1 == clist_head))
    		{
    			if (client_count == 1)
	    			clist_head = NULL;
	    		else
		    		clist_head = clptr2->next;
    		}
	    	rm_client(clptr2);
    		printify("%d removed.\n", pid);
    		return;
    	}
    }
}

void printify(const char* str, ...)
{
    char buff[BUFF_SIZE];

    va_list args;
    va_start(args, str);

    if (write(STDOUT_FILENO, buff, vsprintf(buff, str, args)) == -1)
    {
        perror("Server: printify: write");
        if (errno == EBADF)
        {
        	exit(EXIT_FAILURE);
        }
    }

    va_end(args);
    fsync(STDOUT_FILENO);
}

void exit_gracefully()
{
	exit_handler(0);
}

void exit_handler(int signo)
{
    rm_all_clients();
    close(sock);
    exit(signo);
}

void lower(char* str)
{
    char* c;
    for(c = str; *c; c++)
        *c = tolower(*c);
}

void hr()
{
    int i;
    for(i = 0; i < 76; ++i)
        printify("%s", HORIZONTAL_LINE);
    printify("\n");
}