/* 
 * Copyright 2010
 *
 * tsh - A tiny shell program with job control
 *
 * cmcdonough@wustl.edu, cam4 (Colin McDonough)
 * zwdumey@wustl.edu, zwd1 (Zachariah Dumey)
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>

/* Misc manifest constants */
#define MAXLINE    1024   /* max line size */
#define MAXARGS     128   /* max args on a command line */
#define MAXJOBS      16   /* max jobs at any point in time */
#define MAXJID    1<<16   /* max job ID */

/* Job states */
#define UNDEF 0 /* undefined */
#define FG 1    /* running in foreground */
#define BG 2    /* running in background */
#define ST 3    /* stopped */

/* 
 * Jobs states: FG (foreground), BG (background), ST (stopped)
 * Job state transitions and enabling actions:
 *     FG -> ST  : ctrl-z
 *     ST -> FG  : fg command
 *     ST -> BG  : bg command
 *     BG -> FG  : fg command
 * At most 1 job can be in the FG state.
 */

/* Global variables */
extern char **environ;      /* defined in libc */
char prompt[] = "tsh> ";    /* command line prompt (DO NOT CHANGE) */
int verbose = 0;            /* if true, print additional output */
int nextjid = 1;            /* next job ID to allocate */
char sbuf[MAXLINE];         /* for composing sprintf messages */

struct job_t {              /* The job struct */
    pid_t pid;              /* job PID */
    int jid;                /* job ID [1, 2, ...] */
    int state;              /* UNDEF, BG, FG, or ST */
    char cmdline[MAXLINE];  /* command line */
};
struct job_t jobs[MAXJOBS]; /* The job list */
/* End global variables */


/* Function prototypes */

/* Our own functions */
pid_t Setpgid(pid_t a, pid_t b);
pid_t Fork(void);
int Sigprocmask(int how, const sigset_t *set, sigset_t *oldset);
int Sigemptyset(sigset_t *set);
int Sigaddset(sigset_t *set, int signum);
int Kill(pid_t p, int signum);
/* Here are the functions that you will implement */
void eval(char *cmdline);
int builtin_cmd(char **argv);
void do_bgfg(char **argv);
void waitfg(pid_t pid);

void sigchld_handler(int sig);
void sigtstp_handler(int sig);
void sigint_handler(int sig);

/* Here are helper routines that we've provided for you */
int parseline(const char *cmdline, char **argv); 
void sigquit_handler(int sig);

void clearjob(struct job_t *job);
void initjobs(struct job_t *jobs);
int maxjid(struct job_t *jobs); 
int addjob(struct job_t *jobs, pid_t pid, int state, char *cmdline);
int deletejob(struct job_t *jobs, pid_t pid); 
pid_t fgpid(struct job_t *jobs);
struct job_t *getjobpid(struct job_t *jobs, pid_t pid);
struct job_t *getjobjid(struct job_t *jobs, int jid); 
int pid2jid(pid_t pid); 
void listjobs(struct job_t *jobs);

void usage(void);
void unix_error(char *msg);
void app_error(char *msg);
typedef void handler_t(int);
handler_t *Signal(int signum, handler_t *handler);

/*
 * main - The shell's main routine 
 */
int main(int argc, char **argv) 
{
    char c;
    char cmdline[MAXLINE];
    int emit_prompt = 1; /* emit prompt (default) */

    /* Redirect stderr to stdout (so that driver will get all output
     * on the pipe connected to stdout) */
    dup2(1, 2);

    /* Parse the command line */
    while ((c = getopt(argc, argv, "hvp")) != EOF) {
        switch (c) {
        case 'h':             /* print help message */
            usage();
	    break;
        case 'v':             /* emit additional diagnostic info */
            verbose = 1;
	    break;
        case 'p':             /* don't print a prompt */
            emit_prompt = 0;  /* handy for automatic testing */
	    break;
	default:
            usage();
	}
    }

    /* Install the signal handlers */

    /* These are the ones you will need to implement */
    Signal(SIGINT,  sigint_handler);   /* ctrl-c */
    Signal(SIGTSTP,   sigtstp_handler);  /* ctrl-z */
    Signal(SIGCHLD, sigchld_handler);  /* Terminated or stopped child */

    /* This one provides a clean way to kill the shell */
    Signal(SIGQUIT, sigquit_handler); 

    /* Initialize the job list */
    initjobs(jobs);

    /* Execute the shell's read/eval loop */
    while (1) {

	/* Read command line */
	if (emit_prompt) {
	    printf("%s", prompt);
	    fflush(stdout);
	}
	if ((fgets(cmdline, MAXLINE, stdin) == NULL) && ferror(stdin))
	    app_error("fgets error");
	if (feof(stdin)) { /* End of file (ctrl-d) */
	    fflush(stdout);
	    exit(0);
	}

	/* Evaluate the command line */
	eval(cmdline);
	fflush(stdout);
	fflush(stdout);
    } 

    exit(0); /* control never reaches here */
}
  
/* 
 * eval - Evaluate the command line that the user has just typed in
 * 
 * If the user has requested a built-in command (quit, jobs, bg or fg)
 * then execute it immediately. Otherwise, fork a child process and
 * run the job in the context of the child. If the job is running in
 * the foreground, wait for it to terminate and then return.  Note:
 * each child process must have a unique process group ID so that our
 * background children don't receive SIGINT (SIGTSTP) from the kernel
 * when we type ctrl-c (ctrl-z) at the keyboard.  
*/
void eval(char *cmdline) 
{
    char *argv[MAXARGS]; /* Argument list execve() */
    char buf[MAXLINE];   /* Holds modified command line */
    int bg;              /* Should the job run in bg or fg? */
    pid_t pid;           /* Process id */
    sigset_t mask;

    strcpy(buf, cmdline);
    bg = parseline(buf, argv);
    if (argv[0] == NULL) {
      return; /* Ignore empty lines */
    }

    if (!builtin_cmd(argv)) {
        /* Block SIGCHLD, SIGINT,	 
	and SIGSTP until the job to be run is on the list */
        Sigemptyset(&mask);
        Sigaddset(&mask, SIGCHLD);
        Sigaddset(&mask, SIGINT);
        Sigaddset(&mask, SIGTSTP);
        Sigprocmask(SIG_BLOCK, &mask, NULL); 

        if ((pid = Fork()) == 0) { /* Child runs user job */
            Setpgid(0, 0); // give the child a new group
            /* unblock signals for child */
            Sigprocmask(SIG_UNBLOCK, &mask, NULL); 
            if (execve(argv[0], argv, environ) < 0) {
                printf("%s: Command not found.\n", argv[0]);
                exit(0);
            }
        }

        addjob(jobs, pid, bg?BG:FG, cmdline);
        if (bg) {
            // Notify user that job is running in the background
            printf("[%i] (%i) %s", getjobpid(jobs, pid)->jid, pid, cmdline);
        }
	Sigprocmask(SIG_UNBLOCK, &mask, NULL); /* Unblock signals */

        if (!bg) {
            /* Parent waits for foreground job to terminate */
            waitfg(pid);
        }
    }

    return;
}

/*
 * Robust system call wrapper
 */
pid_t Setpgid(pid_t a, pid_t b) {
	pid_t pid;
	if ((pid = setpgid(a, b)) < 0) {
		unix_error("setpgid error");
	}
	return pid;
}	

/*
 * Robust system call wrapper
 */
pid_t Fork(void) {
    pid_t pid;

    if ((pid = fork()) < 0) {
        unix_error("Fork error");
    }
    return pid;
}

/*
 * Robust system call wrapper
 */
int Sigemptyset(sigset_t *set) {
    int value;

    if ((value = sigemptyset(set)) < 0) {
        unix_error("Sigemtpyset error");
    }
    return value;
}

/*
 * Robust system call wrapper
 */
int Sigaddset(sigset_t *set, int signum) {
    int value;

    if ((value = sigaddset(set, signum)) < 0) {
        unix_error("Sigaddset error");
    }
    return value;
}

/*
 * Robust system call wrapper
 */
int Sigprocmask(int how, const sigset_t *set, sigset_t *oldset) {
    int value;

    if ((value = sigprocmask(how, set, oldset)) < 0) {
        unix_error("Sigprocmask error");
    }
    return value;
}

/*
 * Robust system call wrapper
 */
pid_t Kill(pid_t p, int signum) {
	pid_t pid;	
	if ((pid = kill(p, signum)) < 0) {
		unix_error("kill error");
	}
	return pid;
}

/* 
 * parseline - Parse the command line and build the argv array.
 * 
 * Characters enclosed in single quotes are treated as a single
 * argument.  Return true if the user has requested a BG job, false if
 * the user has requested a FG job.  
 */
int parseline(const char *cmdline, char **argv) 
{
    static char array[MAXLINE]; /* holds local copy of command line */
    char *buf = array;          /* ptr that traverses command line */
    char *delim;                /* points to first space delimiter */
    int argc;                   /* number of args */
    int bg;                     /* background job? */

    strcpy(buf, cmdline);
    buf[strlen(buf)-1] = ' ';  /* replace trailing '\n' with space */
    while (*buf && (*buf == ' ')) /* ignore leading spaces */
	buf++;

    /* Build the argv list */
    argc = 0;
    if (*buf == '\'') {
	buf++;
	delim = strchr(buf, '\'');
    }
    else {
	delim = strchr(buf, ' ');
    }

    while (delim) {
	argv[argc++] = buf;
	*delim = '\0';
	buf = delim + 1;
	while (*buf && (*buf == ' ')) /* ignore spaces */
	       buf++;

	if (*buf == '\'') {
	    buf++;
	    delim = strchr(buf, '\'');
	}
	else {
	    delim = strchr(buf, ' ');
	}
    }
    argv[argc] = NULL;
    
    if (argc == 0)  /* ignore blank line */
	return 1;

    /* should the job run in the background? */
    if ((bg = (*argv[argc-1] == '&')) != 0) {
        /*(printf("Job in background: %s", cmdline);
        fflush(stdout);*/
	argv[--argc] = NULL;
    }
    return bg;
}

/* 
 * builtin_cmd - If the user has typed a built-in command then execute
 *    it immediately.  
 */
int builtin_cmd(char **argv) 
{
    if (!strcmp(argv[0], "quit")) { /* quit command */
        exit(0);
    }
    if (!strcmp(argv[0], "jobs")) { /* jobs command */
        listjobs(jobs);
        return 1;
    }
    if (!strcmp(argv[0], "bg")) { /* bg command */
        do_bgfg(argv);
        return 1;
    }
    if (!strcmp(argv[0], "fg")) { /* fg command */
    	do_bgfg(argv); // sending it off to the do_bgfg method
	return 1;
    }
    if (!strcmp(argv[0], "&")) { /* Ignore singleton & */
        return 1;
    }
    
    return 0;     /* not a builtin command */
}

/* 
 * do_bgfg - Execute the builtin bg and fg commands
 */
void do_bgfg(char **argv) 
{

    struct job_t * job;
    job = NULL;

    if (argv[1] == NULL) {
         // Missing arguments
         printf("%s command requires PID or %%jobid argument\n", argv[0]);
         fflush(stdout);
         return;
    }

    if (isdigit(*argv[1])) {
        // Check if the second argument is a digit
        job = getjobpid(jobs, atoi(argv[1]));
        if (!job) {
            printf("(%s): No such process\n", argv[1]);
            fflush(stdout);
            return;
        }
    } else if (strlen(argv[1]) > 1 && argv[1][0] == '%') {
        // Check if the second argument is %number (e.g. %5)
        if (isdigit(argv[1][1])) {
            // Make sure that the second character is a digit
            job = getjobjid(jobs, atoi(argv[1]+1));
            if (!job) {
                printf("%s: No such job\n", argv[1]);
                fflush(stdout);
                return;
            }
        }
    }

    if (job == NULL) {
        // If the job is null, report invalid argument and stop
        printf("%s: argument must be a PID or %%jobid\n", argv[0]);
        fflush(stdout);
        return;
    }

    if (!strcmp(argv[0], "fg")) {
        if (Kill(-job->pid, SIGCONT) == 0) {
            job->state = FG;
            // since sigcont will just run the program in the background, 
            // we need to explicitly tell the shell
            // to wait for it to complete
            waitfg(job->pid);
        }
    } else if (!strcmp(argv[0], "bg")) {
        // Restart job but do not wait for the job (job is in background)
        printf("[%i] (%i) %s", job->jid, job->pid, job->cmdline);	
        fflush(stdout);
        if (Kill(-job->pid, SIGCONT) == 0) {
            job->state = BG;
        }
    }
    fflush(stdout);
    return;
}

/* 
 * waitfg - Block until process pid is no longer the foreground process
 */
void waitfg(pid_t pid)
{
    
    struct job_t *job = getjobpid(jobs, pid);
    /* fg done and already reaped by handler */
    if (!job) {
        return;
    }
    /* Wait for a fg */
    while (job->pid == pid && job->state == FG) {
        sleep(1);
    }
    return;
}

/*****************
 * Signal handlers
 *****************/

/* 
 * sigchld_handler - The kernel sends a SIGCHLD to the shell whenever
 *     a child job terminates (becomes a zombie), or stops because it
 *     received a SIGSTOP or SIGTSTP signal. The handler reaps all
 *     available zombie children, but doesn't wait for any other
 *     currently running children to terminate.  
 */
void sigchld_handler(int sig) 
{
    pid_t pid;
    struct job_t *job;  
    int status;

    // Handle all four cases: job finished normally, job was signalled by an
    // external process, job was slept by an external process, job terminated
    // abnormally
    while ((pid = waitpid(-1, &status, WNOHANG|WUNTRACED)) > 0) {
        if (WIFEXITED(status)) {
            deletejob(jobs, pid);
        } else if (WIFSIGNALED(status)) {
            job = getjobpid(jobs, pid);
            printf("Job [%i] (%i) terminated by signal %i\n",
                    job->jid, job->pid, WTERMSIG(status));
            fflush(stdout);
            deletejob(jobs, pid);
        } else if (WIFSTOPPED(status)) {
            job = getjobpid(jobs, pid);
            job->state = ST;
        } else {
            printf("Process (%i) terminated abnormally\n", pid);
            fflush(stdout);
            deletejob(jobs, pid);
        }
    }

    // after the loop, it should be the case that the pid returned
    // by waitpid is -1 AND errno is ECHILD 
    // or that the pid is 0
    if ((errno != ECHILD && pid == -1) || pid > 0) {
        unix_error("waitpid error");
    }
    return;
}

/* 
 * sigint_handler - The kernel sends a SIGINT to the shell whenver the
 *    user types ctrl-c at the keyboard.  Catch it and send it along
 *    to the foreground job.  
 */
void sigint_handler(int sig) 
{
    struct job_t *job = getjobpid(jobs, fgpid(jobs));
    if (job != NULL) {
        // Only delete jobs that are on the jobs list
        Kill(-job->pid, SIGINT);
        printf("Job [%i] (%i) terminated by signal %i\n",
                job->jid, job->pid, sig);
        fflush(stdout);
        deletejob(jobs, job->pid);
    }
}

/*
 * sigtstp_handler - The kernel sends a SIGTSTP to the shell whenever
 *     the user types ctrl-z at the keyboard. Catch it and suspend the
 *     foreground job by sending it a SIGTSTP.  
 */
void sigtstp_handler(int sig) 
{
    struct job_t *job = getjobpid(jobs, fgpid(jobs));
    if (job != NULL) {
        // Only sleep jobs that are on the jobs list
        Kill(-job->pid, SIGTSTP);
        job->state = ST;
        printf("Job [%i] (%i) stopped by signal %i\n", job->jid, job->pid, sig);
        fflush(stdout);
        return;
    }
}

/*********************
 * End signal handlers
 *********************/

/***********************************************
 * Helper routines that manipulate the job list
 **********************************************/

/* clearjob - Clear the entries in a job struct */
void clearjob(struct job_t *job) {
    job->pid = 0;
    job->jid = 0;
    job->state = UNDEF;
    job->cmdline[0] = '\0';
}

/* initjobs - Initialize the job list */
void initjobs(struct job_t *jobs) {
    int i;

    for (i = 0; i < MAXJOBS; i++)
	clearjob(&jobs[i]);
}

/* maxjid - Returns largest allocated job ID */
int maxjid(struct job_t *jobs) 
{
    int i, max=0;

    for (i = 0; i < MAXJOBS; i++)
	if (jobs[i].jid > max)
	    max = jobs[i].jid;
    return max;
}

/* addjob - Add a job to the job list */
int addjob(struct job_t *jobs, pid_t pid, int state, char *cmdline) 
{
    /*printf("Adding job %i  with bg = %i\n", pid, state);
    fflush(stdout);*/
    int i;
    
    if (pid < 1)
	return 0;

    for (i = 0; i < MAXJOBS; i++) {
	if (jobs[i].pid == 0) {
	    jobs[i].pid = pid;
	    jobs[i].state = state;
	    jobs[i].jid = nextjid++;
	    if (nextjid > MAXJOBS)
		nextjid = 1;
	    strcpy(jobs[i].cmdline, cmdline);
  	    if(verbose){
	        printf("Added job [%d] %d %s\n", jobs[i].jid, jobs[i].pid, jobs[i].cmdline);
            }
            return 1;
	}
    }
    printf("Tried to create too many jobs\n");
    return 0;
}

/* deletejob - Delete a job whose PID=pid from the job list */
int deletejob(struct job_t *jobs, pid_t pid) 
{
    int i;

    if (pid < 1)
	return 0;

    for (i = 0; i < MAXJOBS; i++) {
	if (jobs[i].pid == pid) {
	    clearjob(&jobs[i]);
	    nextjid = maxjid(jobs)+1;
	    return 1;
	}
    }
    return 0;
}

/* fgpid - Return PID of current foreground job, 0 if no such job */
pid_t fgpid(struct job_t *jobs) {
    int i;

    for (i = 0; i < MAXJOBS; i++)
	if (jobs[i].state == FG)
	    return jobs[i].pid;
    return 0;
}

/* getjobpid  - Find a job (by PID) on the job list */
struct job_t *getjobpid(struct job_t *jobs, pid_t pid) {
    int i;

    if (pid < 1)
	return NULL;
    for (i = 0; i < MAXJOBS; i++)
	if (jobs[i].pid == pid)
	    return &jobs[i];
    return NULL;
}

/* getjobjid  - Find a job (by JID) on the job list */
struct job_t *getjobjid(struct job_t *jobs, int jid) 
{
    int i;

    if (jid < 1)
	return NULL;
    for (i = 0; i < MAXJOBS; i++)
	if (jobs[i].jid == jid)
	    return &jobs[i];
    return NULL;
}

/* pid2jid - Map process ID to job ID */
int pid2jid(pid_t pid) 
{
    int i;

    if (pid < 1)
	return 0;
    for (i = 0; i < MAXJOBS; i++)
	if (jobs[i].pid == pid) {
            return jobs[i].jid;
        }
    return 0;
}

/* listjobs - Print the job list */
void listjobs(struct job_t *jobs) 
{
    int i;
    
    for (i = 0; i < MAXJOBS; i++) {
	if (jobs[i].pid != 0) {
	    printf("[%d] (%d) ", jobs[i].jid, jobs[i].pid);
	    switch (jobs[i].state) {
		case BG: 
		    printf("Running ");
		    break;
		case FG: 
		    printf("Foreground ");
		    break;
		case ST: 
		    printf("Stopped ");
		    break;
	    default:
		    printf("listjobs: Internal error: job[%d].state=%d ", 
			   i, jobs[i].state);
	    }
	    printf("%s", jobs[i].cmdline);
	}
    }
}
/******************************
 * end job list helper routines
 ******************************/


/***********************
 * Other helper routines
 ***********************/

/*
 * usage - print a help message
 */
void usage(void) 
{
    printf("Usage: shell [-hvp]\n");
    printf("   -h   print this message\n");
    printf("   -v   print additional diagnostic information\n");
    printf("   -p   do not emit a command prompt\n");
    exit(1);
}

/*
 * unix_error - unix-style error routine
 */
void unix_error(char *msg)
{
    fprintf(stdout, "%s: %s\n", msg, strerror(errno));
    exit(1);
}

/*
 * app_error - application-style error routine
 */
void app_error(char *msg)
{
    fprintf(stdout, "%s\n", msg);
    exit(1);
}

/*
 * Signal - wrapper for the sigaction function
 */
handler_t *Signal(int signum, handler_t *handler) 
{
    struct sigaction action, old_action;

    action.sa_handler = handler;  
    sigemptyset(&action.sa_mask); /* block sigs of type being handled */
    action.sa_flags = SA_RESTART; /* restart syscalls if possible */

    if (sigaction(signum, &action, &old_action) < 0)
	unix_error("Signal error");
    return (old_action.sa_handler);
}

/*
 * sigquit_handler - The driver program can gracefully terminate the
 *    child shell by sending it a SIGQUIT signal.
 */
void sigquit_handler(int sig) 
{
    printf("Terminating after receipt of SIGQUIT signal\n");
    exit(1);
}



