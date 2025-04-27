/* fss_console.c */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>

#define FIFO_IN  "fss_in"
#define FIFO_OUT "fss_out"

// Helper to produce timestamp in [YYYY-MM-DD HH:MM:SS]
void current_time_str(char *buffer, size_t size) {
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    strftime(buffer, size, "[%Y-%m-%d %H:%M:%S]", tm);
}

int main(int argc, char *argv[]) {
    char *console_logfile = NULL;
    int opt;
    while ((opt = getopt(argc, argv, "l:")) != -1) {
        if (opt == 'l') {
            console_logfile = optarg;
        } else {
            fprintf(stderr, "Usage: %s -l <console_logfile>\n", argv[0]);
            exit(EXIT_FAILURE);
        }
    }
    if (!console_logfile) {
        fprintf(stderr, "Console logfile required.\n");
        exit(EXIT_FAILURE);
    }

    FILE *console_fp = fopen(console_logfile, "a");
    if (!console_fp) {
        perror("Error opening console log file");
        exit(EXIT_FAILURE);
    }

    // Open FIFO_IN (write end) with retry
    int fifo_in_fd;
    while (1) {
        fifo_in_fd = open(FIFO_IN, O_WRONLY | O_NONBLOCK);
        if (fifo_in_fd >= 0) break;
        if (errno == ENXIO || errno == ENOENT) {
            usleep(100000);
            continue;
        }
        perror("Error opening " FIFO_IN);
        fclose(console_fp);
        exit(EXIT_FAILURE);
    }

    // Open FIFO_OUT (read end) with retry
    int fifo_out_fd;
    while (1) {
        fifo_out_fd = open(FIFO_OUT, O_RDONLY | O_NONBLOCK);
        if (fifo_out_fd >= 0) break;
        if (errno == ENXIO || errno == ENOENT) {
            usleep(100000);
            continue;
        }
        perror("Error opening " FIFO_OUT);
        close(fifo_in_fd);
        fclose(console_fp);
        exit(EXIT_FAILURE);
    }

    char command[256];
    char response[2048];
    char tbuf[32];

    while (1) {
        // Prompt user
        printf("> ");
        fflush(stdout);
        if (!fgets(command, sizeof(command), stdin))
            break;
        // Strip newline
        command[strcspn(command, "\n")] = '\0';

        // Log command with timestamp
        current_time_str(tbuf, sizeof(tbuf));
        fprintf(console_fp, "%s Command %s\n", tbuf, command);
        fflush(console_fp);

        // Send command to manager (append newline)
        char cmd_nl[260];
        snprintf(cmd_nl, sizeof(cmd_nl), "%s\n", command);
        if (write(fifo_in_fd, cmd_nl, strlen(cmd_nl)) < 0) {
            perror("Error writing to " FIFO_IN);
            break;
        }

        // Read response
        int total = 0;
        while (1) {
            int n = read(fifo_out_fd, response + total,
                         sizeof(response) - 1 - total);
            if (n > 0) {
                total += n;
                if (total >= (int)sizeof(response) - 1) break;
                continue;
            }
            if (n == 0 || (n < 0 && errno == EAGAIN)) {
                break;
            }
            perror("Error reading from " FIFO_OUT);
            break;
        }

        if (total > 0) {
            response[total] = '\0';
            // Print and log response verbatim (includes timestamps)
            fputs(response, stdout);
            fputs(response, console_fp);
            fflush(console_fp);
        }

        // Check for shutdown command
        char cmd_copy[256];
        strncpy(cmd_copy, command, sizeof(cmd_copy));
        char *token = strtok(cmd_copy, " ");
        if (token && strcmp(token, "shutdown") == 0)
            break;
    }

    close(fifo_in_fd);
    close(fifo_out_fd);
    fclose(console_fp);
    return 0;
}
