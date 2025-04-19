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

#define MAX_LINE 256
#define EVENT_SIZE  ( sizeof(struct inotify_event) )
#define EVENT_BUF_LEN (1024 * (EVENT_SIZE + 16))

// Δομή για πληροφορίες συγχρονισμού κάθε καταλόγου
typedef struct sync_info {
    char source_dir[256];
    char target_dir[256];
    char status[16];
    char last_sync_time[32];
    int error_count;
    int inotify_watch;
    struct sync_info *next;
} sync_info_t;
sync_info_t *sync_info_head = NULL;

// Δομή για task (εργασία) στην ουρά
typedef struct task {
    char source[256];
    char target[256];
    char filename[256];
    char operation[16];
    struct task *next;
} task_t;
task_t *task_queue_head = NULL;
task_t *task_queue_tail = NULL;

// Δομή για χαρτογράφηση worker PID -> pipe FD και source directory
typedef struct worker_pipe {
    pid_t pid;
    int fd;
    char source[256];
    struct worker_pipe *next;
} worker_pipe_t;
worker_pipe_t *worker_pipes = NULL;

// Παράμετροι worker
int worker_limit = 5;
int current_worker_count = 0;

// Αρχείο log του manager
FILE *log_fp = NULL;

// Named Pipes για επικοινωνία με fss_console
#define FIFO_IN "fss_in"
#define FIFO_OUT "fss_out"

// Συνάρτηση για πρόσβαση στην τρέχουσα ώρα σε μορφή "[%Y-%m-%d %H:%M:%S]"
void current_time_str(char *buffer, size_t size) {
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    strftime(buffer, size, "[%Y-%m-%d %H:%M:%S]", tm_info);
}

// Καταγραφή μηνυμάτων (έξοδος και log)
void log_message(const char *message) {
    char timebuf[32];
    current_time_str(timebuf, sizeof(timebuf));
    fprintf(stdout, "%s %s\n", timebuf, message);
    if(log_fp) {
        fprintf(log_fp, "%s %s\n", timebuf, message);
        fflush(log_fp);
    }
}

// Καθαρισμός παλαιών πόρων (named pipes)
void cleanup_resources() {
    unlink(FIFO_IN);
    unlink(FIFO_OUT);
}

// Προσθήκη εγγραφής στην δομή sync_info
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
    sync_info_t *cur = sync_info_head;
    while(cur) {
        if(strcmp(cur->source_dir, source) == 0)
            return cur;
        cur = cur->next;
    }
    return NULL;
}
void remove_sync_info(const char *source) {
    sync_info_t **curr = &sync_info_head;
    while(*curr) {
        if(strcmp((*curr)->source_dir, source) == 0) {
            sync_info_t *temp = *curr;
            *curr = (*curr)->next;
            free(temp);
            return;
        }
        curr = &((*curr)->next);
    }
}

// --- Λειτουργίες για task queue ---
void enqueue_task(const char *source, const char *target, const char *filename, const char *operation) {
    task_t *new_task = malloc(sizeof(task_t));
    strncpy(new_task->source, source, sizeof(new_task->source));
    strncpy(new_task->target, target, sizeof(new_task->target));
    strncpy(new_task->filename, filename, sizeof(new_task->filename));
    strncpy(new_task->operation, operation, sizeof(new_task->operation));
    new_task->next = NULL;
    if(task_queue_tail == NULL) {
        task_queue_head = new_task;
        task_queue_tail = new_task;
    } else {
        task_queue_tail->next = new_task;
        task_queue_tail = new_task;
    }
}
task_t* dequeue_task() {
    if(task_queue_head == NULL)
        return NULL;
    task_t *task = task_queue_head;
    task_queue_head = task_queue_head->next;
    if(task_queue_head == NULL)
        task_queue_tail = NULL;
    return task;
}
void process_task_queue() {
    while(current_worker_count < worker_limit) {
        task_t *task = dequeue_task();
        if(task == NULL)
            break;
        spawn_worker(task->source, task->target, task->filename, task->operation);
        free(task);
    }
}
// --- Τέλος task queue ---

// --- Λειτουργίες για worker_pipe ---
void add_worker_pipe(pid_t pid, int fd, const char *source) {
    worker_pipe_t *wp = malloc(sizeof(worker_pipe_t));
    wp->pid = pid;
    wp->fd = fd;
    strncpy(wp->source, source, sizeof(wp->source));
    wp->next = worker_pipes;
    worker_pipes = wp;
}
void remove_worker_pipe(pid_t pid) {
    worker_pipe_t **curr = &worker_pipes;
    while(*curr) {
        if((*curr)->pid == pid) {
            worker_pipe_t *temp = *curr;
            *curr = (*curr)->next;
            close(temp->fd);
            free(temp);
            return;
        }
        curr = &((*curr)->next);
    }
}
// --- Τέλος worker_pipe ---

// Συνάρτηση για ενημέρωση της εγγραφής στο sync_info βάσει του exec_report που έστειλε ο worker
void update_sync_info(const char *source, const char *exec_report) {
    sync_info_t *info = find_sync_info(source);
    if(info) {
        char timebuf[32];
        current_time_str(timebuf, sizeof(timebuf));
        strncpy(info->last_sync_time, timebuf, sizeof(info->last_sync_time));
        char *status_line = strstr(exec_report, "STATUS:");
        if(status_line) {
            char status_str[64];
            sscanf(status_line, "STATUS: %63s", status_str);
            if(strcmp(status_str, "ERROR") == 0 || strcmp(status_str, "PARTIAL") == 0) {
                info->error_count++;
            }
        }
    }
}

// Εκκίνηση worker με χρήση pipe για capture του exec_report
void spawn_worker(const char *source, const char *target, const char *filename, const char *operation) {
    if(current_worker_count >= worker_limit) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Max worker limit reached. Task queued: %s", source);
        log_message(msg);
        enqueue_task(source, target, filename, operation);
        return;
    }
    int pipefd[2];
    if(pipe(pipefd) < 0) {
        perror("pipe failed");
        return;
    }
    pid_t pid = fork();
    if(pid == 0) {
        // Child: ανακατευθύνουμε το stdout στο pipe write end
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
        execl("./worker", "worker", source, target, filename, operation, NULL);
        perror("execl failed");
        exit(EXIT_FAILURE);
    } else if(pid > 0) {
        close(pipefd[1]);
        current_worker_count++;
        add_worker_pipe(pid, pipefd[0], source);
        char msg[256];
        snprintf(msg, sizeof(msg), "Added directory: %s -> %s (Worker PID: %d)", source, target, pid);
        log_message(msg);
    } else {
        perror("fork failed");
    }
}

// SIGCHLD handler: ενημέρωση για τερματισμούς και επεξεργασία ουράς
void sigchld_handler(int signo) {
    int status;
    pid_t pid;
    while((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        current_worker_count--;
        char msg[256];
        snprintf(msg, sizeof(msg), "Worker %d finished.", pid);
        log_message(msg);
        remove_worker_pipe(pid);
        process_task_queue();
    }
}

int main(int argc, char *argv[]) {
    // Παράμετροι: -l <manager_logfile> -c <config_file> -n <worker_limit>
    char *logfile = NULL;
    char *config_file = NULL;
    int opt;
    while((opt = getopt(argc, argv, "l:c:n:")) != -1) {
        switch(opt) {
            case 'l': logfile = optarg; break;
            case 'c': config_file = optarg; break;
            case 'n': worker_limit = atoi(optarg); break;
            default:
                fprintf(stderr, "Usage: %s -l <manager_logfile> -c <config_file> -n <worker_limit>\n", argv[0]);
                exit(EXIT_FAILURE);
        }
    }
    if(!logfile || !config_file) {
        fprintf(stderr, "Logfile and config_file required\n");
        exit(EXIT_FAILURE);
    }
    cleanup_resources();
    log_fp = fopen(logfile, "a");
    if(!log_fp) {
        perror("Error opening log file");
        exit(EXIT_FAILURE);
    }
    struct sigaction sa;
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    if(sigaction(SIGCHLD, &sa, NULL) == -1) {
        perror("sigaction");
        exit(EXIT_FAILURE);
    }
    if(mkfifo(FIFO_IN, 0666) == -1 && errno != EEXIST) {
        perror("mkfifo FIFO_IN");
        exit(EXIT_FAILURE);
    }
    if(mkfifo(FIFO_OUT, 0666) == -1 && errno != EEXIST) {
        perror("mkfifo FIFO_OUT");
        exit(EXIT_FAILURE);
    }
    FILE *config_fp = fopen(config_file, "r");
    if(!config_fp) {
        perror("Error opening config file");
        exit(EXIT_FAILURE);
    }
    char line[MAX_LINE];
    while(fgets(line, MAX_LINE, config_fp)) {
        char source[256], target[256];
        if(sscanf(line, "%s %s", source, target) != 2)
            continue;
        if(find_sync_info(source)) {
            char msg[256];
            snprintf(msg, sizeof(msg), "Source %s already registered. New target rejected.", source);
            log_message(msg);
            continue;
        }
        add_sync_info(source, target);
        spawn_worker(source, target, "ALL", "FULL");
    }
    fclose(config_fp);
    int inotify_fd = inotify_init();
    if(inotify_fd < 0) {
        perror("inotify_init");
        exit(EXIT_FAILURE);
    }
    sync_info_t *cur = sync_info_head;
    while(cur) {
        int wd = inotify_add_watch(inotify_fd, cur->source_dir, IN_CREATE | IN_MODIFY | IN_DELETE);
        if(wd < 0) {
            perror("inotify_add_watch");
        } else {
            cur->inotify_watch = wd;
            char msg[256];
            snprintf(msg, sizeof(msg), "Monitoring started for %s", cur->source_dir);
            log_message(msg);
        }
        cur = cur->next;
    }
    int fifo_in_fd = open(FIFO_IN, O_RDONLY | O_NONBLOCK);
    int fifo_out_fd = open(FIFO_OUT, O_WRONLY);
    if(fifo_in_fd < 0 || fifo_out_fd < 0) {
        perror("Error opening FIFOs");
        exit(EXIT_FAILURE);
    }
    char buffer[1024];
    int running = 1;
    while(running) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(inotify_fd, &readfds);
        FD_SET(fifo_in_fd, &readfds);
        int maxfd = (inotify_fd > fifo_in_fd ? inotify_fd : fifo_in_fd);
        worker_pipe_t *wp;
        for(wp = worker_pipes; wp != NULL; wp = wp->next) {
            FD_SET(wp->fd, &readfds);
            if(wp->fd > maxfd)
                maxfd = wp->fd;
        }
        maxfd++;
        int ret = select(maxfd, &readfds, NULL, NULL, NULL);
        if(ret < 0) {
            if(errno == EINTR)
                continue;
            perror("select");
            break;
        }
        if(FD_ISSET(inotify_fd, &readfds)) {
            char event_buf[EVENT_BUF_LEN];
            int length = read(inotify_fd, event_buf, EVENT_BUF_LEN);
            if(length < 0) {
                perror("read inotify");
            } else {
                int i = 0;
                while(i < length) {
                    struct inotify_event *event = (struct inotify_event*)&event_buf[i];
                    sync_info_t *node = sync_info_head;
                    while(node) {
                        if(node->inotify_watch == event->wd)
                            break;
                        node = node->next;
                    }
                    if(node && event->len) {
                        char *operation = NULL;
                        if(event->mask & IN_CREATE) operation = "ADDED";
                        if(event->mask & IN_MODIFY) operation = "MODIFIED";
                        if(event->mask & IN_DELETE) operation = "DELETED";
                        if(operation)
                            spawn_worker(node->source_dir, node->target_dir, event->name, operation);
                    }
                    i += EVENT_SIZE + event->len;
                }
            }
        }
        if(FD_ISSET(fifo_in_fd, &readfds)) {
            int n = read(fifo_in_fd, buffer, sizeof(buffer)-1);
            if(n > 0) {
                buffer[n] = '\0';
                char command[16], arg1[256], arg2[256];
                int num_args = sscanf(buffer, "%s %s %s", command, arg1, arg2);
                if(num_args >= 1) {
                    if(strcmp(command, "add") == 0) {
                        if(find_sync_info(arg1)) {
                            char out_msg[256];
                            snprintf(out_msg, sizeof(out_msg), "Already in queue: %s", arg1);
                            write(fifo_out_fd, out_msg, strlen(out_msg));
                        } else {
                            add_sync_info(arg1, arg2);
                            spawn_worker(arg1, arg2, "ALL", "FULL");
                            char out_msg[256];
                            snprintf(out_msg, sizeof(out_msg), "Added directory: %s -> %s", arg1, arg2);
                            write(fifo_out_fd, out_msg, strlen(out_msg));
                        }
                    } else if(strcmp(command, "cancel") == 0) {
                        if(find_sync_info(arg1)) {
                            remove_sync_info(arg1);
                            char out_msg[256];
                            snprintf(out_msg, sizeof(out_msg), "Monitoring stopped for %s", arg1);
                            write(fifo_out_fd, out_msg, strlen(out_msg));
                        } else {
                            char out_msg[256];
                            snprintf(out_msg, sizeof(out_msg), "Directory not monitored: %s", arg1);
                            write(fifo_out_fd, out_msg, strlen(out_msg));
                        }
                    } else if(strcmp(command, "status") == 0) {
                        sync_info_t *info = find_sync_info(arg1);
                        char out_msg[512];
                        if(info) {
                            snprintf(out_msg, sizeof(out_msg),
                                     "Status requested for %s\nDirectory: %s\nTarget: %s\nLast Sync: %s\nErrors: %d\nStatus: %s",
                                     arg1, info->source_dir, info->target_dir, info->last_sync_time, info->error_count, info->status);
                        } else {
                            snprintf(out_msg, sizeof(out_msg), "Directory not monitored: %s", arg1);
                        }
                        write(fifo_out_fd, out_msg, strlen(out_msg));
                    } else if(strcmp(command, "sync") == 0) {
                        sync_info_t *info = find_sync_info(arg1);
                        if(info) {
                            spawn_worker(info->source_dir, info->target_dir, "ALL", "FULL");
                            char out_msg[256];
                            snprintf(out_msg, sizeof(out_msg), "Syncing directory: %s -> %s", info->source_dir, info->target_dir);
                            write(fifo_out_fd, out_msg, strlen(out_msg));
                        } else {
                            char out_msg[256];
                            snprintf(out_msg, sizeof(out_msg), "Directory not monitored: %s", arg1);
                            write(fifo_out_fd, out_msg, strlen(out_msg));
                        }
                    } else if(strcmp(command, "shutdown") == 0) {
                        write(fifo_out_fd, "Shutting down manager...", 26);
                        running = 0;
                    }
                }
            }
        }
        worker_pipe_t *curr_wp = worker_pipes;
        char wp_buffer[1024];
        while(curr_wp) {
            if(FD_ISSET(curr_wp->fd, &readfds)) {
                int r = read(curr_wp->fd, wp_buffer, sizeof(wp_buffer)-1);
                if(r > 0) {
                    wp_buffer[r] = '\0';
                    char msg[1024];
                    snprintf(msg, sizeof(msg), "Worker PID %d exec_report:\n%s", curr_wp->pid, wp_buffer);
                    log_message(msg);
                    update_sync_info(curr_wp->source, wp_buffer);
                }
            }
            curr_wp = curr_wp->next;
        }
    }
    log_message("Manager shutdown complete.");
    fclose(log_fp);
    close(fifo_in_fd);
    close(fifo_out_fd);
    cleanup_resources();
    return 0;
}
