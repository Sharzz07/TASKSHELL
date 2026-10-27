#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <dirent.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/resource.h>
#include <sys/time.h>

#define MAX_ARGS  64
#define MAX_STEPS 32

typedef struct {
    char cmd[256];
    char policy[16];   /* stop | continue | retry */
    int  retries;
    int  timeout;      /* wall-clock limit in seconds, 0 = none */
    int  cpu;          /* CPU-time limit in seconds, 0 = none */
    int  mem;          /* address-space limit in MB, 0 = none */
    char id[32];       /* name of this step, "-" if none */
    char after[128];   /* comma-separated ids this step waits for, "-" if none */
} Step;

typedef struct {
    int timeout;       /* seconds */
    int cpu;           /* seconds */
    int mem;           /* MB */
    int isolate;       /* 1 = run in its own process group (task steps) */
} Limits;

typedef struct {
    int   exit_code;
    int   timed_out;
    int   cpu_limit;   /* killed by the kernel for exceeding the CPU limit */
    pid_t pid;
    long  ms;          /* wall-clock time */
    long  cpu_ms;      /* user + system CPU time, from the kernel */
    long  rss_kb;      /* peak resident memory, from the kernel */
} Result;

static char BASE[512];                 /* directory where the shell started */
static volatile pid_t g_pgid = 0;      /* process group of the running step */

/* ---------- Ctrl+C handling ---------- */

/* The shell survives Ctrl+C. If a task step is running, the signal is
   forwarded to that step's whole process group. */
static void on_sigint(int sig) {
    (void)sig;
    pid_t p = g_pgid;
    if (p > 0) killpg(p, SIGINT);
}

/* ---------- dependency helpers ---------- */

/* index of the step with this id, or -1 */
static int find_step(Step *steps, int n, const char *id) {
    if (strcmp(id, "-") == 0) return -1;
    for (int i = 0; i < n; i++)
        if (strcmp(steps[i].id, id) == 0) return i;
    return -1;
}

/*
 * Decide the order to run steps (topological sort, Kahn style):
 * repeatedly pick the first not-yet-done step whose dependencies are all done.
 * Returns number of steps ordered (== n on success),
 *         -1 if there is a cycle, -2 if a dependency id does not exist.
 */
static int plan_order(Step *steps, int n, int *order) {
    int done[MAX_STEPS] = {0};
    int count = 0;

    while (count < n) {
        int progress = 0;
        for (int i = 0; i < n; i++) {
            if (done[i]) continue;
            int ready = 1;
            if (strcmp(steps[i].after, "-") != 0) {
                char buf[128];
                snprintf(buf, sizeof buf, "%s", steps[i].after);
                for (char *d = strtok(buf, ","); d != NULL; d = strtok(NULL, ",")) {
                    int j = find_step(steps, n, d);
                    if (j < 0) {
                        printf("  Error: step %d depends on unknown id '%s'.\n", i + 1, d);
                        return -2;
                    }
                    if (!done[j]) ready = 0;
                }
            }
            if (ready) {
                order[count++] = i;
                done[i] = 1;
                progress = 1;
                break;                 /* restart scan from the first step */
            }
        }
        if (!progress) {               /* nothing can run -> cycle */
            printf("  Error: dependency cycle detected among:");
            for (int i = 0; i < n; i++)
                if (!done[i]) printf(" %d(%s)", i + 1, steps[i].id);
            printf("\n");
            return -1;
        }
    }
    return count;
}

/* ---------- helpers ---------- */

static int tokenize(char *s, char **argv, int max) {
    int argc = 0;
    while (*s && argc < max - 1) {
        while (*s == ' ' || *s == '\t' || *s == '\n') s++;
        if (!*s) break;
        if (*s == '"') {                 /* quoted -> one token */
            s++;
            argv[argc++] = s;
            while (*s && *s != '"') s++;
        } else {
            argv[argc++] = s;
            while (*s && *s != ' ' && *s != '\t' && *s != '\n') s++;
        }
        if (*s) *s++ = '\0';
    }
    argv[argc] = NULL;
    return argc;
}

static long ms_between(struct timespec a, struct timespec b) {
    return (b.tv_sec - a.tv_sec) * 1000L + (b.tv_nsec - a.tv_nsec) / 1000000L;
}

static int valid_name(const char *n) {
    size_t len = strlen(n);
    if (len == 0 || len > 60) return 0;
    for (size_t i = 0; i < len; i++) {
        char c = n[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-'))
            return 0;
    }
    return 1;
}

/* list of ids separated by commas, e.g. "compile,test" */
static int valid_after(const char *s) {
    size_t len = strlen(s);
    if (len == 0 || len > 127) return 0;
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-' || c == ','))
            return 0;
    }
    return 1;
}

static void task_path(const char *name, char *out, size_t n) {
    snprintf(out, n, "%s/tasks/%s.task", BASE, name);
}

static void state_path(const char *name, char *out, size_t n) {
    snprintf(out, n, "%s/state/%s.state", BASE, name);
}

/* ---------- running a process (fork + exec + wait + limits) ---------- */

/* send a signal to the step: to its whole process group if isolated */
static void signal_step(pid_t pid, int sig, int isolate) {
    if (isolate) killpg(pid, sig);
    else         kill(pid, sig);
}

static Result run_process(char **argv, Limits lim) {
    Result r;
    memset(&r, 0, sizeof r);
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    pid_t pid = fork();
    if (pid < 0) { perror("fork"); r.exit_code = -1; return r; }

    if (pid == 0) {                                   /* CHILD */
        signal(SIGINT, SIG_DFL);
        if (lim.isolate) {
            setpgid(0, 0);                            /* new process group */
            int fd = open("/dev/null", O_RDONLY);     /* steps never read the shell's input */
            if (fd >= 0) { dup2(fd, 0); if (fd > 2) close(fd); }
        }
        if (lim.cpu > 0) {                            /* CPU-time limit enforced by the kernel */
            struct rlimit rl = { (rlim_t)lim.cpu, (rlim_t)lim.cpu + 1 };
            setrlimit(RLIMIT_CPU, &rl);
        }
        if (lim.mem > 0) {                            /* address-space limit */
            rlim_t bytes = (rlim_t)lim.mem * 1024 * 1024;
            struct rlimit rl = { bytes, bytes };
            setrlimit(RLIMIT_AS, &rl);
        }
        execvp(argv[0], argv);
        perror(argv[0]);
        _exit(127);
    }

    if (lim.isolate) {                                /* PARENT */
        setpgid(pid, pid);                            /* avoids a race with the child */
        g_pgid = pid;
    }
    r.pid = pid;

    int status = 0;
    struct rusage ru;
    memset(&ru, 0, sizeof ru);
    int reaped = 0;

    if (lim.timeout <= 0) {
        if (wait4(pid, &status, 0, &ru) == pid) reaped = 1;
    } else {
        while (1) {
            pid_t w = wait4(pid, &status, WNOHANG, &ru);
            if (w == pid || w < 0) { reaped = 1; break; }
            clock_gettime(CLOCK_MONOTONIC, &t1);
            if (ms_between(t0, t1) >= lim.timeout * 1000L) {
                signal_step(pid, SIGTERM, lim.isolate);        /* polite stop */
                usleep(300000);
                if (wait4(pid, &status, WNOHANG, &ru) == pid) reaped = 1;
                signal_step(pid, SIGKILL, lim.isolate);        /* force stop, whole group */
                if (!reaped && wait4(pid, &status, 0, &ru) == pid) reaped = 1;
                r.timed_out = 1;
                break;
            }
            usleep(20000);
        }
    }
    g_pgid = 0;

    clock_gettime(CLOCK_MONOTONIC, &t1);
    r.ms     = ms_between(t0, t1);
    r.cpu_ms = ru.ru_utime.tv_sec * 1000L + ru.ru_utime.tv_usec / 1000L
             + ru.ru_stime.tv_sec * 1000L + ru.ru_stime.tv_usec / 1000L;
    r.rss_kb = ru.ru_maxrss;                          /* kilobytes on Linux */

    if (r.timed_out) {
        r.exit_code = 124;
    } else if (!reaped) {
        r.exit_code = -1;
    } else if (WIFEXITED(status)) {
        r.exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        r.exit_code = 128 + WTERMSIG(status);
        if (WTERMSIG(status) == SIGXCPU) r.cpu_limit = 1;
    } else {
        r.exit_code = -1;
    }
    return r;
}

static Result run_step(const char *cmd, Limits lim) {
    Result r;
    memset(&r, 0, sizeof r);
    char buf[256];
    snprintf(buf, sizeof buf, "%s", cmd);
    char *argv[MAX_ARGS];
    int argc = tokenize(buf, argv, MAX_ARGS);
    if (argc == 0) return r;
    if (strcmp(argv[0], "cd") == 0) {
        if (argc < 2 || chdir(argv[1]) != 0) { perror("cd"); r.exit_code = 1; }
        return r;
    }
    return run_process(argv, lim);
}

static const char *status_name(Result r) {
    if (r.timed_out)       return "TIMEOUT";
    if (r.cpu_limit)       return "CPU-LIMIT";
    return r.exit_code == 0 ? "OK" : "FAILED";
}

/* ---------- logging ---------- */

static void log_step(const char *mode, const char *task, int idx, int attempt,
                     const char *cmd, Result r) {
    char path[700];
    snprintf(path, sizeof path, "%s/logs/history.log", BASE);
    FILE *f = fopen(path, "a");
    if (!f) return;
    char ts[32];
    time_t now = time(NULL);
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", localtime(&now));
    fprintf(f, "%s | %s | %s | step %d (try %d) | pid %d | %s | exit %d | wall %ld ms | cpu %ld ms | mem %ld KB | %s\n",
            ts, mode, task, idx, attempt, (int)r.pid, status_name(r), r.exit_code,
            r.ms, r.cpu_ms, r.rss_kb, cmd);
    fclose(f);
}

static void log_skip(const char *mode, const char *task, int idx, const char *cmd) {
    char path[700];
    snprintf(path, sizeof path, "%s/logs/history.log", BASE);
    FILE *f = fopen(path, "a");
    if (!f) return;
    char ts[32];
    time_t now = time(NULL);
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", localtime(&now));
    fprintf(f, "%s | %s | %s | step %d | SKIPPED (dependency failed) | %s\n",
            ts, mode, task, idx, cmd);
    fclose(f);
}

static void task_history(void) {
    char path[700];
    snprintf(path, sizeof path, "%s/logs/history.log", BASE);
    FILE *f = fopen(path, "r");
    if (!f) { printf("No history yet.\n"); return; }
    char line[700];
    while (fgets(line, sizeof line, f)) fputs(line, stdout);
    fclose(f);
}

/* ---------- checkpoints (crash recovery) ---------- */
/* state/NAME.state:  first line "STEPS n", then one "OK k" line for every step
   that finished successfully. Each line is flushed to disk (fsync) at once, so
   it survives even if the shell is killed with SIGKILL. */

static void ckpt_begin(const char *name, int nsteps) {
    char path[700];
    state_path(name, path, sizeof path);
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "STEPS %d\n", nsteps);
    fflush(f);
    fsync(fileno(f));
    fclose(f);
}

static void ckpt_mark_ok(const char *name, int step_no) {
    char path[700];
    state_path(name, path, sizeof path);
    FILE *f = fopen(path, "a");
    if (!f) return;
    fprintf(f, "OK %d\n", step_no);
    fflush(f);
    fsync(fileno(f));
    fclose(f);
}

/* returns 0 on success, -1 if no checkpoint exists, -2 if the task changed */
static int ckpt_load(const char *name, int nsteps, int *done) {
    char path[700];
    state_path(name, path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[128];
    int steps = -1;
    if (!fgets(line, sizeof line, f) || sscanf(line, "STEPS %d", &steps) != 1 || steps != nsteps) {
        fclose(f);
        return -2;
    }
    while (fgets(line, sizeof line, f)) {
        int k;
        if (sscanf(line, "OK %d", &k) == 1 && k >= 1 && k <= nsteps) done[k - 1] = 1;
    }
    fclose(f);
    return 0;
}

static void ckpt_clear(const char *name) {
    char path[700];
    state_path(name, path, sizeof path);
    unlink(path);
}

/* ---------- task storage ---------- */
/* file line format:  policy retries timeout cpu mem id after command */

static int load_task(const char *name, Step *steps) {
    char path[700];
    task_path(name, path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[700];
    int n = 0;
    while (n < MAX_STEPS && fgets(line, sizeof line, f)) {
        Step *s = &steps[n];
        int pos = 0;
        if (sscanf(line, "%15s %d %d %d %d %31s %127s %n",
                   s->policy, &s->retries, &s->timeout, &s->cpu, &s->mem,
                   s->id, s->after, &pos) < 7)
            continue;
        line[strcspn(line, "\n")] = '\0';
        if (pos > (int)strlen(line)) continue;
        snprintf(s->cmd, sizeof s->cmd, "%s", line + pos);
        n++;
    }
    fclose(f);
    return n;
}

static void task_create(const char *name) {
    if (!valid_name(name)) { printf("Invalid name (use letters, digits, _ or -).\n"); return; }
    char path[700];
    task_path(name, path, sizeof path);
    if (access(path, F_OK) == 0) { printf("Task '%s' already exists.\n", name); return; }
    FILE *f = fopen(path, "w");
    if (!f) { perror("create"); return; }
    fclose(f);
    printf("Created task '%s'.\n", name);
}

static void task_add(int argc, char **argv) {
    if (argc < 4) {
        printf("Usage: task add <name> \"<command>\" [--id ID] [--after ID1,ID2] [--retry N]\n"
               "                                   [--continue] [--timeout S] [--cpu S] [--mem MB]\n");
        return;
    }
    const char *name = argv[2];
    const char *cmd  = argv[3];
    if (!valid_name(name) || strlen(cmd) == 0 || strlen(cmd) > 250) {
        printf("Invalid task name or command.\n");
        return;
    }
    char path[700];
    task_path(name, path, sizeof path);
    if (access(path, F_OK) != 0) { printf("No such task. Run: task create %s\n", name); return; }

    char policy[16] = "stop";
    char id[32] = "-";
    char after[128] = "-";
    int retries = 0, timeout = 0, cpu = 0, mem = 0;

    for (int i = 4; i < argc; i++) {
        if (strcmp(argv[i], "--continue") == 0) {
            strcpy(policy, "continue");
        } else if (strcmp(argv[i], "--retry") == 0 && i + 1 < argc) {
            strcpy(policy, "retry");
            retries = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--timeout") == 0 && i + 1 < argc) {
            timeout = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--cpu") == 0 && i + 1 < argc) {
            cpu = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--mem") == 0 && i + 1 < argc) {
            mem = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--id") == 0 && i + 1 < argc) {
            const char *v = argv[++i];
            if (!valid_name(v) || strlen(v) > 31 || strcmp(v, "-") == 0) {
                printf("Invalid id (letters, digits, _ or -, max 31 chars).\n");
                return;
            }
            strcpy(id, v);
        } else if (strcmp(argv[i], "--after") == 0 && i + 1 < argc) {
            const char *v = argv[++i];
            if (!valid_after(v)) {
                printf("Invalid --after list (use ids separated by commas, no spaces).\n");
                return;
            }
            strcpy(after, v);
        } else {
            printf("Unknown option: %s\n", argv[i]);
            return;
        }
    }
    if (retries < 0)  retries = 0;
    if (retries > 10) retries = 10;
    if (timeout < 0)  timeout = 0;
    if (cpu < 0)      cpu = 0;
    if (mem < 0)      mem = 0;
    if (mem > 65536)  mem = 65536;

    Step existing[MAX_STEPS];
    int n = load_task(name, existing);
    if (n >= MAX_STEPS) { printf("Task is full (max %d steps).\n", MAX_STEPS); return; }
    if (n > 0 && find_step(existing, n, id) >= 0) {
        printf("A step with id '%s' already exists in this task.\n", id);
        return;
    }

    FILE *f = fopen(path, "a");
    if (!f) { perror("add"); return; }
    fprintf(f, "%s %d %d %d %d %s %s %s\n", policy, retries, timeout, cpu, mem, id, after, cmd);
    fclose(f);
    printf("Added step to '%s'.\n", name);
}

static void task_show(const char *name) {
    Step steps[MAX_STEPS];
    int n = load_task(name, steps);
    if (n < 0) { printf("No such task: %s\n", name); return; }
    printf("Task '%s' (%d step(s)):\n", name, n);
    for (int i = 0; i < n; i++) {
        printf("  %d. %s   [id=%s after=%s on-fail=%s retries=%d timeout=%ds cpu=%ds mem=%dMB]\n",
               i + 1, steps[i].cmd, steps[i].id, steps[i].after,
               steps[i].policy, steps[i].retries, steps[i].timeout,
               steps[i].cpu, steps[i].mem);
    }
}

static void task_delete(const char *name) {
    char path[700];
    task_path(name, path, sizeof path);
    if (unlink(path) == 0) { ckpt_clear(name); printf("Deleted '%s'.\n", name); }
    else printf("No such task: %s\n", name);
}

static void task_list(void) {
    char dir[700];
    snprintf(dir, sizeof dir, "%s/tasks", BASE);
    DIR *d = opendir(dir);
    if (!d) { perror("tasks"); return; }
    struct dirent *e;
    int count = 0;
    while ((e = readdir(d)) != NULL) {
        size_t len = strlen(e->d_name);
        if (len > 5 && strcmp(e->d_name + len - 5, ".task") == 0) {
            printf("  %.*s\n", (int)(len - 5), e->d_name);
            count++;
        }
    }
    closedir(d);
    if (count == 0) printf("  (no tasks yet)\n");
}

/* ---------- running a whole task ---------- */

static void print_stats(Result r) {
    printf("(wall %ld ms, cpu %ld ms, mem %ld KB)", r.ms, r.cpu_ms, r.rss_kb);
}

static void run_task(const char *name, const char *mode, int resume) {
    Step steps[MAX_STEPS];
    int n = load_task(name, steps);
    if (n < 0) { printf("No such task: %s\n", name); return; }
    if (n == 0) { printf("Task '%s' has no steps.\n", name); return; }

    int order[MAX_STEPS];
    if (plan_order(steps, n, order) < 0) {
        printf("Task '%s' was not run.\n", name);
        return;
    }

    int restored_flag[MAX_STEPS] = {0};
    int restored = 0;
    if (resume) {
        int rc = ckpt_load(name, n, restored_flag);
        if (rc == -1) { printf("Nothing to resume for '%s' (no checkpoint).\n", name); return; }
        if (rc == -2) {
            printf("Task '%s' changed since the checkpoint. Run it again with: task run %s\n", name, name);
            return;
        }
    } else {
        ckpt_begin(name, n);
    }

    printf("[%s] %s: %d step(s)\n", mode, name, n);
    int state[MAX_STEPS] = {0};      /* 0 = not run, 1 = ok, 2 = failed, 3 = skipped */
    int ok = 0, failed = 0, skipped = 0;

    for (int k = 0; k < n; k++) {
        int i = order[k];
        Step *s = &steps[i];

        /* finished in an earlier, interrupted run: do not run it again */
        if (resume && restored_flag[i]) {
            printf("  step %d: %s\n    -> RESTORED (finished before the interruption, not run again)\n",
                   i + 1, s->cmd);
            state[i] = 1;
            restored++;
            continue;
        }

        /* skip this step if any dependency did not succeed */
        char blocker[128] = "";
        if (strcmp(s->after, "-") != 0) {
            char buf[128];
            snprintf(buf, sizeof buf, "%s", s->after);
            for (char *d = strtok(buf, ","); d != NULL; d = strtok(NULL, ",")) {
                int j = find_step(steps, n, d);
                if (j >= 0 && state[j] != 1) {
                    snprintf(blocker, sizeof blocker, "%s", d);
                    break;
                }
            }
        }
        if (blocker[0] != '\0') {
            printf("  step %d: %s\n    -> SKIPPED (dependency '%s' did not succeed)\n",
                   i + 1, s->cmd, blocker);
            log_skip(mode, name, i + 1, s->cmd);
            state[i] = 3;
            skipped++;
            continue;
        }

        int tries = (strcmp(s->policy, "retry") == 0) ? s->retries + 1 : 1;
        int success = 0;
        Limits lim = { s->timeout, s->cpu, s->mem, 1 };

        for (int a = 1; a <= tries; a++) {
            printf("  step %d: %s", i + 1, s->cmd);
            if (tries > 1) printf("   (try %d/%d)", a, tries);
            printf("\n");

            Result r = run_step(s->cmd, lim);
            log_step(mode, name, i + 1, a, s->cmd, r);

            if (r.exit_code == 0) {
                printf("    -> OK ");
                print_stats(r);
                printf("\n");
                success = 1;
                break;
            }
            printf("    -> %s, exit %d ", status_name(r), r.exit_code);
            print_stats(r);
            printf("\n");
            if (a < tries) sleep(1);      /* small pause before retrying */
        }

        if (success) {
            state[i] = 1;
            ok++;
            ckpt_mark_ok(name, i + 1);    /* recorded only after the step really succeeded */
        } else {
            state[i] = 2;
            failed++;
            if (strcmp(s->policy, "continue") != 0) {
                printf("  Step %d failed -> aborting task.\n", i + 1);
                break;
            }
            printf("  Step %d failed -> continuing (policy: continue).\n", i + 1);
        }
    }
    printf("Done: %d ok, %d failed, %d skipped", ok, failed, skipped);
    if (resume) printf(", %d restored", restored);
    printf(".\n");
    if (failed == 0 && skipped == 0) ckpt_clear(name);   /* nothing left to resume */
}

/* ---------- command dispatch ---------- */

static void print_help(void) {
    printf("Commands:\n"
           "  task create <name>\n"
           "  task add <name> \"<cmd>\" [--id ID] [--after ID1,ID2] [--retry N] [--continue]\n"
           "                          [--timeout S] [--cpu S] [--mem MB]\n"
           "  task list | task show <name> | task delete <name>\n"
           "  task run <name> | task replay <name> | task resume <name> | task history\n"
           "  cd <dir> | help | exit   (anything else runs as a normal command)\n");
}

static void handle_task(int argc, char **argv) {
    if (argc < 2) { print_help(); return; }
    const char *sub = argv[1];
    if      (strcmp(sub, "create") == 0 && argc >= 3) task_create(argv[2]);
    else if (strcmp(sub, "add") == 0)                 task_add(argc, argv);
    else if (strcmp(sub, "list") == 0)                task_list();
    else if (strcmp(sub, "show") == 0 && argc >= 3)   task_show(argv[2]);
    else if (strcmp(sub, "delete") == 0 && argc >= 3) task_delete(argv[2]);
    else if (strcmp(sub, "run") == 0 && argc >= 3)    run_task(argv[2], "RUN", 0);
    else if (strcmp(sub, "replay") == 0 && argc >= 3) run_task(argv[2], "REPLAY", 0);
    else if (strcmp(sub, "resume") == 0 && argc >= 3) run_task(argv[2], "RESUME", 1);
    else if (strcmp(sub, "history") == 0)             task_history();
    else print_help();
}

static void run_line(char *line) {
    char *argv[MAX_ARGS];
    int argc = tokenize(line, argv, MAX_ARGS);
    if (argc == 0) return;

    if (strcmp(argv[0], "exit") == 0) exit(0);
    if (strcmp(argv[0], "help") == 0) { print_help(); return; }
    if (strcmp(argv[0], "cd") == 0) {
        if (argc < 2 || chdir(argv[1]) != 0) perror("cd");
        return;
    }
    if (strcmp(argv[0], "task") == 0) { handle_task(argc, argv); return; }

    Limits none = {0, 0, 0, 0};          /* ordinary commands: no limits, same process group */
    run_process(argv, none);
}

int main(void) {
    if (!getcwd(BASE, sizeof BASE)) { perror("getcwd"); return 1; }
    char d[700];
    snprintf(d, sizeof d, "%s/tasks", BASE); mkdir(d, 0755);
    snprintf(d, sizeof d, "%s/logs", BASE);  mkdir(d, 0755);
    snprintf(d, sizeof d, "%s/state", BASE); mkdir(d, 0755);

    setvbuf(stdout, NULL, _IOLBF, 0);    /* output appears line by line, even if the shell is killed */

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGINT, &sa, NULL);        /* Ctrl+C never kills the shell */

    printf("TaskShell v2 - type 'help' for commands.\n");
    char *line = NULL;
    size_t cap = 0;
    while (1) {
        printf("TASKSHELL> ");
        fflush(stdout);
        if (getline(&line, &cap, stdin) == -1) break;
        run_line(line);
    }
    free(line);
    printf("\n");
    return 0;
}
