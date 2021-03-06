/*
 * COMP 321 Project 4: Shell
 *
 * This program implements a tiny shell with job control.
 *
 * Kyran Adams, kpa1
 * Alex Bluestein, arb19
 */

#include <sys/types.h>
#include <sys/wait.h>

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// You may assume that these constants are large enough.
#define MAXLINE      1024   // max line size
#define MAXARGS       128   // max args on a command line
#define MAXJOBS        16   // max jobs at any point in time
#define MAXJID   (1 << 16)  // max job ID

#define PATH_MAX 4096 // Defined in linux/limits.h

// The job states are:
#define UNDEF 0 // undefined
#define FG 1    // running in foreground
#define BG 2    // running in background
#define ST 3    // stopped

/*
 * The job state transitions and enabling actions are:
 *     FG -> ST  : ctrl-z
 *     ST -> FG  : fg command
 *     ST -> BG  : bg command
 *     BG -> FG  : fg command
 * At most one job can be in the FG state.
 */

struct Job {
	pid_t pid;              // job PID
	int jid;                // job ID [1, 2, ...]
	int state;              // UNDEF, FG, BG, or ST
	char cmdline[MAXLINE];  // command line
};
typedef volatile struct Job *JobP;

/*
 * Define the jobs list using the "volatile" qualifier because it is accessed
 * by a signal handler (as well as the main program).
 */
static volatile struct Job jobs[MAXJOBS];
static int nextjid = 1;            // next job ID to allocate

extern char **environ;             // defined by libc

static char prompt[] = "tsh> ";    // command line prompt (DO NOT CHANGE)
static bool verbose = false;       // If true, print additional output.

 // An array that contains all of the paths in the PATH variable
static char **search_path;

/*
 * The following array can be used to map a signal number to its name.
 * This mapping is valid for x86(-64)/Linux systems, such as CLEAR.
 * The mapping for other versions of Unix, such as FreeBSD, Mac OS X, or
 * Solaris, differ!
 */
static const char *const signame[NSIG] = {
	"Signal 0",
	"HUP",				/* SIGHUP */
	"INT",				/* SIGINT */
	"QUIT",				/* SIGQUIT */
	"ILL",				/* SIGILL */
	"TRAP",				/* SIGTRAP */
	"ABRT",				/* SIGABRT */
	"BUS",				/* SIGBUS */
	"FPE",				/* SIGFPE */
	"KILL",				/* SIGKILL */
	"USR1",				/* SIGUSR1 */
	"SEGV",				/* SIGSEGV */
	"USR2",				/* SIGUSR2 */
	"PIPE",				/* SIGPIPE */
	"ALRM",				/* SIGALRM */
	"TERM",				/* SIGTERM */
	"STKFLT",			/* SIGSTKFLT */
	"CHLD",				/* SIGCHLD */
	"CONT",				/* SIGCONT */
	"STOP",				/* SIGSTOP */
	"TSTP",				/* SIGTSTP */
	"TTIN",				/* SIGTTIN */
	"TTOU",				/* SIGTTOU */
	"URG",				/* SIGURG */
	"XCPU",				/* SIGXCPU */
	"XFSZ",				/* SIGXFSZ */
	"VTALRM",			/* SIGVTALRM */
	"PROF",				/* SIGPROF */
	"WINCH",			/* SIGWINCH */
	"IO",				/* SIGIO */
	"PWR",				/* SIGPWR */
	"Signal 31"
};

// You must implement the following functions:

static int	builtin_cmd(char **argv);
static void	do_bgfg(char **argv);
static void	eval(const char *cmdline);
static void	initpath(const char *pathstr);
static void	waitfg(pid_t pid);

static void	sigchld_handler(int signum);
static void	sigint_handler(int signum);
static void	sigtstp_handler(int signum);

// We are providing the following functions to you:

static int	parseline(const char *cmdline, char **argv); 

static void	sigquit_handler(int signum);

static int	addjob(JobP jobs, pid_t pid, int state, const char *cmdline);
static void	clearjob(JobP job);
static int	deletejob(JobP jobs, pid_t pid); 
static pid_t	fgpid(JobP jobs);
static JobP	getjobjid(JobP jobs, int jid); 
static JobP	getjobpid(JobP jobs, pid_t pid);
static void	initjobs(JobP jobs);
static void	listjobs(JobP jobs);
static int	maxjid(JobP jobs); 
static int	pid2jid(pid_t pid); 

static void	app_error(const char *msg);
static void	unix_error(const char *msg);
static void	usage(void);

static void	Sio_error(const char s[]);
static ssize_t	Sio_putl(long v);
static ssize_t	Sio_puts(const char s[]);
static void	sio_error(const char s[]);
static void	sio_ltoa(long v, char s[], int b);
static ssize_t	sio_putl(long v);
static ssize_t	sio_puts(const char s[]);
static void	sio_reverse(char s[]);
static size_t	sio_strlen(const char s[]);

/*
 * Requires:
 *   "argc" is the number of strings in the following array.
 *
 *   "**argv" is an array of strings consisting of a name and zero or 
 *   more arguments. The name should either start with a directory
 *   or be the name of an executable file contained in a 
 *   directory in the search path.
 *
 * Effects:
 *   Performs a loop that reads and processes the user input from the
 *   command line, executes the given commands, and prints the output
 *   to stdout.
 */
int
main(int argc, char **argv) 
{
	struct sigaction action;
	int c;
	char cmdline[MAXLINE];
	char *path = NULL;
	bool emit_prompt = true;	// Emit a prompt by default.

	/*
	 * Redirect stderr to stdout (so that driver will get all output
	 * on the pipe connected to stdout).
	 */
	dup2(1, 2);

	// Parse the command line.
	while ((c = getopt(argc, argv, "hvp")) != -1) {
		switch (c) {
		case 'h':             // Print a help message.
			usage();
			break;
		case 'v':             // Emit additional diagnostic info.
			verbose = true;
			break;
		case 'p':             // Don't print a prompt.
			// This is handy for automatic testing.
			emit_prompt = false;
			break;
		default:
			usage();
		}
	}

	/*
	 * Install sigint_handler() as the handler for SIGINT (ctrl-c).  SET
	 * action.sa_mask TO REFLECT THE SYNCHRONIZATION REQUIRED BY YOUR
	 * IMPLEMENTATION OF sigint_handler().
	 */
	action.sa_handler = sigint_handler;
	action.sa_flags = SA_RESTART;
	sigemptyset(&action.sa_mask);
	if (sigaction(SIGINT, &action, NULL) < 0)
		unix_error("sigaction error");

	/*
	 * Install sigtstp_handler() as the handler for SIGTSTP (ctrl-z).  SET
	 * action.sa_mask TO REFLECT THE SYNCHRONIZATION REQUIRED BY YOUR
	 * IMPLEMENTATION OF sigtstp_handler().
	 */
	action.sa_handler = sigtstp_handler;
	action.sa_flags = SA_RESTART;
	sigemptyset(&action.sa_mask);
	if (sigaction(SIGTSTP, &action, NULL) < 0)
		unix_error("sigaction error");

	/*
	 * Install sigchld_handler() as the handler for SIGCHLD (terminated or
	 * stopped child).  SET action.sa_mask TO REFLECT THE SYNCHRONIZATION
	 * REQUIRED BY YOUR IMPLEMENTATION OF sigchld_handler().
	 */
	action.sa_handler = sigchld_handler;
	action.sa_flags = SA_RESTART;
	sigemptyset(&action.sa_mask);
	if (sigaction(SIGCHLD, &action, NULL) < 0)
		unix_error("sigaction error");

	/*
	 * Install sigquit_handler() as the handler for SIGQUIT.  This handler
	 * provides a clean way for the test harness to terminate the shell.
	 * Preemption of the processor by the other signal handlers during
	 * sigquit_handler() does no harm, so action.sa_mask is set to empty.
	 */
	action.sa_handler = sigquit_handler;
	action.sa_flags = SA_RESTART;
	sigemptyset(&action.sa_mask);
	if (sigaction(SIGQUIT, &action, NULL) < 0)
		unix_error("sigaction error");

	// Initialize the search path.
	path = getenv("PATH");
	initpath(path);

	// Initialize the jobs list.
	initjobs(jobs);

	// Execute the shell's read/eval loop.
	while (true) {

		// Read the command line.
		if (emit_prompt) {
			printf("%s", prompt);
			fflush(stdout);
		}
		if ((fgets(cmdline, MAXLINE, stdin) == NULL) && ferror(stdin))
			app_error("fgets error");
		if (feof(stdin)) { // End of file (ctrl-d)
			fflush(stdout);
			exit(0);
		}

		// Evaluate the command line.
		eval(cmdline);
		fflush(stdout);
		fflush(stdout);
	}

	// Control never reaches here.
	assert(false);
}
  
/* 
 * eval - Evaluate the command line that the user has just typed in.
 * 
 * If the user has requested a built-in command (quit, jobs, bg or fg)
 * then execute it immediately.  Otherwise, fork a child process and
 * run the job in the context of the child.  If the job is running in
 * the foreground, wait for it to terminate and then return.  Note:
 * each child process must have a unique process group ID so that our
 * background children don't receive SIGINT (SIGTSTP) from the kernel
 * when we type ctrl-c (ctrl-z) at the keyboard.  
 *
 * Requires:
 *  "*cmdline" is a string consisting of a name and zero or more
 *  arguments that are separated by one or more spaces. The name 
 *  should either be a built-in command or the name of an 
 *  executable file.
 *
 * Effects:
 *   If "*cmdline" is a built-in command then eval executes the 
 *   built-in command. Otherwise, eval finds the entire name of 
 *   the executable and executes it using execve. If the executable
 *   is run in the foreground, then eval waits for the execution to
 *   finish before terminating.
 */
static void
eval(const char *cmdline) 
{
	// Parse the string from the shell into argument values
	char **argv;
	if ((argv = malloc(sizeof(char*) * MAXARGS)) == NULL) {
		Sio_error("Failed allocating memory");
	}
	int bg = parseline(cmdline, argv);
	
	if (argv[0] == NULL) {
		return;
	}
	// If builtin command, evaluate it
	if (builtin_cmd(argv)) {
		return;
	}
	int is_exe_in_cwd = 0;
	if (access(argv[0], X_OK) == 0) {
		is_exe_in_cwd = 1;
		int i = 0;
		while (argv[0][i] != '\0') {
			if (argv[0][i] == '/') {
				is_exe_in_cwd = 0;
			}
			i++;
		}
	}
	
	char *executable = NULL;
	// Otherwise we have a executable path or name
	if (argv[0][0] == '/' || argv[0][0] == '.' || search_path == NULL) { 
		executable = argv[0];
		// We have a full path to executable
	} else if (!is_exe_in_cwd) {
		int i = 0;
		// search through path for valid path to executable
		while(search_path[i] != NULL) {
			char *abs_path;
			if ((abs_path = malloc(strlen(search_path[i]) + 1 
					       + strlen(argv[0]))) == NULL) {
				    Sio_error("Failed allocating memory");
			}
			// concat executable to path
			strcpy(abs_path, search_path[i]);
			strcat(abs_path, "/");
			strcat(abs_path, argv[0]);

			if (access(abs_path, X_OK) == 0) {
				executable = abs_path;
				break;
			}
			i++;
		} 
	}

	if (executable == NULL || access(executable, X_OK) != 0) {
		printf("%s: Command not found\n", argv[0]);
		return;
	}
        
	sigset_t mask, prev_mask;
	sigemptyset(&mask);
	sigaddset(&mask, SIGCHLD);
	sigprocmask(SIG_BLOCK, &mask, &prev_mask);

	pid_t pid = fork();
	if (pid == 0) {
		// Child task

		// Put child into new process group, so only shell is in 
		// FG process group
		setpgid(0, 0);
		// Unblock blocking of child signal before we execute
		sigprocmask(SIG_SETMASK, &prev_mask,  NULL);

		if (execve(executable, argv, environ) < 0) {
			printf("%s: Command not found\n", argv[0]);
			exit(0);
		}
	} else if (pid < 0) {
		// TASK CREATION FAILED
		printf("Task creation failed.\n");
	}
	addjob(jobs, pid, bg ? BG : FG, cmdline);
	JobP job = getjobpid(jobs, pid);
	if (bg) { 
		printf("[%d] (%d) %s", job->jid, job->pid, job->cmdline);
	}

       	sigprocmask(SIG_SETMASK, &prev_mask,  NULL);
	// If it's a foreground task, 
	// wait for it to finish before continuing REPL
	if (!bg) {
		waitfg(pid);
	}
}

/* 
 * parseline - Parse the command line and build the argv array.
 *
 * Requires:
 *   "cmdline" is a NUL ('\0') terminated string with a trailing
 *   '\n' character.  "cmdline" must contain less than MAXARGS
 *   arguments.
 *
 * Effects:
 *   Builds "argv" array from space delimited arguments on the command line.
 *   The final element of "argv" is set to NULL.  Characters enclosed in
 *   single quotes are treated as a single argument.  Returns true if
 *   the user has requested a BG job and false if the user has requested
 *   a FG job.
 */
static int
parseline(const char *cmdline, char **argv) 
{
	int argc;                   // number of args
	int bg;                     // background job?
	static char array[MAXLINE]; // local copy of command line
	char *buf = array;          // ptr that traverses command line
	char *delim;                // points to first space delimiter

	strcpy(buf, cmdline);

	// Replace trailing '\n' with space.
	buf[strlen(buf) - 1] = ' ';

	// Ignore leading spaces.
	while (*buf != '\0' && *buf == ' ')
		buf++;

	// Build the argv list.
	argc = 0;
	if (*buf == '\'') {
		buf++;
		delim = strchr(buf, '\'');
	} else
		delim = strchr(buf, ' ');
	while (delim != NULL) {
		argv[argc++] = buf;
		*delim = '\0';
		buf = delim + 1;
		while (*buf != '\0' && *buf == ' ')	// Ignore spaces.
			buf++;
		if (*buf == '\'') {
			buf++;
			delim = strchr(buf, '\'');
		} else
			delim = strchr(buf, ' ');
	}
	argv[argc] = NULL;

	// Ignore blank line.
	if (argc == 0)
		return (1);

	// Should the job run in the background?
	if ((bg = (*argv[argc - 1] == '&')) != 0)
		argv[--argc] = NULL;

	return (bg);
}

/* 
 * builtin_cmd - If the user has typed a built-in command then execute
 *  it immediately.  
 *
 * Requires:
 *   A string array argv with a non-null first element.
 *
 * Effects:
 *   If the first word of argv is a builtin command, executes it
 *   and returns 1. Otherwise returns 0.
 */
static int
builtin_cmd(char **argv) 
{

	if (!strcmp(argv[0], "quit")) {
		exit(0);
	}
	if (!strcmp(argv[0], "jobs")) {
	        listjobs(jobs);
		return 1;
	}
	if (!strcmp(argv[0], "bg") || !strcmp(argv[0], "fg")) {
		do_bgfg(argv);
		return 1;
	}

	return (0);     // This is not a built-in command.
}

/* 
 * do_bgfg - Execute the built-in bg and fg commands.
 *
 * Requires:
 *   "**argv" is an array of strings where the first string is either
 *   "bg" or "fg".
 *
 * Effects:
 *   Restarts the job given by the second element of the **argv array
 *   in either the foreground or background by sending the job a 
 *   SIGCONT signal. Prints an error if the bg/fg command was used 
 *   incorrectly.
 */
static void
do_bgfg(char **argv) 
{
	if (argv[1] == NULL) { 
		printf("%s command requires PID", argv[0]);
		printf(" or %%jobid argument\n");
	}
	
	char *jobnum = argv[1];
	int is_jid = 0;
	// If it's a jid
	if (jobnum[0] == '%') { 
		is_jid = 1;
		// Discard this character
		jobnum = &jobnum[1];
	}
	// check that id is valid number
	int i = 0;
	while (jobnum[i] != '\0') {
		if (!isdigit(jobnum[i])) {
			printf("%s command requires PID", argv[0]);
			printf(" or %%jobid argument\n");
			return;
		}
		i++;
	}

	int id = (int) strtol(jobnum, (char **)NULL, 10);

	JobP job;
	if (is_jid) {
		job = getjobjid(jobs, id);
		if (job == NULL) {
			printf("%%%d: No such job\n", id);
			return;
		}
	} else {
		job = getjobpid(jobs, (pid_t) id);
		if (job == NULL) {
			printf("%d: No such job\n", id);
			return;
		}
	}
	
	// Do the actual command
	
	kill(-job->pid, SIGCONT);
        if (!strcmp(argv[0], "bg")) {
		job->state = BG;
		printf("[%d] (%d) %s", job->jid, job->pid, job->cmdline);
	} else if (!strcmp(argv[0], "fg")) {
		job->state = FG;
		waitfg(job->pid);
	} else {
		Sio_error("unknown bg/fg command");
	}
}

/* 
 * waitfg - Block until process pid is no longer the foreground process.
 *
 * Requires:
 *   The pid of the job that the calling thread is to wait for.
 *
 * Effects:
 *   Suspends the calling thread until the job corresponding to the pid
 *   is no longer running in the foreground.
 */
static void
waitfg(pid_t pid)
{
	while (1) {
		sleep(1);
		
		JobP job = getjobpid(jobs, pid);
		// if fg task doesn't exist or it isn't FG, stop waiting
		if (job == NULL || job->state != FG) {
			break;
		}
	}
}

/* 
 * initpath - Perform all necessary initialization of the search path,
 *  which may be simply saving the path.
 *
 * Requires:
 *   "pathstr" is a string of directories separated by the ":"
 *   character.
 *
 * Effects:
 *   Parses the pathstr into the static search_path array.
 */
static void
initpath(const char *pathstr)
{
	if (pathstr == NULL) {
		search_path = NULL;
		return;
	}

	// Calculate the number of paths to store, equals
	// the number of colons in the string plus one,
	// plus 1 for the current dir.
	int num_paths = 2;
	int i = 0;
	while (pathstr[i] != '\0') {
		if (pathstr[i] == ':') {
			num_paths++;
		}
		i++;
	}
	// Allocate the path array
	if ((search_path = malloc(sizeof(char*) * (num_paths + 1))) == NULL) {
		Sio_error("Failed getting path");
	}
        
	// Put current directory at beginning so we search this first
	char *path_cwd;
	if ((path_cwd = malloc(sizeof(char) * PATH_MAX)) == NULL) {
		Sio_error("Failed getting path");
	}
	if (getcwd(path_cwd, sizeof(char) * PATH_MAX) == NULL) {
		Sio_error("Failed getting path");
	}
	search_path[0] = path_cwd;
	// Copy the paths into search_path
	int cur_pos = 0;
	for (i = 1; i < num_paths; i++) {
		int len = 0;
		while (pathstr[cur_pos + len] != ':' && 
		       pathstr[cur_pos + len] != '\0') {
			len++;
		}
		char *path;
		if (len == 0) {
			if ((path = malloc(sizeof(char) * PATH_MAX)) == NULL) {
				Sio_error("Failed getting path");
			}
			if (getcwd(path, sizeof(char) * PATH_MAX) == NULL) {
				Sio_error("Failed getting path");
			}
		} else {
			if ((path = malloc(sizeof(char) * (len + 1))) == NULL) {
				Sio_error("Failed getting path");
			}
			int z;
			for (z = 0; z < len; z++) {
				path[z] = pathstr[cur_pos + z];
			}
			path[z + 1] = '\0';
		}
		search_path[i] = path;
		cur_pos += len + 1;
	}
	search_path[num_paths] = NULL;
}

/*
 * The signal handlers follow.
 */

/* 
 * sigchld_handler - The kernel sends a SIGCHLD to the shell whenever
 *  a child job terminates (becomes a zombie), or stops because it
 *  received a SIGSTOP or SIGTSTP signal.  The handler reaps all
 *  available zombie children, but doesn't wait for any other
 *  currently running children to terminate.  
 *
 * Requires:
 *   An integer corresponding to the type of signal.
 *
 * Effects:
 *   Reaps all of the zombie children and delets their corresponding 
 *   job structs from jobs.
 */
static void
sigchld_handler(int signum)
{
	(void)signum;
        int olderrno = errno;
	sigset_t mask_all, prev_all;
	pid_t pid;
	int stat_loc;

	sigfillset(&mask_all);
	while ((pid = waitpid(-1, &stat_loc, WNOHANG | WUNTRACED)) > 0) {
		// If a job is stopped, we print it and stop it
		if (WIFSTOPPED(stat_loc)) {
			Sio_puts("Job [");
			Sio_putl((long) pid2jid(pid));
			Sio_puts("] (");
			Sio_putl((long) pid);
			Sio_puts(") stopped by signal SIGTSTP\n");
			sigprocmask(SIG_BLOCK, &mask_all, &prev_all);
			JobP job = getjobpid(jobs, pid);
			job->state = ST;
			sigprocmask(SIG_SETMASK, &prev_all, NULL);
		} else {
			// If the job was terminated by signal
			if (WIFSIGNALED(stat_loc)) {
				Sio_puts("Job [");
				Sio_putl((long) pid2jid(pid));
				Sio_puts("] (");
				Sio_putl((long) pid);
				Sio_puts(") terminated by signal SIGINT\n");
			}
			// Delete task
			sigprocmask(SIG_BLOCK, &mask_all, &prev_all);
			deletejob(jobs, pid);
			sigprocmask(SIG_SETMASK, &prev_all, NULL);
		}
	}

	errno = olderrno;
}

/* 
 * sigint_handler - The kernel sends a SIGINT to the shell whenever the
 *  user types ctrl-c at the keyboard.  Catch it and send it along
 *  to the foreground job.  
 *
 * Requires:
 *   An integer corresponding to the type of signal.
 *
 * Effects:
 *   Terminates each process in the foregound by sending a SIGINT signal 
 *   and then displays information about the job that was terminated. 
 */
static void
sigint_handler(int signum)
{
        pid_t pid = fgpid(jobs);
	if (pid == 0) {
		return;
	}
	// send signal to every process in pid process group
	kill(-pid, signum);
}

/*
 * sigtstp_handler - The kernel sends a SIGTSTP to the shell whenever
 *  the user types ctrl-z at the keyboard.  Catch it and suspend the
 *  foreground job by sending it a SIGTSTP.  
 *
 * Requires:
 *   An integer corresponding to the type of signal.
 *
 * Effects:
 *   Places each process in the foreground in the stopped state by sending 
 *   a SIGSTP signal.
 */
static void
sigtstp_handler(int signum)
{
	pid_t pid = fgpid(jobs);
	if (pid == 0) {
		return;
	}
	// send signal to every process in pid process group
	kill(-pid, signum);
}

/*
 * sigquit_handler - The driver program can gracefully terminate the
 *  child shell by sending it a SIGQUIT signal.
 *
 * Requires:
 *   "signum" is SIGQUIT.
 *
 * Effects:
 *   Terminates the program.
 */
static void
sigquit_handler(int signum)
{

	// Prevent an "unused parameter" warning.
	(void)signum;
	Sio_puts("Terminating after receipt of SIGQUIT signal\n");
	_exit(1);
}

/*
 * This comment marks the end of the signal handlers.
 */

/*
 * The following helper routines manipulate the jobs list.
 */

/*
 * Requires:
 *   "job" points to a job structure.
 *
 * Effects:
 *   Clears the fields in the referenced job structure.
 */
static void
clearjob(JobP job)
{

	job->pid = 0;
	job->jid = 0;
	job->state = UNDEF;
	job->cmdline[0] = '\0';
}

/*
 * Requires:
 *   "jobs" points to an array of MAXJOBS job structures.
 *
 * Effects:
 *   Initializes the jobs list to an empty state.
 */
static void
initjobs(JobP jobs)
{
	int i;

	for (i = 0; i < MAXJOBS; i++)
		clearjob(&jobs[i]);
}

/*
 * Requires:
 *   "jobs" points to an array of MAXJOBS job structures.
 *
 * Effects:
 *   Returns the largest allocated job ID.
 */
static int
maxjid(JobP jobs) 
{
	int i, max = 0;

	for (i = 0; i < MAXJOBS; i++)
		if (jobs[i].jid > max)
			max = jobs[i].jid;
	return (max);
}

/*
 * Requires:
 *   "jobs" points to an array of MAXJOBS job structures, and "cmdline" is
 *   a properly terminated string.
 *
 * Effects: 
 *   Adds a job to the jobs list. 
 */
static int
addjob(JobP jobs, pid_t pid, int state, const char *cmdline)
{
	int i;
    
	if (pid < 1)
		return (0);
	for (i = 0; i < MAXJOBS; i++) {
		if (jobs[i].pid == 0) {
			jobs[i].pid = pid;
			jobs[i].state = state;
			jobs[i].jid = nextjid++;
			if (nextjid > MAXJOBS)
				nextjid = 1;
			// Remove the "volatile" qualifier using a cast.
			strcpy((char *)jobs[i].cmdline, cmdline);
			if (verbose) {
				printf("Added job [%d] %d %s\n", jobs[i].jid,
				    (int)jobs[i].pid, jobs[i].cmdline);
			}
			return (1);
		}
	}
	printf("Tried to create too many jobs\n");
	return (0);
}

/*
 * Requires:
 *   "jobs" points to an array of MAXJOBS job structures.
 *
 * Effects:
 *   Deletes a job from the jobs list whose PID equals "pid".
 */
static int
deletejob(JobP jobs, pid_t pid) 
{
	int i;

	if (pid < 1)
		return (0);
	for (i = 0; i < MAXJOBS; i++) {
		if (jobs[i].pid == pid) {
			clearjob(&jobs[i]);
			nextjid = maxjid(jobs) + 1;
			return (1);
		}
	}
	return (0);
}

/*
 * Requires:
 *   "jobs" points to an array of MAXJOBS job structures.
 *
 * Effects:
 *   Returns the PID of the current foreground job or 0 if no foreground
 *   job exists.
 */
static pid_t
fgpid(JobP jobs)
{
	int i;

	for (i = 0; i < MAXJOBS; i++)
		if (jobs[i].state == FG)
			return (jobs[i].pid);
	return (0);
}

/*
 * Requires:
 *   "jobs" points to an array of MAXJOBS job structures.
 *
 * Effects:
 *   Returns a pointer to the job structure with process ID "pid" or NULL if
 *   no such job exists.
 */
static JobP
getjobpid(JobP jobs, pid_t pid)
{
	int i;

	if (pid < 1)
		return (NULL);
	for (i = 0; i < MAXJOBS; i++)
		if (jobs[i].pid == pid)
			return (&jobs[i]);
	return (NULL);
}

/*
 * Requires:
 *   "jobs" points to an array of MAXJOBS job structures.
 *
 * Effects:
 *   Returns a pointer to the job structure with job ID "jid" or NULL if no
 *   such job exists.
 */
static JobP
getjobjid(JobP jobs, int jid) 
{
	int i;

	if (jid < 1)
		return (NULL);
	for (i = 0; i < MAXJOBS; i++)
		if (jobs[i].jid == jid)
			return (&jobs[i]);
	return (NULL);
}

/*
 * Requires:
 *   Nothing.
 *
 * Effects:
 *   Returns the job ID for the job with process ID "pid" or 0 if no such
 *   job exists.
 */
static int
pid2jid(pid_t pid) 
{
	int i;

	if (pid < 1)
		return (0);
	for (i = 0; i < MAXJOBS; i++)
		if (jobs[i].pid == pid)
			return (jobs[i].jid);
	return (0);
}

/*
 * Requires:
 *   "jobs" points to an array of MAXJOBS job structures.
 *
 * Effects:
 *   Prints the jobs list.
 */
static void
listjobs(JobP jobs) 
{
	int i;

	for (i = 0; i < MAXJOBS; i++) {
		if (jobs[i].pid != 0) {
			printf("[%d] (%d) ", jobs[i].jid, (int)jobs[i].pid);
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
				printf("listjobs: Internal error: "
				    "job[%d].state=%d ", i, jobs[i].state);
			}
			printf("%s", jobs[i].cmdline);
		}
	}
}

/*
 * This comment marks the end of the jobs list helper routines.
 */

/*
 * Other helper routines follow.
 */

/*
 * Requires:
 *   Nothing.
 *
 * Effects:
 *   Prints a help message.
 */
static void
usage(void) 
{

	printf("Usage: shell [-hvp]\n");
	printf("   -h   print this message\n");
	printf("   -v   print additional diagnostic information\n");
	printf("   -p   do not emit a command prompt\n");
	exit(1);
}

/*
 * Requires:
 *   "msg" is a properly terminated string.
 *
 * Effects:
 *   Prints a Unix-style error message and terminates the program.
 */
static void
unix_error(const char *msg)
{

	fprintf(stdout, "%s: %s\n", msg, strerror(errno));
	exit(1);
}

/*
 * Requires:
 *   "msg" is a properly terminated string.
 *
 * Effects:
 *   Prints "msg" and terminates the program.
 */
static void
app_error(const char *msg)
{

	fprintf(stdout, "%s\n", msg);
	exit(1);
}

/*
 * Requires:
 *   The character array "s" is sufficiently large to store the ASCII
 *   representation of the long "v" in base "b".
 *
 * Effects:
 *   Converts a long "v" to a base "b" string, storing that string in the
 *   character array "s" (from K&R).  This function can be safely called by
 *   a signal handler.
 */
static void
sio_ltoa(long v, char s[], int b)
{
	int c, i = 0;

	do
		s[i++] = (c = v % b) < 10 ? c + '0' : c - 10 + 'a';
	while ((v /= b) > 0);
	s[i] = '\0';
	sio_reverse(s);
}

/*
 * Requires:
 *   "s" is a properly terminated string.
 *
 * Effects:
 *   Reverses a string (from K&R).  This function can be safely called by a
 *   signal handler.
 */
static void
sio_reverse(char s[])
{
	int c, i, j;

	for (i = 0, j = sio_strlen(s) - 1; i < j; i++, j--) {
		c = s[i];
		s[i] = s[j];
		s[j] = c;
	}
}

/*
 * Requires:
 *   "s" is a properly terminated string.
 *
 * Effects:
 *   Computes and returns the length of the string "s".  This function can be
 *   safely called by a signal handler.
 */
static size_t
sio_strlen(const char s[])
{
	size_t i = 0;

	while (s[i] != '\0')
		i++;
	return (i);
}

/*
 * Requires:
 *   None.
 *
 * Effects:
 *   Prints the long "v" to stdout using only functions that can be safely
 *   called by a signal handler, and returns either the number of characters
 *   printed or -1 if the long could not be printed.
 */
static ssize_t
sio_putl(long v)
{
	char s[128];
    
	sio_ltoa(v, s, 10);
	return (sio_puts(s));
}

/*
 * Requires:
 *   "s" is a properly terminated string.
 *
 * Effects:
 *   Prints the string "s" to stdout using only functions that can be safely
 *   called by a signal handler, and returns either the number of characters
 *   printed or -1 if the string could not be printed.
 */
static ssize_t
sio_puts(const char s[])
{

	return (write(STDOUT_FILENO, s, sio_strlen(s)));
}

/*
 * Requires:
 *   "s" is a properly terminated string.
 *
 * Effects:
 *   Prints the string "s" to stdout using only functions that can be safely
 *   called by a signal handler, and exits the program.
 */
static void
sio_error(const char s[])
{

	sio_puts(s);
	_exit(1);
}

/*
 * Requires:
 *   None.
 *
 * Effects:
 *   Prints the long "v" to stdout using only functions that can be safely
 *   called by a signal handler.  Either returns the number of characters
 *   printed or exits if the long could not be printed.
 */
static ssize_t
Sio_putl(long v)
{
	ssize_t n;
  
	if ((n = sio_putl(v)) < 0)
		sio_error("Sio_putl error");
	return (n);
}

/*
 * Requires:
 *   "s" is a properly terminated string.
 *
 * Effects:
 *   Prints the string "s" to stdout using only functions that can be safely
 *   called by a signal handler.  Either returns the number of characters
 *   printed or exits if the string could not be printed.
 */
static ssize_t
Sio_puts(const char s[])
{
	ssize_t n;
  
	if ((n = sio_puts(s)) < 0)
		sio_error("Sio_puts error");
	return (n);
}

/*
 * Requires:
 *   "s" is a properly terminated string.
 *
 * Effects:
 *   Prints the string "s" to stdout using only functions that can be safely
 *   called by a signal handler, and exits the program.
 */
static void
Sio_error(const char s[])
{

	sio_error(s);
}

// Prevent "unused function" and "unused variable" warnings.
static const void *dummy_ref[] = { Sio_error, Sio_putl, addjob, builtin_cmd,
    deletejob, do_bgfg, dummy_ref, fgpid, getjobjid, getjobpid, listjobs,
    parseline, pid2jid, signame, waitfg };
