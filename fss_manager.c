/* fss_manager.c */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <signal.h>
#include <sys/inotify.h>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <errno.h>
#include <time.h>

#define MAX_LINE      256
#define EVENT_SIZE    (sizeof(struct inotify_event))
#define EVENT_BUF_LEN (1024 * (EVENT_SIZE + 16))

// Forward declaration
void spawn_worker(const char *source, const char *target, const char *filename, const char *operation);

// --- sync_info structure ---
typedef struct sync_info {
    char source_dir[256];
    char target_dir[256];
    char status[16];
    char last_sync_time[32];
    int error_count;
    int inotify_watch;
    struct sync_info *next;
} sync_info_t;
static sync_info_t *sync_info_head = NULL;

// --- task queue ---
typedef struct task {
    char source[256];
    char target[256];
    char filename[256];
    char operation[16];
    struct task *next;
} task_t;
static task_t *task_queue_head = NULL, *task_queue_tail = NULL;

void enqueue_task(const char *source, const char *target, const char *filename, const char *operation) {
    task_t *new_task = malloc(sizeof(task_t));
    strncpy(new_task->source, source, sizeof(new_task->source));
    strncpy(new_task->target, target, sizeof(new_task->target));
    strncpy(new_task->filename, filename, sizeof(new_task->filename));
    strncpy(new_task->operation, operation, sizeof(new_task->operation));
    new_task->next = NULL;
    if (!task_queue_tail) {
        task_queue_head = task_queue_tail = new_task;
    } else {
        task_queue_tail->next = new_task;
        task_queue_tail = new_task;
    }
}
task_t* dequeue_task() {
    if (!task_queue_head) return NULL;
    task_t *t = task_queue_head;
    task_queue_head = t->next;
    if (!task_queue_head) task_queue_tail = NULL;
    return t;
}
void process_task_queue() {
    extern int current_worker_count, worker_limit;
    while (current_worker_count < worker_limit) {
        task_t *t = dequeue_task();
        if (!t) break;
        spawn_worker(t->source, t->target, t->filename, t->operation);
        free(t);
    }
}

// --- worker_pipe mapping ---
typedef struct worker_pipe {
    pid_t pid;
    int fd;
    char source[256];
    struct worker_pipe *next;
} worker_pipe_t;
static worker_pipe_t *worker_pipes = NULL;

void add_worker_pipe(pid_t pid, int fd, const char *source) {
    worker_pipe_t *wp = malloc(sizeof(worker_pipe_t));
    wp->pid = pid;
    wp->fd  = fd;
    strncpy(wp->source, source, sizeof(wp->source));
    wp->next = worker_pipes;
    worker_pipes = wp;
}
void remove_worker_pipe(pid_t pid) {
    worker_pipe_t **curr = &worker_pipes;
    while (*curr) {
        if ((*curr)->pid == pid) {
            worker_pipe_t *tmp = *curr;
            *curr = tmp->next;
            close(tmp->fd);
            free(tmp);
            return;
        }
        curr = &((*curr)->next);
    }
}

// --- sync_info helpers ---
void add_sync_info(const char *source, const char *target) {
    sync_info_t *node = malloc(sizeof(sync_info_t));
    strncpy(node->source_dir, source, sizeof(node->source_dir));
    strncpy(node->target_dir, target, sizeof(node->target_dir));
    strcpy(node->status, "Active");
    strcpy(node->last_sync_time, "Never");
    node->error_count = 0;
    node->inotify_watch = -1;
    node->next = sync_info_head;
    sync_info_head = node;
}
sync_info_t *find_sync_info(const char *source) {
    for (sync_info_t *cur = sync_info_head; cur; cur = cur->next)
        if (strcmp(cur->source_dir, source) == 0)
            return cur;
    return NULL;
}
void remove_sync_info(const char *source) {
    sync_info_t **curr = &sync_info_head;
    while (*curr) {
        if (strcmp((*curr)->source_dir, source) == 0) {
            sync_info_t *tmp = *curr;
            *curr = tmp->next;
            free(tmp);
            return;
        }
        curr = &((*curr)->next);
    }
}

// update last_sync_time and error_count from exec_report
void update_sync_info(const char *source, const char *exec_report) {
    sync_info_t *info = find_sync_info(source);
    if (!info) return;
    char buf[32];
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", tm);
    strncpy(info->last_sync_time, buf, sizeof(info->last_sync_time));
    char *st = strstr(exec_report, "STATUS:");
    if (st) {
        char status_str[16];
        sscanf(st, "STATUS: %15s", status_str);
        if (strcmp(status_str, "ERROR")==0 || strcmp(status_str, "PARTIAL")==0)
            info->error_count++;
    }
}

// --- logging & utilities ---
static FILE *log_fp = NULL;
void current_time_str(char *buffer, size_t size) {
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    strftime(buffer, size, "[%Y-%m-%d %H:%M:%S]", tm);
}
void log_message(const char *message) {
    char tbuf[32];
    current_time_str(tbuf, sizeof(tbuf));
    fprintf(stdout, "%s %s\n", tbuf, message);
    if (log_fp) {
        fprintf(log_fp, "%s %s\n", tbuf, message);
        fflush(log_fp);
    }
}
void cleanup_resources() {
    unlink("fss_in");
    unlink("fss_out");
}

// worker process count
int worker_limit = 5;
int current_worker_count = 0;

// --- spawn_worker implementation ---
void spawn_worker(const char *source, const char *target, const char *filename, const char *operation) {
    if (current_worker_count >= worker_limit) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Max worker limit reached. Task queued: %s", source);
        log_message(msg);
        enqueue_task(source, target, filename, operation);
        return;
    }
    int pipefd[2];
    if (pipe(pipefd) < 0) {
        perror("pipe");
        return;
    }
    pid_t pid = fork();
    if (pid == 0) {
        // child
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
        execl("./worker", "worker", source, target, filename, operation, NULL);
        perror("execl");
        _exit(1);
    } else if (pid > 0) {
        close(pipefd[1]);
        current_worker_count++;
        add_worker_pipe(pid, pipefd[0], source);
        char msg[256];
        snprintf(msg, sizeof(msg), "Added directory: %s -> %s (Worker PID: %d)", source, target, pid);
        log_message(msg);
    } else {
        perror("fork");
    }
}

// --- SIGCHLD handler ---
void sigchld_handler(int signo) {
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        current_worker_count--;
        char msg[256];
        snprintf(msg, sizeof(msg), "Worker %d finished.", pid);
        log_message(msg);
        process_task_queue();
    }
}

// --- main ---
int main(int argc, char *argv[]) {
    char *logfile     = NULL;
    char *config_file = NULL;
    int opt;
    while ((opt = getopt(argc, argv, "l:c:n:")) != -1) {
        switch(opt) {
            case 'l': logfile     = optarg; break;
            case 'c': config_file = optarg; break;
            case 'n': worker_limit = atoi(optarg); break;
            default:
                fprintf(stderr, "Usage: %s -l <manager_logfile> -c <config_file> -n <worker_limit>\n", argv[0]);
                exit(1);
        }
    }
    if (!logfile || !config_file) {
        fprintf(stderr, "Logfile and config_file required\n");
        exit(1);
    }

    cleanup_resources();
    log_fp = fopen(logfile, "a");
    if (!log_fp) { perror("fopen log"); exit(1); }

    struct sigaction sa = { .sa_handler = sigchld_handler };
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGCHLD, &sa, NULL);

    // create FIFOs
    if (mkfifo("fss_in", 0666) && errno != EEXIST) { perror("mkfifo in"); exit(1); }
    if (mkfifo("fss_out",0666) && errno != EEXIST) { perror("mkfifo out"); exit(1); }

    // read config
    FILE *cf = fopen(config_file, "r");
    if (!cf) { perror("open config"); exit(1); }
    char line[MAX_LINE];
    while (fgets(line, sizeof(line), cf)) {
        char src[256], dst[256];
        if (sscanf(line, "%255s %255s", src, dst) != 2) continue;
        if (find_sync_info(src)) {
            char m[256];
            snprintf(m,sizeof(m),"Source %s already registered. New target rejected.", src);
            log_message(m);
            continue;
        }
        add_sync_info(src, dst);
        spawn_worker(src, dst, "ALL", "FULL");
    }
    fclose(cf);

    // setup inotify
    int inotify_fd = inotify_init();
    for (sync_info_t *si = sync_info_head; si; si = si->next) {
        int wd = inotify_add_watch(inotify_fd, si->source_dir, IN_CREATE|IN_MODIFY|IN_DELETE);
        if (wd < 0) perror("inotify_add_watch");
        else {
            si->inotify_watch = wd;
            char m[256];
            snprintf(m,sizeof(m),"Monitoring started for %s", si->source_dir);
            log_message(m);
        }
    }

    // open FIFOs
    int fifo_in_fd  = open("fss_in",  O_RDONLY | O_NONBLOCK);
    int fifo_out_fd;
    do {
        fifo_out_fd = open("fss_out", O_WRONLY | O_NONBLOCK);
        if (fifo_out_fd < 0 && (errno==ENXIO||errno==ENOENT)) usleep(100000);
    } while (fifo_out_fd < 0);

    // main loop
    char buf[1024];
    int running = 1;
    while (running) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(inotify_fd,  &rfds);
        FD_SET(fifo_in_fd,  &rfds);
        int maxfd = inotify_fd > fifo_in_fd ? inotify_fd : fifo_in_fd;
        for (worker_pipe_t *wp=worker_pipes; wp; wp=wp->next)
            if ((FD_SET(wp->fd,&rfds), wp->fd)>maxfd) maxfd=wp->fd;
        maxfd++;

        if (select(maxfd, &rfds, NULL, NULL, NULL) < 0) {
            if (errno==EINTR) continue;
            perror("select");
            break;
        }

        // inotify events
        if (FD_ISSET(inotify_fd, &rfds)) {
            char evbuf[EVENT_BUF_LEN];
            int len = read(inotify_fd, evbuf, sizeof(evbuf));
            for (int i=0; i<len; ) {
                struct inotify_event *e = (void*)(evbuf+i);
                for (sync_info_t *si=sync_info_head; si; si=si->next) {
                    if (si->inotify_watch==e->wd && e->len) {
                        const char *op = NULL;
                        if (e->mask&IN_CREATE) op="ADDED";
                        if (e->mask&IN_MODIFY) op="MODIFIED";
                        if (e->mask&IN_DELETE) op="DELETED";
                        if (op) spawn_worker(si->source_dir, si->target_dir, e->name, op);
                    }
                }
                i += EVENT_SIZE + e->len;
            }
        }

        // console commands
        if (FD_ISSET(fifo_in_fd, &rfds)) {
            int n = read(fifo_in_fd, buf, sizeof(buf)-1);
            if (n>0) {
                buf[n]='\0';
                char cmd[16], a1[256], a2[256];
                int na = sscanf(buf,"%15s %255s %255s",cmd,a1,a2);
                if      (!strcmp(cmd,"add")) {
                    if (find_sync_info(a1)) {
                        char out[256];
                        snprintf(out,sizeof(out),"Already in queue: %s",a1);
                        write(fifo_out_fd,out,strlen(out));
                    } else {
                        add_sync_info(a1,a2);
                        spawn_worker(a1,a2,"ALL","FULL");
                        char out[256];
                        snprintf(out,sizeof(out),"Added directory: %s -> %s",a1,a2);
                        write(fifo_out_fd,out,strlen(out));
                    }
                }
                else if (!strcmp(cmd,"cancel")) {
                    if (find_sync_info(a1)) {
                        remove_sync_info(a1);
                        char out[256];
                        snprintf(out,sizeof(out),"Monitoring stopped for %s",a1);
                        write(fifo_out_fd,out,strlen(out));
                    } else {
                        char out[256];
                        snprintf(out,sizeof(out),"Directory not monitored: %s",a1);
                        write(fifo_out_fd,out,strlen(out));
                    }
                }
                else if (!strcmp(cmd,"status")) {
                    sync_info_t *si = find_sync_info(a1);
                    char out[512];
                    if (si) {
                        snprintf(out,sizeof(out),
                            "Status requested for %s\nDirectory: %s\nTarget: %s\nLast Sync: %s\nErrors: %d\nStatus: %s",
                            a1, si->source_dir, si->target_dir, si->last_sync_time, si->error_count, si->status);
                    } else {
                        snprintf(out,sizeof(out),"Directory not monitored: %s",a1);
                    }
                    write(fifo_out_fd,out,strlen(out));
                }
                else if (!strcmp(cmd,"sync")) {
                    sync_info_t *si = find_sync_info(a1);
                    if (si) {
                        spawn_worker(si->source_dir, si->target_dir, "ALL", "FULL");
                        char out[256];
                        snprintf(out,sizeof(out),"Syncing directory: %s -> %s", si->source_dir, si->target_dir);
                        write(fifo_out_fd,out,strlen(out));
                    } else {
                        char out[256];
                        snprintf(out,sizeof(out),"Directory not monitored: %s",a1);
                        write(fifo_out_fd,out,strlen(out));
                    }
                }
                else if (!strcmp(cmd,"shutdown")) {
                    write(fifo_out_fd,"Shutting down manager...",24);
                    running = 0;
                }
            }
        }

        // worker exec_report pipes
        for (worker_pipe_t *wp=worker_pipes; wp; wp=wp->next) {
            if (FD_ISSET(wp->fd, &rfds)) {
                char wbuf[1024];
                int r = read(wp->fd, wbuf, sizeof(wbuf)-1);
                if (r>0) {
                    wbuf[r]='\0';
                    char msg[1024];
                    snprintf(msg,sizeof(msg),"Worker PID %d exec_report:\n%s", wp->pid, wbuf);
                    log_message(msg);
                    update_sync_info(wp->source, wbuf);
                }
                else if (r==0) {
                    // EOF: remove pipe mapping
                    remove_worker_pipe(wp->pid);
                }
            }
        }
    }

    log_message("Manager shutdown complete.");
    fclose(log_fp);
    close(fifo_in_fd);
    close(fifo_out_fd);
    cleanup_resources();
    return 0;
}
