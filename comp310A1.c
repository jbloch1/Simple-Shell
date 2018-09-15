/* Student: Jonathan Bloch 
   Student_ID: 260632216

   This has not been tested really and has some bugs*/

/* By typing into my program command prompt:
	sleep 20 & and then fg 1, it causes the timer, on stopping, to put my shell into a background job in linux and ubuntu, but works properly on bsd.
*/

#include <stdio.h> /* printf getline */
#include <unistd.h> /* execvp */
#include <signal.h> /* signal */
#include <stdlib.h> /* exit malloc */
#include <string.h> /* index strlen ... */
#include <sys/types.h> /* wait */
#include <sys/wait.h> /* wait */
#include <unistd.h> /* sleep tcgetpgrp tcsetpgrp pipe dup */
#include <limits.h> /* UINT_MAX */
#include <sys/stat.h> /* open */
#include <fcntl.h> /* open */

#define COMMAND_MAX_ARGS 20

/* Represents a running process */
struct job
{
	int jobId; /* key in an easy to use form */
	int isDone, isBackground, isFile, isLineAlloc;
	pid_t pid; /* pid of the child */
	char *buffer; /* Contains the value that argv points to; will have to be freed */
	int argc; /* how many argvs are there, max COMMAND_MAX_ARGS */
	char *argv[COMMAND_MAX_ARGS]; /* points to elements in buffer */
	struct job *redirect, *next; /* linked list */
};

/* Reads a command from the keyboard of COMMAND_SIZE or returns 0 */
static char *getCommand(void);
/* parses the string and returns a job */
static struct job *parseJob(char *line);
static struct job *parseJobRecursive(char *line, int lineAlloc);
/* executes the job */
static int dispatchJob(struct job *job);
/* This deletes a job */
static void eraseJob(struct job *job);

/* Interrupt handler for ctrl-c and child */
static void sigHandler(int sig);
static void terminateHandler(int sig);
static void ignoreHandler(int sig);

/* Built in */
static void builtInJobs(void);
static void builtInFg(struct job *job);

/* global pointer */
struct job *jobHead = NULL, *fgJob = NULL;
pid_t shell_pid;
int shell_terminal;

int main(void)
{
	struct job *newJob;
	char *commandString;
	/* setup interrupt ctrl-c, terminate, ignore ctrl-z */
	if(signal(SIGINT, sigHandler) == SIG_ERR
		|| signal(SIGCHLD, terminateHandler) == SIG_ERR
		|| signal(SIGTSTP, ignoreHandler) == SIG_ERR)
	{
		perror("signal"); /* prints a readable message based on errno, which signal sets */
		exit(EXIT_FAILURE);
	}

	/* process group https://www.gnu.org/software/libc/manual/html_node/Initializing-the-Shell.html#Initializing-the-Shell */
	shell_pid = getpid();
	if (setpgid (shell_pid, shell_pid) < 0)
	{
		perror ("process group");
		exit (1);
	}
	tcsetpgrp (shell_terminal, shell_pid);
	fprintf(stderr, "Set shell terminal group id %d.\n", shell_pid);

	/* command loop */
	while(1) 
	{
		/* this gets a command as a string */
		if(!(commandString = getCommand())) break;
		/* parses the job and dispatches, if the return value is true, it means quit */
		if((newJob = parseJob(commandString)) && dispatchJob(newJob)) break;
	}

	 /* on quiting, have to clean up the malloced linked list */
	if(jobHead) {
		printf("Jobs are being terminated:\n");
		builtInJobs();
		while(jobHead) kill(jobHead -> pid, SIGKILL);
		/* kill sends it to terminateHandler and that frees it, but just in case */
		printf("Should be all done:\n");
		builtInJobs();
		while(jobHead) eraseJob(jobHead);
	}
	printf("Bye.\n");
	return EXIT_SUCCESS;
}

/* returns newly allocated string or a NULL pointer */
static char *getCommand(void)
{
	ssize_t length;
	size_t linecap = 0;
	char *line = NULL;
	printf("%s", "\n>>");
	length = getline(&line, &linecap, stdin);
	if (length <= 0) {
		free(line); // have to do this
		perror("getline");
		exit(-1);
	}
	//printf("%s", line);

	return line;
}

/* call recursive function */
static struct job *parseJob(char *line) {
	/* getline returns an allocated line, 1 is for freeing */
	return parseJobRecursive(line, 1);
}

/* lineAlloc, is the line allocated and should be freed? */
static struct job *parseJobRecursive(char *line, int lineAlloc) {
	struct job *newJob;
	char *token, *loc;

	//printf("parseJobR(\"%s\", %d)\n", line, lineAlloc);
	/* Check buffer for NULL */
	if(line == NULL || line[0] == '\0') return 0;

	/* Create a job */
	newJob = malloc(sizeof *newJob);
	if(newJob == NULL) {
		perror("newJob");
		return 0;
	}
	newJob -> jobId = 0;
	newJob -> isBackground = 0;
	newJob -> isDone = 0;
	newJob -> isFile = 0;
	newJob -> isLineAlloc = lineAlloc;
	newJob -> pid = 0; /* pid has not been assigned yet */
	newJob -> buffer = line;
	newJob -> argc = 0;
	newJob -> redirect = NULL;
	newJob -> next = NULL; /* linked list */
	//printf("New job -- %p.\n", (void *)newJob);

	// Check if background is specified..
	if ((loc = strchr(line, '&')) != NULL) {
		newJob -> isBackground = 1;
		*loc = ' ';
		//printf("bg: %s\n", line);
	}
	/* redirection */
	if((loc = strpbrk(line, ">|")) != NULL) {
		/* > is a file, | is a pipe to a program */
		const int isFile = (*loc == '>') ? 1 : 0;
		/* split line into two */
		loc[0] = '\0';
		loc++;
		/* recurse */
		newJob -> redirect = parseJobRecursive(loc, 0);
		if(newJob -> redirect == NULL) {
			printf("No parse redirect.\n");
			eraseJob(newJob);
			return 0;
		}
		newJob -> redirect -> isFile = isFile;
	}

	/* Parse buffer into the newly created job */
	while ((token = strsep(&line, " \t\n")) != NULL) {
		for (int j = 0; j < strlen(token); j++)
			if (token[j] <= 32)
				token[j] = '\0';
		if (strlen(token) > 0 && newJob -> argc < COMMAND_MAX_ARGS - 1)
			newJob -> argv[newJob -> argc ++] = token;
	}
	newJob -> argv[newJob -> argc] = NULL; /* null-terminate for execvp */

	/* check that there's at least one arg */
	if(newJob -> argc < 1) {
		eraseJob(newJob);
		return 0;
	}

	return newJob;
}

/* Returns whether to quit. Fixme: this function might be buggy */
static int dispatchJob(struct job * job)
{
	int pipefd[2], fdDupOut, fd, wstatus;
	pid_t pid;
	struct job *job2 = NULL; /* For redirection. fixme: not recursive */

	/* all background processes have a unique jobId, starting at 1 */
	static int jobIdCounter = 1;

	if(job == NULL) return 0;

	/* Check built-in commands */
	if(strcmp(job -> argv[0], "exit") == 0)
	{
		eraseJob(job);
		return 1; /* quit */
	}
	else if(strcmp(job -> argv[0], "jobs") == 0) {
		printf("jobs:\n");
		builtInJobs();
		eraseJob(job);
		return 0;
	}
	else if(strcmp(job -> argv[0], "cd") == 0)
	{
		if(job -> argc == 2) {
			chdir(job -> argv[1]);
		}
		else
		{
			fprintf(stderr, "This doesn't support spaces.\n");
		}
		eraseJob(job);
		return 0;
	}
	else if(strcmp(job -> argv[0], "pwd") == 0)
	{
		char buffer[1000];
		if(getcwd(buffer, sizeof buffer) != NULL ) {
			printf("%s.\n", buffer);
		}
		else
		{
			perror("pwd");
		}
		eraseJob(job);
		return 0;
	}
	else if(strcmp(job -> argv[0], "fg") == 0)
	{
		int inputJobId;
		if(job -> argc != 2) {
			printf("What?\n");
		}
		else
		{
			inputJobId = atoi(job -> argv[1]);
			if(inputJobId == 0) {
				printf("What?\n");
			}
			else
			{
				struct job *j;
				for(j = jobHead; j != NULL; j = j -> next) {
					if(j -> jobId == inputJobId) break;
				}
				if(j == NULL) {
					fprintf(stderr, "No job with that number.\n");
				}
				else
				{
					builtInFg(j);
				}
			}
		}
		eraseJob(job);
		return 0;
	} 

	/* pipe? http://tldp.org/LDP/lpg/node11.html */
	if(job -> redirect) {
		job2 = job -> redirect;
		/* is > */
		if(job2 -> isFile)
		{
			if(job2 -> argc != 1) {
				printf("Files can not have spaces.\n");
				eraseJob(job);
				return 0;
			}
			/* close stdout and open the file specified with > */
			fdDupOut = dup(STDOUT_FILENO);
			close(STDOUT_FILENO);
			fd = open(job2 -> argv[0], O_RDWR | O_CREAT | O_TRUNC, S_IRWXU);
		}
		/* is | */
		else
		{
			/* open two files, input/output */
			if(pipe(pipefd) == -1) {
				perror("pipe");
				eraseJob(job);
				return 0;
			}

		}
	}

	pid = fork();
	/* this is the child process, execvp will replace the process,
         eg, "ls | wc" would have job = "ls" and job -> redirect = "wc" */

	if(pid == 0)
	{
		printf("(Child process.)\n");
		/* pipe http://stackoverflow.com/questions/13801175/classic-c-using-pipes-in-execvp-function-stdin-and-stdout-redirection */
		if(job -> redirect && !job2 -> isFile) {
			close(pipefd[0]);
			dup2(pipefd[1], STDOUT_FILENO);
			close(pipefd[0]);
			/* eg, "wc" */
			execvp(job -> argv[0], job -> argv);
			exit(0);
		}
		waitpid(pid, &wstatus, 0);
		execvp(job -> argv[0], job -> argv);
	}
	/* this is the parent */
	job -> pid = pid;
	//printf("%d %s\n", pid, job -> argv[0]);
	/* redirection (fixme: not recursive) ignores & */
	if(job -> redirect)
	{
		/* | */
		if(!job2 -> isFile)
		{
			struct job *job2 = job -> redirect;
			pid_t pid2 = fork();

			/* this is the child http://stackoverflow.com/questions/13801175/classic-c-using-pipes-in-execvp-function-stdin-and-stdout-redirection says it should be this way */
			if(pid2 == 0)
			{
				close(pipefd[1]);
				dup2(pipefd[0],STDIN_FILENO);
            			close(pipefd[1]);
				execvp(job2 -> argv[0], job2 -> argv);
				exit(0);
			}
			close(pipefd[0]);
			close(pipefd[1]);
			wait(0);
			wait(0);
			eraseJob(job);
			return 0;
		}
		/* > */
		else
		{
			waitpid(pid, &wstatus, 0);
			close(fd);
			dup2(fdDupOut, STDOUT_FILENO);
			close(fdDupOut);
			eraseJob(job);
			return 0;
		}
	}

	/* fg */
	if(job -> isBackground == 0) {
		/* wait for it to complete */
		int wstatus;
		printf("Parent: exec imm\n");
		fgJob = job;
		/* fixme:*/
		//tcsetpgrp (shell_terminal, newJob -> pid);
		/* wait until it's done (fixme: kind of) */
		waitpid(job -> pid, &wstatus, 0);
		/* reset */
		if(job) fgJob = NULL;
		eraseJob(job);
		/*tcsetpgrp (shell_terminal, shell_pid);*/
		//printf("Parent: imm exit %d.\n", wstatus);
	}

	/* bg */
	else
	{
		/* add it to the linked list */
		//printf("add a job\n");
		job -> jobId = jobIdCounter++;
		job -> next = jobHead;
		jobHead = job;
	}

	return 0;
}

/* free the job */
static void eraseJob(struct job *job) {
	if(job == NULL) return;
	//printf("Erase job %d (pid %d) %s -- %p.\n", job -> jobId, job -> pid, job -> argv[0], (void *)job);
	if(job -> redirect) eraseJob(job -> redirect);
	if(job -> isLineAlloc && job -> buffer) free(job -> buffer);
	free(job);
}

/* ctrl-c handler */
static void sigHandler(int sig)
{
	struct job *prev, *this;
	//printf("Hey! Caught signal %d\n",sig);
	/* if no one's running in the foreground, ignore */
	/* kill fgJob */
	kill(fgJob -> pid, SIGKILL);
}

/* termination handler */
static void terminateHandler(int sig) {
	struct job *prev, *this;
	int wstatus = 0, pid;

	//printf("terminate? %d\n", sig);
	pid = waitpid(-1, &wstatus, 0/*WNOHANG doesn't work*/);
	//printf("pid %d %s\n", pid, WIFEXITED(wstatus) ? "exited" : "notExited");
	if(pid == 0) return;
	/* if not terminated */
	if(sig != SIGCHLD) {
		printf("Terminate pid %d\n", pid);
		kill(pid, SIGKILL);
	}
	/* linear search for pid */
	for(prev = NULL, this = jobHead; this != NULL; prev = this, this = this -> next) {
		if(this -> pid == pid) break;
	}
	/* not in the child list */
	if(this == NULL) return;
	/* remove the process from the list */
	if(prev == NULL) {
		jobHead = this -> next;
	}
	else
	{
		prev -> next = this -> next;
	}
	/* if it's fgJob, set fgJob to NULL */
	if(this == fgJob) fgJob = NULL;
	/* erase this */
	eraseJob(this);
}

static void ignoreHandler(int sig) {
	printf("Ignoring the %d signal", sig);
}

/* print jobs */
static void builtInJobs(void)
{
	struct job * j;
	j = jobHead;
	while(j != NULL) 
	{
		printf("%d\t%s\n", j -> jobId, j -> argv[0]);
		j = j->next;
	}
}

static void builtInFg(struct job *job) {
	int wstatus;
	if(fgJob != NULL) {
		printf("Fg job while Fg job is running.\n");
		return;
	}
	if(job -> pid == 0) {
		printf("Fg job %d %s has an uninitialized PID.\n", job -> jobId, job -> argv[0]);
		return;
	}
	/* remove from bg */
	if(job == jobHead) {
		jobHead = job -> next;
	}
	else
	{
		struct job *last;
		for(last = jobHead; last != NULL && last -> next != job; last = last -> next);
		if(last == NULL) {
			printf("Fg job not in jobs.\n");
			return;
		}
		last -> next = job -> next;
	}
	fgJob = job;
	/* bring the keyboard from the command prompt to the program
         https://www.chemie.fu-berlin.de/chemnet/use/info/libc/libc_24.html */
	tcsetpgrp (shell_terminal, job -> pid);
	/* wait for it to exit */
	waitpid(job -> pid, &wstatus, 0);
	fgJob = NULL;
	/* bring the keyboard the the command prompt again */
	tcsetpgrp (shell_terminal, shell_pid);
	eraseJob(job);
}
