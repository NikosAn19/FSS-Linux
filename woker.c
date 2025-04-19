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

// Συνάρτηση για αντιγραφή αρχείου χρησιμοποιώντας χαμηλού επιπέδου I/O
int copy_file(const char *src_path, const char *dest_path) {
    int src_fd = open(src_path, O_RDONLY);
    if(src_fd < 0) {
        return -1;
    }
    int dest_fd = open(dest_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if(dest_fd < 0) {
        close(src_fd);
        return -1;
    }
    char buffer[BUFFER_SIZE];
    ssize_t bytes;
    while((bytes = read(src_fd, buffer, BUFFER_SIZE)) > 0) {
        if(write(dest_fd, buffer, bytes) != bytes) {
            close(src_fd);
            close(dest_fd);
            return -1;
        }
    }
    close(src_fd);
    close(dest_fd);
    return 0;
}

// Υλοποίηση πλήρους συγχρονισμού: αντιγράφει όλα τα αρχεία από source στο target
void full_sync(const char *source_dir, const char *target_dir) {
    DIR *dir = opendir(source_dir);
    if(!dir) {
        perror("opendir");
        return;
    }
    struct dirent *entry;
    int file_count = 0, error_count = 0;
    while((entry = readdir(dir)) != NULL) {
        // Παραλείπουμε τα "." και ".."
        if(strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        char src_path[512];
        char dest_path[512];
        snprintf(src_path, sizeof(src_path), "%s/%s", source_dir, entry->d_name);
        snprintf(dest_path, sizeof(dest_path), "%s/%s", target_dir, entry->d_name);
        if(copy_file(src_path, dest_path) == 0)
            file_count++;
        else
            error_count++;
    }
    closedir(dir);

    // Παραγωγή exec_report
    printf("EXEC_REPORT_START\n");
    if(error_count == 0)
        printf("STATUS: SUCCESS\n");
    else if(file_count > 0)
        printf("STATUS: PARTIAL\n");
    else
        printf("STATUS: ERROR\n");
    printf("DETAILS: %d files copied, %d errors\n", file_count, error_count);
    if(error_count > 0) {
        printf("ERRORS:\n");
        printf("- Error copying some files.\n");
    }
    printf("EXEC_REPORT_END\n");
    fflush(stdout);
}

int main(int argc, char *argv[]) {
    // Αναμενόμενα ορίσματα: source_dir, target_dir, filename, operation
    if(argc != 5) {
        fprintf(stderr, "Usage: %s <source_dir> <target_dir> <filename> <operation>\n", argv[0]);
        exit(EXIT_FAILURE);
    }
    char *source_dir = argv[1];
    char *target_dir = argv[2];
    char *filename = argv[3];
    char *operation = argv[4];

    // Αν πρόκειται για πλήρη συγχρονισμό
    if(strcmp(operation, "FULL") == 0 && strcmp(filename, "ALL") == 0) {
        full_sync(source_dir, target_dir);
    } else {
        // Επεξεργασία για μεμονωμένες αλλαγές (ADDED ή MODIFIED)
        if(strcmp(operation, "ADDED") == 0 || strcmp(operation, "MODIFIED") == 0) {
            char src_path[512], dest_path[512];
            snprintf(src_path, sizeof(src_path), "%s/%s", source_dir, filename);
            snprintf(dest_path, sizeof(dest_path), "%s/%s", target_dir, filename);
            if(copy_file(src_path, dest_path) == 0) {
                printf("EXEC_REPORT_START\n");
                printf("STATUS: SUCCESS\n");
                printf("DETAILS: File %s copied.\n", filename);
                printf("EXEC_REPORT_END\n");
            } else {
                printf("EXEC_REPORT_START\n");
                printf("STATUS: ERROR\n");
                printf("DETAILS: File %s copy failed: %s.\n", filename, strerror(errno));
                printf("EXEC_REPORT_END\n");
            }
            fflush(stdout);
        } else if(strcmp(operation, "DELETED") == 0) {
            // Διαγραφή αρχείου από το target
            char dest_path[512];
            snprintf(dest_path, sizeof(dest_path), "%s/%s", target_dir, filename);
            if(unlink(dest_path) == 0) {
                printf("EXEC_REPORT_START\n");
                printf("STATUS: SUCCESS\n");
                printf("DETAILS: File %s deleted.\n", filename);
                printf("EXEC_REPORT_END\n");
            } else {
                printf("EXEC_REPORT_START\n");
                printf("STATUS: ERROR\n");
                printf("DETAILS: File %s deletion failed: %s.\n", filename, strerror(errno));
                printf("EXEC_REPORT_END\n");
            }
            fflush(stdout);
        }
    }
    return 0;
}
