#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <sys/wait.h> // waitpid
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/select.h>
#include <sys/socket.h> // shutdown
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <ctype.h> // isspace, tolower

#define TRUE 1
#define FALSE 0

#define READ_END 0
#define WRITE_END 1
#define EXEC_FAILED 'F'
#define BUFF_SIZE 500
#define LENGTH_BYTES 1

#define MAX_INPUT 100
#define MAX_PROCESSES 10
#define ALIVE 1
#define DEAD 0
#define VERTICAL_LINE "\u2502"
#define HORIZONTAL_LINE "\u2500"

// processes
typedef struct
{
	pid_t pid;
    char* name;
    int status;
    struct tm* start;
    struct tm* end;
} process;
static process* processes[MAX_PROCESSES];
static int process_count = 0;

// i/o multiplexing
static int infd, outfd, errfd;
#define CL_IN           3 // sock
#define CL_OUT          4 // sock
#define SV_TO_TM_CMD    5 // pipe
#define SV_TO_TM_RESULT 6 // pipe
#define TM_TO_SV_CMD    7 // pipe
#define TM_TO_SV_RESULT 8 // pipe
static fd_set rfds;
static int numfds;

// function declarations
void wait_for_input();
char* get_input();
void handle_input(char*);
void add_process();
void list();
void list_all(int details);
void kill_by_id(int pid);
void kill_by_name(char* pname, int n);
void kill_all();
void free_all_processes();
void free_process(process* p);
char* first_n_letters(char* s, int n);
void lower(char* str);
void hr();
void sigchld_handler(int signo);
void exit_gracefully(int signo);
void printify(const char* str, ...);
void fprintify(int fd, const char* str, ...);
void perrorize(char* str, int eno);

static time_t time_zero = 0;

int main()
{
    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);
    infd = CL_IN;
    outfd = CL_OUT;
    // register signal handlers
    if (signal(SIGCHLD, sigchld_handler) == SIG_ERR)
    {
        perrorize("signal: SIGCHLD", errno);
        return -1;
    }
    if (signal(SIGTERM, exit_gracefully) == SIG_ERR)
    {
        perrorize("signal: SIGTERM", errno);
        return -1;
    }
    // get (max fd + 1) for select()
    numfds = ((SV_TO_TM_CMD > CL_IN) ? SV_TO_TM_CMD:CL_IN);
    numfds = ((SV_TO_TM_RESULT > numfds) ? SV_TO_TM_RESULT:numfds) + 1;

    wait_for_input();
    
    return 0;
}

/* 
 * Listens on CL_IN, SV_TO_TM_CMD and SV_TO_TM_RESULT simultaneously for commands.
 * Sets infd, outfd and errfd according to where the command has to be read from 
 * and where the response has to be sent.
 */
void wait_for_input()
{
    while(TRUE)
    {
        // printify("TM waiting for input\n");
        int r;
        FD_ZERO(&rfds);
        FD_SET(CL_IN, &rfds);
        FD_SET(SV_TO_TM_CMD, &rfds);
        FD_SET(SV_TO_TM_RESULT, &rfds);
        
        if ((r = select(numfds, &rfds, NULL, NULL, NULL)) < 0)
        {
            // perrorize("select", errno);
        }
        if (r == 0) // not likely to happen because no timeout has been specified, but handle anyway
        {
            continue;
        }
        if (FD_ISSET(CL_IN, &rfds)) // if input coming from client
        {
            infd = CL_IN;
            outfd = CL_OUT;
            errfd = CL_OUT;
            // printify("TM detected client input\n");
            handle_input(get_input());
            fsync(CL_OUT);
        }
        if (FD_ISSET(SV_TO_TM_CMD, &rfds)) // if cmd coming from server
        {
            // printify("TM detected server cmd\n");
            infd = SV_TO_TM_CMD;
            outfd = TM_TO_SV_RESULT;
            errfd = TM_TO_SV_RESULT;
            // printify("TM detected server cmd\n");
            handle_input(get_input());
            fsync(TM_TO_SV_RESULT);
        }
        if (FD_ISSET(SV_TO_TM_RESULT, &rfds)) // if result of a cmd coming from server
        {
            infd = SV_TO_TM_RESULT;
            outfd = CL_OUT;
            errfd = CL_OUT;
            // printify("TM detected server result dump\n");
            // non-blocking read till pipe empty
            int r;
            char buff[BUFF_SIZE];
            while ((r = read(infd, buff, BUFF_SIZE)) > 0)
            {
                write(outfd, buff, r);
            }
            fsync(CL_OUT);
        }
    }
}

/* 
 * Reads the incoming command from infd, converts it to lowercase and returns it as a char* 
 */
char* get_input()
{
    char* input = malloc(MAX_INPUT+2);
    int r, len;
    r = len = 0;
    // read the 1-byte header representing the length of the input command
    r = read(infd, &len, 1);
    if (r == -1)
    {
        perrorize("read", errno);
        return NULL;
    }
    // read only len bytes, to avoid reading the next command, if it is already present
    r = read(infd, input, len);
    if (r == -1)
    {
        perrorize("read", errno);
        return NULL;
    }
    if (r < len)
    {
        printify("Incomplete read. len: %d, r: %d\n", len, r);
        return NULL;
    }
    input[r-1] = '\0'; // null-terminate
    lower(input);
    return input;
}

/* 
 *  Parse command and pass it to the relevant handler method.
 */
void handle_input(char* input)
{
    if (!input)
        return;

    // keep a copy of the original input
    int original_len = strlen(input) + 1;
    char original[original_len];
    memcpy(original, input, original_len);

    // printify("TM received \"%s\"\n", original);
    char* cmd = strtok(input, " ");

    if (!cmd)
        return;

    if (!strcmp(cmd, "q") || !strcmp(cmd, "ex") || !strcmp(cmd, "quit") || !strcmp(cmd, "exit") || !strcmp(cmd, "disconnect"))
    {
        exit_gracefully(0);
    }
    else if (!strcmp(cmd, "broadcast"))
    {
        char* msg = strtok(NULL, "");
        if (!msg)
            return;
        // forward cmd to server
        outfd = CL_OUT;
        printify("SV says: %s\n", msg);
    }
    else if (!strcmp(cmd, "msg"))
    {
        // forward cmd to server
        write(TM_TO_SV_CMD, &original_len, LENGTH_BYTES);
        write(TM_TO_SV_CMD, original, original_len);
        // printify("TM sent msg \"%s\"\n", original);
    }
    else if (!strcmp(cmd, "add"))
    {
        int sum = 0;
        char* numstr = strtok(NULL, " ");
        while (numstr)
        {
            sum += atoi(numstr);
            numstr = strtok(NULL, " ");
        }
        printify("ans = %d\n", sum);
    }
    else if (!strcmp(cmd, "sub"))
    {
        int ans = 0;
        char* numstr = strtok(NULL, " ");
        if (numstr)
            ans = 2*atoi(numstr);
        while (numstr)
        {
            ans -= atoi(numstr);
            numstr = strtok(NULL, " ");
        }
        printify("ans = %d\n", ans);
    }
    else if (!strcmp(cmd, "mul"))
    {
        int prod = 1;
        char* numstr = strtok(NULL, " ");
        while (numstr)
        {
            prod *= atoi(numstr);
            numstr = strtok(NULL, " ");
        }
        printify("ans = %d\n", prod);
    }
    else if (!strcmp(cmd, "div"))
    {
        float ans = 0;
        char* numstr = strtok(NULL, " ");
        if (numstr)
        {
            ans = atoi(numstr);
            ans *= ans;
        }
        while (numstr)
        {
            int tmp = atoi(numstr);
            if (tmp == 0)
            {
                printify("Divide-by-zero error.\n");
                return;
            }
            ans /= tmp;
            numstr = strtok(NULL, " ");
        }
        printify("ans = %.3f\n", ans);
    }
    else if (!strcmp(cmd, "sleep"))
    {
        char* param = strtok(NULL, " ");
        int sec = param ? atoi(param):0;
        sleep(sec);
        printify("Slept for %d seconds.\n", sec);
    }
    else if (!strcmp(cmd, "list"))
    {
        char* param = strtok(NULL, " ");
        if (!param)
        {
            list();
        }
        else if (!strcmp(param, "*") || !strcmp(param, "all"))
        {
            list_all(FALSE);
        }
        else if (!strcmp(param, "-d") || !strcmp(param, "details"))
        {
            list_all(TRUE);
        }
        else
        {
            printify("Usage: list [-d %s *]\n", VERTICAL_LINE);
        }
    }
    else if (!strcmp(cmd, "kill"))
    {
        char* param = strtok(NULL, " ");
        if (!param)
        {
            printify("Usage: kill <processID> %s <processName> %s *\n", VERTICAL_LINE, VERTICAL_LINE); 
            return;
        }
        int pid = 0;
        if ((pid = atoi(param)))
        {
            kill_by_id(pid);
        }
        else if (!strcmp(param, "*") || !strcmp(param, "all"))
        {
            kill_all();
        }
        else
        {
            char* count = strtok(NULL, " ");
            int n = 1;
            if (!count)
                kill_by_name(param, 1);
            else if (!strcmp(count, "*") || !strcmp(count, "all"))
                kill_by_name(param, -1);
            else if ((n = atoi(count)) > 0)
                kill_by_name(param, n);
        }
    }
    else
    {
        int count = 1;
        char* pname = cmd;
        char* param = strtok(NULL, " ");
        if (param)
        {
            if (!strcmp(cmd, "run"))
            {
                pname = param;
                char* tmp = strtok(NULL, " ");
                count = (tmp && (count = atoi(tmp))) ? count:1;
            }
            else
            {
                pname = cmd;
                count = (count = atoi(param)) ? count:1;
            }
        }
        if (process_count + count >= MAX_PROCESSES)
        {
            count = MAX_PROCESSES - process_count;
            printify("Process limit exceeded.\n");
        }
        add_process(pname, count);
    }
    free(input);
}

void list()
{
    if (!process_count) 
    	return;

    hr();
    printify(" %-6s %s %-10s\n", "PID", VERTICAL_LINE, "Name");
    hr();
    int i;
    for(i = 0; i < process_count; i++)
    {
        if (processes[i]->status != ALIVE) continue;
        char* print_name = first_n_letters(processes[i]->name, 10);
        printify(" %6d %s %-10s \n", processes[i]->pid, VERTICAL_LINE, print_name);
        free(print_name);
    }
    hr();
}

void list_all(int details)
{
    if (!process_count) return;

    hr();
    printify(" %-6s %s %-10s %s %-5s", "PID", VERTICAL_LINE, "Name", VERTICAL_LINE, "Status");
    if (details)
        printify(" %s %-8s %s %-8s %s %-8s", /*VERTICAL_LINE, "Priority", */VERTICAL_LINE, "Start", VERTICAL_LINE, "End", VERTICAL_LINE, "Elapsed");
    printify("\n");
    hr();

    int i;
    for(i = 0; i < process_count; i++, printify("\n"))
    {
        char* print_name = first_n_letters(processes[i]->name, 10);
        printify(" %6d %s %-10s %s %-6s ", processes[i]->pid, VERTICAL_LINE,
                                         print_name, VERTICAL_LINE,
                                         processes[i]->status ? "Alive":"Dead");
        free(print_name);
        if (!details) continue;
        char buff[9];
        strftime(buff, sizeof(buff), "%H:%M:%S", (processes[i]->start));
        printify("%s %8s ", VERTICAL_LINE, buff); // start

        strftime(buff, sizeof(buff), "%H:%M:%S", (processes[i]->end));
        printify("%s %8s ", VERTICAL_LINE, buff); // end
        time_t t1 = mktime((processes[i]->start));
        time_t t2;
        if (processes[i]->status != DEAD)
        {
            t2 = time(NULL);
        }
        else
        {
            t2 = mktime((processes[i]->end));
        }
        struct tm* elapsed;
        time_t seconds = difftime(t2, t1);
        elapsed = gmtime(&seconds);
        strftime(buff, sizeof(buff), "%H:%M:%S", elapsed);
        printify("%s %8s", VERTICAL_LINE, buff); // elapsed
    }
    hr();
}

void add_process(char* name, int count)
{
    if (count <= 0)
        return;

    if (process_count >= MAX_PROCESSES)
    {
        printify( "Error: Process limit exceeded.\n" );
        return;
    }
    int c2p[2];
    int r1 = pipe2(c2p, O_CLOEXEC);
    if (r1 == -1)
    {
    	perrorize("pipe", errno);
    	return;
    }
    pid_t cpid = fork();
    if (cpid == -1)
    {
        perrorize("add_process: fork", errno);
        return;
    }
    if (cpid > 0) // parent
    {
    	close(c2p[WRITE_END]);
    	char c;
    	int r2 = read(c2p[READ_END], &c, 1);
    	if (r2 == -1)
    	{
			perrorize("pipe read", errno);
    		close(c2p[READ_END]);
			return;    		
    	}
        if (c == EXEC_FAILED)
        {
            close(c2p[READ_END]);
            waitpid(cpid, NULL, 0);
            return;
        }

		process* new_proc = malloc(sizeof(*new_proc));

		new_proc->pid = cpid;
        new_proc->name = malloc(strlen(name));
		strcpy(new_proc->name, name);
		new_proc->status = ALIVE;

        time_t curr_time = time(NULL);
        new_proc->start = malloc(sizeof(struct tm));
        new_proc->end = malloc(sizeof(struct tm));
        memcpy(new_proc->start, localtime(&curr_time), sizeof(struct tm));
        memcpy(new_proc->end, gmtime(&time_zero), sizeof(struct tm));

		processes[process_count++] = new_proc;
    	close(c2p[READ_END]);
    }
    else // child
    {
    	close(c2p[READ_END]);
    	int r3 = execlp(name, name, NULL); // TODO: accept command line args
    	if (r3 == -1)
    	{
            perrorize("exec", errno);
            printify("Failed to start process.\n");
            char c = EXEC_FAILED;
            r3 = write(c2p[WRITE_END], &c, 1);
            if (r3 == -1)
            {
                perrorize("write", errno);
                close(c2p[WRITE_END]);
                return;
            }
    	}
    	close(c2p[WRITE_END]);
    }    
    add_process(name, count-1);
}

void kill_by_id(int pid)
{
    int i;
    for(i = 0; i < process_count; i++)
    {
        if(processes[i]->status == ALIVE && processes[i]->pid == pid)
        {
            if(kill(processes[i]->pid, SIGTERM) != -1)
            {
                processes[i]->status = DEAD;
                time_t curr_time = time(NULL);
                memcpy(processes[i]->end, localtime(&curr_time), sizeof(struct tm));
            }
            else
            {
                perrorize("kill", errno);
                printify( "Failed to kill process %d.\n", pid);
            }
            return;
        }
    }
    printify( "No running process with process ID %d.\n", pid );
}

void kill_by_name(char* pname, int n)
{
    int death_toll = 0;
    int lim = (n < 0) ? MAX_PROCESSES:n;
    int i;
    for(i = 0; (i < process_count) && (death_toll < lim); i++)
    {
        if(processes[i]->status == ALIVE && !strcmp(processes[i]->name, pname))
        {
            if(kill(processes[i]->pid, SIGTERM) != -1)
            {
                processes[i]->status = DEAD;
                death_toll++;
                time_t curr_time = time(NULL);
                memcpy(processes[i]->end, localtime(&curr_time), sizeof(struct tm));
            }
            else
            {
                perrorize("kill", errno);
                printify( "Failed to kill process %s(%d).\n", pname, processes[i]->pid);
            }
            return;
        }
    }
    printify("%d processes killed\n", death_toll);
}

void kill_all()
{
    if (signal(SIGCHLD, SIG_IGN) == SIG_ERR)
    {
        perror("rm_all_clients: signal: SIGCHLD(SIG_IGN)");
        exit(EXIT_FAILURE);
    }
    int death_toll = 0;
    int i;
    for(i = 0; i < process_count; i++)
    {
        if (processes[i]->status != ALIVE) 
            continue;
        if(kill(processes[i]->pid, SIGTERM) != -1)
        {
            processes[i]->status = DEAD;
            death_toll++;
            time_t curr_time = time(NULL);
            memcpy(processes[i]->end, localtime(&curr_time), sizeof(struct tm));
        }
        else
        {
            perrorize("kill", errno);
            printify( "Failed to kill process %d.\n", processes[i]->pid);
        }
    }
    
    if (signal(SIGCHLD, sigchld_handler) == SIG_ERR)
    {
        perror("rm_all_clients: signal: SIGCHLD");
        exit(EXIT_FAILURE);
    }
    printify("%d processes killed\n", death_toll);
}

void free_all_processes()
{
    int i;
    for(i = 0; i < process_count; i++)
    {
        if (processes[i])
            free_process(processes[i]);
    }
}

void free_process(process* p)
{
    free(p->name);
    free(p->start);
    free(p->end);
    free(p);
}

char* first_n_letters(char* s, int n)
{
    char* ss = malloc(n+1);
    memcpy(ss, s, n);
    ss[n] = 0;
    if (strlen(s) > n && n > 3)
    {
        ss[n-1] = ss[n-2] = ss[n-3] = '.';
    }
    return ss;
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

void sigchld_handler(int signo)
{
    pid_t pid;
    pid = waitpid(-1, NULL, 0);
    if (pid < 0)
        return;
    int i;
    for (i = 0; i < process_count; i++)
    {
        if (processes[i]->pid == pid)
        {
            processes[i]->status = DEAD;
            time_t curr_time = time(NULL);
            memcpy(processes[i]->end, localtime(&curr_time), sizeof(struct tm));
            return;
        }
    }
}

void exit_gracefully(int signo)
{
    if (signo == SIGINT)
    {
        outfd = CL_OUT;
        printify("%d received ctrl+c", getpid());
    }
    kill_all();
    free_all_processes();
    shutdown(CL_IN, SHUT_RDWR);
    close(CL_IN);
    close(CL_OUT);
    close(SV_TO_TM_CMD);
    close(SV_TO_TM_RESULT);
    close(TM_TO_SV_CMD);
    close(TM_TO_SV_RESULT);
    exit(signo);
}

void printify(const char* str, ...)
{
    char buff[BUFF_SIZE];

    va_list args;
    va_start(args, str);

    if (write(outfd, buff, vsnprintf(buff, BUFF_SIZE, str, args)) == -1)
    {
        perrorize("TM: printify: write", errno);
        if (errno == EFAULT)
        {
            exit_gracefully(0);
        }
    }

    va_end(args);
}

void fprintify(int fd, const char* str, ...)
{
    char buff[BUFF_SIZE];

    va_list args;
    va_start(args, str);

    if (write(fd, buff, vsnprintf(buff, BUFF_SIZE, str, args)) == -1)
    {
        perrorize("TM: printify: write", errno);
        if (errno == EFAULT)
        {
            exit_gracefully(0);
        }
    }

    va_end(args);
}

void perrorize(char* str, int eno)
{
    fprintify(errfd, "%s: %s\n", str, strerror(eno));
    fsync(outfd);
}