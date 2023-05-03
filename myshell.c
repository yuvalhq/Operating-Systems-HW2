#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>

#define PIPE '|'
#define AMPERSAND '&'
#define REDIRECT '<'
#define FAIL -1
#define CHILD 0

#define PIPE_ERROR "Error while piping"
#define OPEN_ERROR "Failed to open the file"
#define CHECK_FORK_STATUS(pid) \
    if (pid == FAIL) { \
        perror("Error while forking"); \
        return 0; \
    }
#define FINALIZE_WAIT_ERROR "Waitpid() failed in finalize()"
#define CHECK_WAIT_STATUS(status) \
    if (status == FAIL && errno != EINTR && errno != ECHILD) { \
        perror("Waitpid() failed"); \
        return 0; \
    }
#define CHECK_DUP_STATUS(status) \
    if (status == FAIL) { \
        perror("dup2() failed"); \
        exit(1); \
    }
#define CHECK_EXEC_STATUS(status) \
    if (status == FAIL) { \
        perror("Error while executing the command"); \
        exit(1); \
    }

#define PREPARE_SIGINT_SIGCHLD_ERROR "SIGINT or SIGCHLD error in prepare()"
#define SIGNAL_HANDLER() \
    if (signal(SIGINT, SIG_DFL) == SIG_ERR) { \
        perror("SIGINT error"); \
        exit(1); \
    } \
    if (signal(SIGCHLD, SIG_DFL) == SIG_ERR) { \
        perror("SIGCHLD error"); \
        exit(1); \
    }


int prepare(void);
int process_arglist(int count, char **arglist);
int finalize(void);
int executing_commands_in_the_background(int count, char **arglist);
int input_redirecting(int count, char **arglist);
int single_piping(int index, char **arglist);
int check_for_pipe(int count, char **arglist);
int single_piping(int index, char **arglist);
int executing_commands(char **arglist);

// this methods deals with SIGINT and SIGCHLD issues before the shell is ready to accept commands
int prepare(void) {
    if (signal(SIGINT, SIG_IGN) == SIG_ERR || signal(SIGCHLD, SIG_IGN) == SIG_ERR) {
        perror(PREPARE_SIGINT_SIGCHLD_ERROR);
        return 1;
    }
    return 0;
}

// execute the given command through the arglist
int process_arglist(int count, char **arglist) {
    // choosing how to execute according to the arglist:
    if (*arglist[count - 1] == AMPERSAND) {
        return executing_commands_in_the_background(count, arglist);
    }
    if (count >= 2 && arglist[count - 2] != NULL && *arglist[count - 2] == REDIRECT) {
        return input_redirecting(count, arglist);
    }
    int idx = check_for_pipe(count, arglist);
    if (idx != FAIL) {
        return single_piping(idx, arglist);
    }
    return executing_commands(arglist);
}

// this method is called when the shell is about to exit and deals with zombie processes
int finalize(void) {
    int status;
    pid_t pid;

    // Reap zombie processes using waitpid() with WNOHANG
    do {
        pid = waitpid(-1, &status, WNOHANG);
        if (pid == -1 && errno != ECHILD) {
            perror(FINALIZE_WAIT_ERROR);
            return 1;
        }
    } while (pid > 0);

    return 0;
}

// execute the given command in the background (does not wait for it to complete)
int executing_commands_in_the_background(int count, char **arglist) {
    arglist[count - 1] = NULL; // removes the & from the arglist's end
    pid_t pid = fork(); // run the command in the background using a child process
    CHECK_FORK_STATUS(pid);

    if (pid == CHILD) {
        // signal handling
        SIGNAL_HANDLER();

        // execute the command
        int exec_status = execvp(arglist[0], arglist);
        CHECK_EXEC_STATUS(exec_status);
    }
    return 1; // no need to wait for the child process to complete
}

// execute the command so that the standard output is redirected to the output file
int input_redirecting(int count, char **arglist) {
    arglist[count - 2] = NULL;
    pid_t pid = fork();
    CHECK_FORK_STATUS(pid);

    if (pid == CHILD) {
        // signal handling
        SIGNAL_HANDLER();

        // creating a file descriptor for the input file
        int input_fd = open(arglist[count - 1], O_RDONLY);
        if (input_fd == FAIL) {
            perror(OPEN_ERROR);
            exit(1);
        }

        // redirecting the standard input to the input file
        int dup_status = dup2(input_fd, 0);
        close(input_fd);
        CHECK_DUP_STATUS(dup_status);

        // execute the command
        int exec_status = execvp(arglist[0], arglist);
        CHECK_EXEC_STATUS(exec_status);
    }

    // parent - wait for the child process to complete
    int wait_status = waitpid(pid, NULL, 0);
    CHECK_WAIT_STATUS(wait_status);
    return 1;
}

// checks if there is a pipe ('|') in the arglist
int check_for_pipe(int count, char **arglist) {
    int i;

    for (i = 0; i < count; i++) {
        if (*arglist[i] == PIPE) {
            return i;
        }
    }
    return FAIL;
}

// execute the commands so that the standard output of the first command is piped to the standard input of the second command
int single_piping(int idx, char **arglist) {
    int pipe_fd[2]; // pipe_fd[0] is the read end of the pipe and pipe_fd[1] is the write end of the pipe

    arglist[idx] = NULL; // removes the | from the arglist
    if (pipe(pipe_fd) == FAIL) {
        perror(PIPE_ERROR);
        return 0;
    }

    // FIRST child
    pid_t first_pid = fork();
    CHECK_FORK_STATUS(first_pid);
    if (first_pid == CHILD) {
        // signal handling
        SIGNAL_HANDLER();

        // redirecting the standard output to the write end of the pipe
        // no need for the read end of the pipe in the first child
        close(pipe_fd[0]);
        int dup_status = dup2(pipe_fd[1], 1);
        close(pipe_fd[1]);
        CHECK_DUP_STATUS(dup_status);

        // execute the command
        int exec_status = execvp(arglist[0], arglist);
        CHECK_EXEC_STATUS(exec_status);
    }

    // SECOND child
    pid_t second_pid = fork();
    CHECK_FORK_STATUS(second_pid);
    if (second_pid == CHILD) {
        // signal handling
        SIGNAL_HANDLER();

        // redirecting the standard input to the read end of the pipe
        // no need for the write end of the pipe in the second child
        close(pipe_fd[1]);
        int dup_status = dup2(pipe_fd[0], 0);
        close(pipe_fd[0]);
        CHECK_DUP_STATUS(dup_status);

        // execute the command
        int exec_status = execvp(arglist[idx + 1], arglist + idx + 1);
        CHECK_EXEC_STATUS(exec_status);
    }

    // parent - wait for the children processes to complete and close the pipe
    close(pipe_fd[0]);
    close(pipe_fd[1]);
    int first_wait_status = waitpid(first_pid, NULL, 0);
    CHECK_WAIT_STATUS(first_wait_status);
    int second_wait_status = waitpid(second_pid, NULL, 0);
    CHECK_WAIT_STATUS(second_wait_status);
    return 1;
}

// execute the command - no piping, input redirection or background execution
int executing_commands(char **arglist) {
    pid_t pid = fork();
    CHECK_FORK_STATUS(pid);

    if (pid == CHILD) {
        // signal handling
        SIGNAL_HANDLER();

        // execute the command
        int exec_status = execvp(arglist[0], arglist);
        CHECK_EXEC_STATUS(exec_status);
    }

    // parent - wait for the child process to complete
    int wait_status = waitpid(pid, NULL, 0);
    CHECK_WAIT_STATUS(wait_status);
    return 1;
}
