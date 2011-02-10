/*
    Example forking daemon utilizing SIGCHLD, with lots of comments.

    For instruction, and perhaps ridicule.

    Copyright (c) 2011 Sung Pae <sung@metablu.com>
    Distributed under the MIT license.
*/

#include <stdio.h>
#include <stdlib.h>     /* core functions */
#include <unistd.h>     /* POSIX API (fork/exec, etc) */
#include <signal.h>     /* signal handling */
#include <fcntl.h>      /* file handling */
#include <stdbool.h>    /* C99 boolean types */
#include <errno.h>      /* provides global variable errno */
#include <string.h>     /* basic string functions */
#include <sys/stat.h>   /* inode manipulation (needed for umask()) */
#include <sys/wait.h>   /* for waitpid() and friends on linux */


/*
 * TYPEDEFS (custom types)
 */

/* options storage */
typedef struct {
    int                 jobs;           /* number of children to fork */
    bool                daemonize;      /* fork to background? */
    char                logfile[0xff];  /* File to log to when daemonized */
} options_t;

/* simple storage for registering signal handlers */
typedef struct {
    int                 signal;         /* What SIGNAL to trap */
    struct sigaction    action;         /* handler, passed to sigaction() */
} sigpair_t;


/*
 * FILE VARIABLES (effectively global, since this is a one-file program)
 */

options_t   options;                    /* global options */
int         sigcount = 0;               /* total signals trapped */
sigpair_t   sigpairs[0xff];             /* array of SIGNALS, with handlers */
pid_t       children[0xff];             /* array of child process ids */


/*
 * FUNCTIONS
 */

/* When passed true, traps signals and assigns handlers as defined in sigpairs[]
 * When passed false, resets all trapped signals back to their default behavior.
 */
int trap_signals(bool on)
{
    int i;
    struct sigaction dfl;       /* the handler object */

    dfl.sa_handler = SIG_DFL;   /* for resetting to default behavior */

    /* Loop through all registered signals and either set to the new handler
     * or reset them back to the default */
    for (i = 0; i < sigcount; ++i) {
        /* notice that the second parameter takes the address of the handler */
        if (sigaction(sigpairs[i].signal, on ? &sigpairs[i].action : &dfl, NULL) < 0)
            return false;
    }

    return true;
}

/* Forks a child process, and returns true if successful.
 * The forked child blocks and randomly dies.
 *
 * If this program did any work, the main communication between parent and child
 * could take place through a pipe(2).
 */
bool child(int id)
{
    pid_t   pid;

    /* see main() for discussion on fork() */
    pid = fork();

    if (pid < 0) {
        return false;
    } else if (pid > 0) {
        printf("Spawning child %d (pid %d)\n", id, pid);
        children[id] = pid; /* record the child pid */
        return true;
    }

    /* Child process continues here */

    /* Children processes are exact (almost) copies of the parent!
     * Including signal traps! Our parent has a SIGTERM handler, and this child
     * will dutifully execute the handler on SIGTERM unless we reset all
     * signals back to their default handlers */
    if (!trap_signals(false)) {
        fprintf(stderr, "Child %d: trap_signals() failed!\n", id);
        exit(1);
    }

    /* Block, and randomly die.
     * If you're on Linux, arc4random() is why you need to link to libbsd
     * (because it works, and I'm lazy) */
    while (1) {
        arc4random_stir();
        if (arc4random() % 20)
            sleep(1);
        else
            break;
    }

    exit(0);
}

/* Master's SIGCHLD handler.
 *
 * When a process is fork()ed by a process, the new process is an exact copy
 * of the old process, except for a few values, one of which is that the parent
 * pid of the child is that of the process that forked it.
 *
 * When this child exits, the signal SIGCHLD is sent to the parent process to
 * alert it. By default, the signal is ignored, but we can take this opportunity
 * to restart any children that have died.
 *
 * There are many ways to determine which children have died, but the most
 * portable method is to use the wait() family of system calls.
 *
 * A dead child process releases its memory, but sticks around so that any
 * interested parties can determine how they died (exit status). Calling wait()
 * in the master collects the status of the first available dead process, and
 * removes it from the process table.
 *
 * If wait() is never called by the parent, the dead child sticks around as a
 * "zombie" process, marked with status `Z' in ps output. If the parent process
 * exits without ever calling wait, the zombie process does not disappear, but
 * is inherited by the root process (its parent pid is set to 1).
 *
 * Because SIGCHLD is an asynchronous signal, it is possible that if many
 * children die simultaneously, the parent may only notice one SIGCHLD when many
 * have been sent. In order to beat this edge case, we can simply loop through
 * all the known children and call waitpid() in non-blocking mode to see if they
 * have died, and spawn a new one in their place.
 */
void restart_children()
{
    int     i, status[0xff];    /* array of exit statuses, just in case */
    pid_t   pid;

    /* Racing signals may let dead children through the cracks,
     * so check all children
     */
    for (i = 0; i < options.jobs; ++i) {
        pid = waitpid(children[i], &status[i], WNOHANG); /* non-blocking! */

        if (pid < 0) {
            perror("waitpid()");
        } else if (!pid) {
            /* waitpid returns 0 when the process is alive and well */
            continue;
        } else {
            printf("Master: reaped dead child %d (pid %d)\n", i, pid);
            child(i); /* relaunch this particular child */
        }
    }
}

/* Master's kill switch
 *
 * It's important to ensure that all children have exited before the master
 * exits so no root zombies are created. The default handler for SIGINT sends
 * SIGINT to all children, but this is not true with SIGTERM.
 */
void terminate_children()
{
    int     i, status;
    pid_t   pid;

    printf("Termination signal received! Killing children");

    /* Reset all signals! If the SIGCHLD handler in particular is not reset,
     * all terminated children will be restarted
     */
    trap_signals(false);

    for (i = 0; i < options.jobs; ++i)
        kill(children[i], SIGTERM);

    /* reap all children as they die, until none are left;
     * at that point wait() returns -1 */
    while ((pid = wait(&status)) != -1)
        printf(".");

    printf("\nAll children reaped, shutting down.\n");
    exit(0);
}

/* Populate our array of custom handler objects */
void register_signals()
{
    int i = 0;

    sigpairs[i].signal            = SIGCHLD;
    sigpairs[i].action.sa_handler = &restart_children;
    /* Don't send SIGCHLD when a process has been frozen (e.g. Ctrl-Z) */
    sigpairs[i].action.sa_flags   = SA_NOCLDSTOP;

    sigpairs[++i].signal          = SIGINT;
    sigpairs[i].action.sa_handler = &terminate_children;

    sigpairs[++i].signal          = SIGTERM;
    sigpairs[i].action.sa_handler = &terminate_children;

    /* setting sigcount now is easier than doing it dynamically */
    sigcount = ++i;
}

/* Fork children, trap signals, block, and wait.
 *
 * Return values become exit values for the program.
 */
int master()
{
    int i;

    /* spawn some children */
    for (i = 0; i < options.jobs; ++i) {
        if (!child(i)) {
            fprintf(stderr, "child() failed!\n");
            return 1;
        }
    }

    register_signals();

    /* Trap after forking children, during which SIGCHLD should be ignored
     *
     * This bit is new to me; forking many times in a row can lead to stillborn
     * children. The program will handle this gracefully by itself if SIGCHLD
     * is left alone during the spawning.
     */
    if (!trap_signals(true)) {
        fprintf(stderr, "trap_signals() failed!\n");
        return 1;
    }

    /* Block and wait for signals.
     *
     * An alternate, synchronous strategy to SIGCHLD could be deployed here,
     * which is to call waitpid(..., WNOHANG) in a loop
     */
    while (1) sleep(1);

    return 0;
}

/* Simple options parsing with getopt(), which has the old UNIX semantics.
 *
 * Modern GNU style long options processing can be achieved with getopt_long()
 */
void optparse(int argc, char *argv[])
{
    int     opt;
    bool    clean_exit = false;

    /* default options */
    options.jobs      = 2;
    options.daemonize = false;
    strcpy(options.logfile, "/dev/null\0");

    /* Colons indicate flags that have required arguments */
    while ((opt = getopt(argc, argv, "hdf:j:")) != -1) {
        switch (opt) {
        case 'd':
            options.daemonize = true;
            break;
        case 'f':
            /* strncpy() to prevent buffer overflows */
            strncpy(options.logfile, optarg, sizeof(options.logfile));
            break;
        case 'j':
            /* keep it simple; no argument checking */
            options.jobs = atoi(optarg);
            break;
        case 'h':
            clean_exit = true;
            /* fall through */
        default:
            printf("An example forking daemon utilizing SIGCHLD.\n\n");
            printf("Usage: %s [options]\n\n", argv[0]);
            printf("Options:\n");
            printf("    -j JOBS     number of children to spawn\n");
            printf("    -f FILE     log to file when daemonized\n");
            printf("    -d          daemonize\n");
            printf("    -h\n");

            exit(clean_exit ? 0 : 1);
        }
    }
}

/* Go through the proper incantations to make this a proper UNIX daemon */
int daemonize()
{
    int log;

    /* Reset master file umask in case it has been altered.
     * Notice that this is a bitmask, and not a file mode!
     *
     * e.g. umask(022); open(f, O_CREAT, 0777) => -rwxr-xr-x
     *      umask(077); open(f, O_CREAT, 0777) => -rwx------
     *      umask(0);   open(f, O_CREAT, 0777) => -rwxrwxrwx
     *
     * Setting to zero essentially allows the program to fully manage its
     * file permissions.
     */
    umask(0);

    /* Redirect the 3 standard streams to /dev/null, or to a specified logfile.
     *
     * This is better than closing the streams, since printf() to a closed file
     * descriptor will raise noisy errors.
     */
    if ((log = open(options.logfile, O_WRONLY|O_APPEND|O_CREAT, 0644)) < 0) {
        perror("open()");
        return errno;
    }
    dup2(log, STDIN_FILENO);
    dup2(log, STDOUT_FILENO);
    dup2(log, STDERR_FILENO);
    close(log);

    /* Create a new session.
     *
     * Processes are not only grouped hierarchically by parent pid, but also by
     * `process groups', which are typically composed of programs launched by
     * a common process, like your shell (which of course often correspond
     * exactly with the parent-child hierarchy).
     *
     * Process groups can be targets of various signals, so it's important to
     * break from the parent's group and form a new group (aka session). Calling
     * setsid() makes this process the session leader for a new process group.
     */
    if (setsid() < 0) {
        perror("setsid()");
        return errno;
    }

    /* Change working directory to root.
     *
     * Every process has a working directory. Life is better when your working
     * directory is set to something that is liable to disappear.
     */
    if (chdir("/") < 0) {
        perror("chdir()");
        return errno;
    }

    return 0;
}

/* Entry routine; parse command line options and launch master process.
 *
 * REGARDING fork():
 *
 * Fork is a strange and unique system call. Along with exec(), it forms one of
 * the core features of Unix.
 *
 * At the moment fork() is called,
 *   - if the fork failed, it returns -1
 *   - if the fork succeeds, a duplicate of the parent process is formed,
 *     except that the caller is designated as the parent of new process
 *   - fork() returns the pid of the newly created process
 *   - BUT IN THE CHILD process, fork returns 0
 *
 * Because the child process will see fork() returning 0, and the parent will
 * see fork() returning a pid, we can carefully diverge the behavior of both to
 * do what we want!
 */
int main(int argc, char *argv[])
{
    pid_t   pid;
    int     status;

    optparse(argc, argv);

    if (options.daemonize) {
        pid = fork();

        if (pid < 0) {
            /* fork failed, probably due to resource limits */
            perror("fork()");
            return errno;
        } else if (pid > 0) {
            /* fork successful! Tell us the forked processes's pid and exit */
            printf("Forked master process: %d\n", pid);
            return 0;
        }

        /* Forked process continues here */

        if (status = daemonize())
            exit(status);
    }

    return master();
}
