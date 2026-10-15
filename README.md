# minishell

A custom POSIX-compliant command-line shell written in C that implements multi-stage pipelines (`|`), file descriptor redirection (`>`, `<`, `2>&1`), background job execution (`&`), and interactive signal handling (`Ctrl+C`, `Ctrl+Z`).

## Overview

<!-- CUSTOMIZE: Rewrite this paragraph in your own words to explain why you built this project -->
When we type commands into a terminal, it is easy to forget that a shell is just an ordinary user-space C program orchestrating Linux system calls. I built `minishell` from scratch to understand how the Linux kernel creates processes, how programs communicate through in-memory pipes by manipulating the per-process File Descriptor Table, and how terminal job control manages foreground and background process groups.

---

## Core Linux Kernel Primitives Covered

### 1. Process Lifecycle (`fork()`, `execve()`, `waitpid()`)
<!-- CUSTOMIZE: Explain in your own words how fork clones the process and execve replaces its memory -->
* **`fork()`:** Clones the running shell process into an identical child process.
* **`execve()`:** Resolves the binary path across `$PATH` and replaces the child process's memory space with the target program while preserving its rewired File Descriptor Table.
* **`waitpid()`:** Used with `WUNTRACED` to block for foreground jobs (catching both exits and `Ctrl+Z` stops) and with `WNOHANG` to asynchronously reap completed background (`&`) jobs without freezing the shell prompt.

### 2. Per-Process File Descriptor Table Manipulation (`pipe()` & `dup2()`)
Every Linux process has a private File Descriptor (FD) table where `FD 0` is `stdin`, `FD 1` is `stdout`, and `FD 2` is `stderr`. Programs read from `FD 0` and write to `FD 1` without knowing what is attached on the other end.

In the brief window between `fork()` and `execve()`, `minishell` uses `dup2(oldfd, newfd)` to rewire the child's FD table:

```text
    Process 1 (cat main.c)               Kernel Memory               Process 2 (wc -l)
┌────────────────────────┐            ┌─────────────────┐          ┌────────────────────────┐
│ FD 0: Keyboard         │            │                 │          │ FD 0: Pipe Read End    │
│ FD 1: Pipe Write End   │───────────►│   Pipe Buffer   │─────────►│ FD 1: Screen / File    │
│ FD 2: Screen           │            │   (pipe(fd))    │          │ FD 2: Screen           │
└────────────────────────┘            └─────────────────┘          └────────────────────────┘
 (dup2(pipefd[1], 1))                                               (dup2(prev_read, 0))
```

* **Input/Output Redirection (`<`, `>`):** Opens the target file and calls `dup2(fd, STDIN_FILENO)` or `dup2(fd, STDOUT_FILENO)` before closing the temporary descriptor.
* **Error Redirection (`2>&1`):** Calls `dup2(STDOUT_FILENO, STDERR_FILENO)` so `FD 2` points to the exact same kernel file description as `FD 1`.

### 3. Job Control, Process Groups & Signals (`SIGINT`, `SIGTSTP`)
<!-- CUSTOMIZE: Explain in your own words why Ctrl+C kills the running command instead of killing your shell -->
To prevent `Ctrl+C` (`SIGINT`) or `Ctrl+Z` (`SIGTSTP`) from killing the shell itself:
* The main `minishell` process ignores `SIGINT`, `SIGTSTP`, `SIGTTIN`, and `SIGTTOU` (`SIG_IGN`), while child processes restore default signal handlers (`SIG_DFL`) before calling `execve()`.
* Every pipeline is placed into its own **Process Group** via `setpgid(pid, pgid)`.
* For foreground jobs, `minishell` hands terminal ownership to the child's process group using `tcsetpgrp(STDIN_FILENO, pgid)` and reclaims ownership once the job exits or stops.
* Built-in `jobs`, `fg <id>`, and `bg <id>` commands track stopped/background jobs and resume them using `kill(-pgid, SIGCONT)`.

---

## Project Structure

```text
minishell/
├── .gitignore    # Excludes compiled binary and test output files
├── main.c        # Complete shell implementation (parser, pipelines, redirection, job control)
├── Makefile      # Build and run automation using gcc
└── README.md     # Project documentation
```

---

## Getting Started

### Prerequisites
* Linux or Windows **WSL 2**
* `gcc` and `make` (`sudo apt install build-essential`)

### Build and Run
```bash
make run
```

---

## Feature Walkthrough & Demo

### 1. Multi-Stage Pipelines (`cmd1 | cmd2 | cmd3`)
```text
minishell> cat main.c | grep dup2 | wc -l
5
```

### 2. File Descriptor Redirection (`>`, `<`, `2>&1`)
```text
minishell> ls -l main.c /nonexistent > out.txt 2>&1
minishell> cat < out.txt
ls: cannot access '/nonexistent': No such file or directory
-rw-r--r-- 1 anshul anshul 7540 Oct 15 03:35 main.c
```

### 3. Background Processes (`&`) & Job Control (`Ctrl+Z`, `jobs`, `fg`, `Ctrl+C`)
```text
minishell> sleep 20 &
[1] 14502
minishell> jobs
[1] Running    (pgid: 14502)  sleep 20 &
minishell> sleep 30
^Z
[2] Stopped    sleep 30
minishell> fg 2
^C
minishell> exit
```