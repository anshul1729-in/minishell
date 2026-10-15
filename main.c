#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#define MAX_LINE 1024
#define MAX_ARGS 64
#define MAX_CMDS 16
#define MAX_JOBS 32

extern char **environ;

typedef enum { JOB_RUNNING, JOB_STOPPED } job_state_t;

typedef struct {
    int id;
    pid_t pgid;
    job_state_t state;
    char cmdline[MAX_LINE];
    int active;
} job_t;

static job_t jobs[MAX_JOBS];
static int next_job_id = 1;
static pid_t shell_pgid;

/* Structure representing a single stage in a pipeline (e.g., cmd1 | cmd2) */
typedef struct {
    char *argv[MAX_ARGS];
    char *infile;
    char *outfile;
    int err_to_out; /* 1 if 2>&1 is present */
} command_t;

/* --- Job Table Helpers --- */
int add_job(pid_t pgid, job_state_t state, const char *cmdline) {
    for (int i = 0; i < MAX_JOBS; i++) {
        if (!jobs[i].active) {
            jobs[i].active = 1;
            jobs[i].id = next_job_id++;
            jobs[i].pgid = pgid;
            jobs[i].state = state;
            strncpy(jobs[i].cmdline, cmdline, MAX_LINE - 1);
            return jobs[i].id;
        }
    }
    return -1;
}

job_t *find_job_by_id(int id) {
    for (int i = 0; i < MAX_JOBS; i++) {
        if (jobs[i].active && jobs[i].id == id) return &jobs[i];
    }
    return NULL;
}

void remove_job(pid_t pgid) {
    for (int i = 0; i < MAX_JOBS; i++) {
        if (jobs[i].active && jobs[i].pgid == pgid) {
            jobs[i].active = 0;
            return;
        }
    }
}

/* Reap background processes that have finished without blocking the prompt */
void reap_background_jobs(void) {
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG | WUNTRACED)) > 0) {
        for (int i = 0; i < MAX_JOBS; i++) {
            if (jobs[i].active && jobs[i].pgid == pid) {
                if (WIFEXITED(status) || WIFSIGNALED(status)) {
                    printf("\n[%d] Done       %s\n", jobs[i].id, jobs[i].cmdline);
                    jobs[i].active = 0;
                } else if (WIFSTOPPED(status)) {
                    jobs[i].state = JOB_STOPPED;
                    printf("\n[%d] Stopped    %s\n", jobs[i].id, jobs[i].cmdline);
                }
            }
        }
    }
}

/* Resolves command name across $PATH and invokes the raw execve() syscall */
void exec_with_path(char **argv) {
    if (strchr(argv[0], '/') != NULL) {
        execve(argv[0], argv, environ);
        perror("execve");
        exit(127);
    }

    char *path_env = getenv("PATH");
    if (!path_env) path_env = "/bin:/usr/bin";

    char path_copy[MAX_LINE];
    strncpy(path_copy, path_env, sizeof(path_copy) - 1);

    char *dir = strtok(path_copy, ":");
    while (dir != NULL) {
        char fullpath[MAX_LINE];
        snprintf(fullpath, sizeof(fullpath), "%s/%s", dir, argv[0]);
        execve(fullpath, argv, environ);
        dir = strtok(NULL, ":");
    }

    fprintf(stderr, "minishell: command not found: %s\n", argv[0]);
    exit(127);
}

/* Waits for a foreground process group and handles Ctrl+Z (SIGTSTP) */
void wait_for_foreground_job(pid_t pgid, const char *cmdline) {
    int status;
    pid_t pid;

    /* Hand terminal control to the foreground job so it receives Ctrl+C / Ctrl+Z */
    tcsetpgrp(STDIN_FILENO, pgid);

    while ((pid = waitpid(-pgid, &status, WUNTRACED)) > 0) {
        if (WIFSTOPPED(status)) {
            int jid = add_job(pgid, JOB_STOPPED, cmdline);
            printf("\n[%d] Stopped    %s\n", jid, cmdline);
            break;
        }
    }

    /* Return terminal control back to minishell */
    tcsetpgrp(STDIN_FILENO, shell_pgid);
}

/* Parses a single pipeline stage for tokens and redirections (<, >, 2>&1) */
void parse_stage(char *stage_str, command_t *cmd) {
    memset(cmd, 0, sizeof(command_t));
    int argc = 0;
    char *tok = strtok(stage_str, " \t\n");

    while (tok != NULL) {
        if (strcmp(tok, "<") == 0) {
            cmd->infile = strtok(NULL, " \t\n");
        } else if (strcmp(tok, ">") == 0) {
            cmd->outfile = strtok(NULL, " \t\n");
        } else if (strcmp(tok, "2>&1") == 0) {
            cmd->err_to_out = 1;
        } else {
            cmd->argv[argc++] = tok;
        }
        tok = strtok(NULL, " \t\n");
    }
    cmd->argv[argc] = NULL;
}

/* Executes single commands or multi-stage pipelines (cmd1 | cmd2 | cmd3) */
void execute_pipeline(char *raw_line) {
    char cmdline_copy[MAX_LINE];
    strncpy(cmdline_copy, raw_line, MAX_LINE - 1);
    cmdline_copy[strcspn(cmdline_copy, "\n")] = '\0';

    /* Check for trailing '&' (background execution) */
    int is_bg = 0;
    for (int i = (int)strlen(raw_line) - 1; i >= 0; i--) {
        if (raw_line[i] == ' ' || raw_line[i] == '\t' || raw_line[i] == '\n') continue;
        if (raw_line[i] == '&') {
            is_bg = 1;
            raw_line[i] = ' ';
        }
        break;
    }

    /* Split input by '|' into pipeline stages */
    char *stage_strs[MAX_CMDS];
    int num_cmds = 0;
    char *saveptr;
    char *stage = strtok_r(raw_line, "|", &saveptr);
    while (stage != NULL && num_cmds < MAX_CMDS) {
        stage_strs[num_cmds++] = stage;
        stage = strtok_r(NULL, "|", &saveptr);
    }
    if (num_cmds == 0) return;

    command_t cmds[MAX_CMDS];
    for (int i = 0; i < num_cmds; i++) {
        parse_stage(stage_strs[i], &cmds[i]);
    }
    if (cmds[0].argv[0] == NULL) return;

    /* Handle Shell Built-in Commands (cd, exit, jobs, fg, bg) */
    char *first = cmds[0].argv[0];
    if (strcmp(first, "exit") == 0) exit(0);

    if (strcmp(first, "cd") == 0) {
        char *target = cmds[0].argv[1] ? cmds[0].argv[1] : getenv("HOME");
        if (chdir(target) != 0) perror("cd");
        return;
    }

    if (strcmp(first, "jobs") == 0) {
        for (int i = 0; i < MAX_JOBS; i++) {
            if (jobs[i].active) {
                printf("[%d] %-10s (pgid: %d)  %s\n", jobs[i].id,
                       jobs[i].state == JOB_RUNNING ? "Running" : "Stopped",
                       jobs[i].pgid, jobs[i].cmdline);
            }
        }
        return;
    }

    if (strcmp(first, "fg") == 0 || strcmp(first, "bg") == 0) {
        if (!cmds[0].argv[1]) {
            fprintf(stderr, "Usage: %s <job_id>\n", first);
            return;
        }
        int id = atoi(cmds[0].argv[1]);
        job_t *job = find_job_by_id(id);
        if (!job) {
            fprintf(stderr, "%s: job %d not found\n", first, id);
            return;
        }
        kill(-job->pgid, SIGCONT);
        if (strcmp(first, "fg") == 0) {
            pid_t pgid = job->pgid;
            char saved_cmd[MAX_LINE];
            strncpy(saved_cmd, job->cmdline, MAX_LINE);
            remove_job(pgid);
            wait_for_foreground_job(pgid, saved_cmd);
        } else {
            job->state = JOB_RUNNING;
            printf("[%d] %s &\n", job->id, job->cmdline);
        }
        return;
    }

    /* Launch Pipeline Stages using pipe(), fork(), dup2(), and execve() */
    int prev_pipe_read = -1;
    int pipefd[2];
    pid_t pgid = 0;

    for (int i = 0; i < num_cmds; i++) {
        if (i < num_cmds - 1) {
            if (pipe(pipefd) < 0) {
                perror("pipe");
                return;
            }
        }

        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            return;
        }

        if (pid == 0) {
            /* --- CHILD PROCESS --- */
            pid_t child_pgid = (pgid == 0) ? getpid() : pgid;
            setpgid(0, child_pgid);

            /* Restore default signal handlers so Ctrl+C / Ctrl+Z work on children */
            signal(SIGINT, SIG_DFL);
            signal(SIGTSTP, SIG_DFL);
            signal(SIGTTIN, SIG_DFL);
            signal(SIGTTOU, SIG_DFL);

            /* 1. Wire pipe input from previous command to STDIN (FD 0) */
            if (prev_pipe_read != -1) {
                dup2(prev_pipe_read, STDIN_FILENO);
                close(prev_pipe_read);
            }

            /* 2. Wire pipe output to next command via STDOUT (FD 1) */
            if (i < num_cmds - 1) {
                close(pipefd[0]);
                dup2(pipefd[1], STDOUT_FILENO);
                close(pipefd[1]);
            }

            /* 3. Handle '<' input file redirection */
            if (cmds[i].infile) {
                int fd = open(cmds[i].infile, O_RDONLY);
                if (fd < 0) { perror(cmds[i].infile); exit(1); }
                dup2(fd, STDIN_FILENO);
                close(fd);
            }

            /* 4. Handle '>' output file redirection */
            if (cmds[i].outfile) {
                int fd = open(cmds[i].outfile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if (fd < 0) { perror(cmds[i].outfile); exit(1); }
                dup2(fd, STDOUT_FILENO);
                close(fd);
            }

            /* 5. Handle '2>&1' (redirect STDERR FD 2 to current STDOUT FD 1) */
            if (cmds[i].err_to_out) {
                dup2(STDOUT_FILENO, STDERR_FILENO);
            }

            exec_with_path(cmds[i].argv);
        } else {
            /* --- PARENT PROCESS --- */
            if (pgid == 0) pgid = pid;
            setpgid(pid, pgid);

            if (prev_pipe_read != -1) close(prev_pipe_read);
            if (i < num_cmds - 1) {
                close(pipefd[1]);
                prev_pipe_read = pipefd[0];
            }
        }
    }

    if (is_bg) {
        int jid = add_job(pgid, JOB_RUNNING, cmdline_copy);
        printf("[%d] %d\n", jid, pgid);
    } else {
        wait_for_foreground_job(pgid, cmdline_copy);
    }
}

int main(void) {
    /* Ignore interactive & job-control signals in the main shell process */
    signal(SIGINT, SIG_IGN);
    signal(SIGTSTP, SIG_IGN);
    signal(SIGTTIN, SIG_IGN);
    signal(SIGTTOU, SIG_IGN);

    shell_pgid = getpid();
    setpgid(shell_pgid, shell_pgid);
    tcsetpgrp(STDIN_FILENO, shell_pgid);

    char line[MAX_LINE];
    while (1) {
        reap_background_jobs();
        printf("minishell> ");
        fflush(stdout);

        if (fgets(line, sizeof(line), stdin) == NULL) {
            printf("\n");
            break;
        }
        execute_pipeline(line);
    }
    return 0;
}