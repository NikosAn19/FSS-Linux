/* worker.c */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>

#define BUFFER_SIZE 1024
#define MAX_ERRORS 1024

typedef struct {
    char file[256];
    char msg[128];
} error_item_t;

int copy_file(const char *src_path, const char *dest_path) {
    int src_fd = open(src_path, O_RDONLY);
    if (src_fd < 0) return -1;
    int dest_fd = open(dest_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (dest_fd < 0) {
        close(src_fd);
        return -1;
    }
    char buffer[BUFFER_SIZE];
    ssize_t bytes;
    while ((bytes = read(src_fd, buffer, BUFFER_SIZE)) > 0) {
        if (write(dest_fd, buffer, bytes) != bytes) {
            close(src_fd);
            close(dest_fd);
            return -1;
        }
    }
    close(src_fd);
    close(dest_fd);
    return (bytes < 0 ? -1 : 0);
}

void full_sync(const char *source_dir, const char *target_dir) {
    DIR *dir = opendir(source_dir);
    if (!dir) {
        perror("opendir");
        return;
    }
    struct dirent *entry;
    int file_count = 0;
    int error_count = 0;
    error_item_t errors[MAX_ERRORS];

    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        char src_path[512];
        char dest_path[512];
        snprintf(src_path, sizeof(src_path), "%s/%s", source_dir, entry->d_name);
        snprintf(dest_path, sizeof(dest_path), "%s/%s", target_dir, entry->d_name);
        if (copy_file(src_path, dest_path) == 0) {
            file_count++;
        } else {
            if (error_count < MAX_ERRORS) {
                strncpy(errors[error_count].file, entry->d_name, sizeof(errors[error_count].file));
                strncpy(errors[error_count].msg, strerror(errno), sizeof(errors[error_count].msg));
            }
            error_count++;
        }
    }
    closedir(dir);

    // Output exec_report
    printf("EXEC_REPORT_START\n");
    if (error_count == 0) {
        printf("STATUS: SUCCESS\n");
    } else if (file_count > 0) {
        printf("STATUS: PARTIAL\n");
    } else {
        printf("STATUS: ERROR\n");
    }
    printf("DETAILS: %d files copied, %d errors\n", file_count, error_count);
    if (error_count > 0) {
        printf("ERRORS:\n");
        int limit = error_count < MAX_ERRORS ? error_count : MAX_ERRORS;
        for (int i = 0; i < limit; i++) {
            printf("- File %s: %s\n", errors[i].file, errors[i].msg);
        }
    }
    printf("EXEC_REPORT_END\n");
    fflush(stdout);
    exit(error_count > 0 ? 1 : 0);
}

void single_file_op(const char *source_dir, const char *target_dir,
                    const char *filename, const char *operation) {
    char src_path[512];
    char dest_path[512];
    error_item_t err;
    int error_count = 0;

    if (strcmp(operation, "DELETED") == 0) {
        snprintf(dest_path, sizeof(dest_path), "%s/%s", target_dir, filename);
        if (unlink(dest_path) != 0) {
            error_count = 1;
            strncpy(err.file, filename, sizeof(err.file));
            strncpy(err.msg, strerror(errno), sizeof(err.msg));
        }
    } else {
        snprintf(src_path, sizeof(src_path), "%s/%s", source_dir, filename);
        snprintf(dest_path, sizeof(dest_path), "%s/%s", target_dir, filename);
        if (copy_file(src_path, dest_path) != 0) {
            error_count = 1;
            strncpy(err.file, filename, sizeof(err.file));
            strncpy(err.msg, strerror(errno), sizeof(err.msg));
        }
    }

    // Output exec_report
    printf("EXEC_REPORT_START\n");
    if (error_count == 0) {
        printf("STATUS: SUCCESS\n");
    } else {
        printf("STATUS: ERROR\n");
    }
    if (strcmp(operation, "DELETED") == 0) {
        if (error_count == 0)
            printf("DETAILS: File %s deleted.\n", filename);
        else
            printf("DETAILS: File %s deletion failed.\n", filename);
    } else {
        if (error_count == 0)
            printf("DETAILS: File %s copied.\n", filename);
        else
            printf("DETAILS: File %s copy failed.\n", filename);
    }
    if (error_count > 0) {
        printf("ERRORS:\n");
        printf("- File %s: %s\n", err.file, err.msg);
    }
    printf("EXEC_REPORT_END\n");
    fflush(stdout);
    exit(error_count > 0 ? 1 : 0);
}

int main(int argc, char *argv[]) {
    if (argc != 5) {
        fprintf(stderr, "Usage: %s <source_dir> <target_dir> <filename> <operation>\n", argv[0]);
        exit(EXIT_FAILURE);
    }
    const char *source_dir = argv[1];
    const char *target_dir = argv[2];
    const char *filename   = argv[3];
    const char *operation  = argv[4];

    if (strcmp(operation, "FULL") == 0 && strcmp(filename, "ALL") == 0) {
        full_sync(source_dir, target_dir);
    } else {
        single_file_op(source_dir, target_dir, filename, operation);
    }
    return 0;
}
