/* fss_console.c */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#define FIFO_IN  "fss_in"
#define FIFO_OUT "fss_out"

int main(int argc, char *argv[]) {
    char *console_logfile = NULL;
    int opt;
    while((opt = getopt(argc, argv, "l:")) != -1) {
        switch(opt) {
            case 'l':
                console_logfile = optarg;
                break;
            default:
                fprintf(stderr, "Usage: %s -l <console_logfile>\n", argv[0]);
                exit(EXIT_FAILURE);
        }
    }
    if (!console_logfile) {
        fprintf(stderr, "Console logfile required.\n");
        exit(EXIT_FAILURE);
    }

    FILE *console_fp = fopen(console_logfile, "a");
    if(!console_fp) {
        perror("Error opening console log file");
        exit(EXIT_FAILURE);
    }

    // Άνοιγμα του FIFO_IN με retry, non-blocking
    int fifo_in_fd;
    while (1) {
        fifo_in_fd = open(FIFO_IN, O_WRONLY | O_NONBLOCK);
        if (fifo_in_fd >= 0) break;
        if (errno == ENXIO || errno == ENOENT) {
            usleep(100000); // 100 ms
            continue;
        }
        perror("Error opening FIFO_IN");
        exit(EXIT_FAILURE);
    }

    // Άνοιγμα του FIFO_OUT non-blocking
    int fifo_out_fd = open(FIFO_OUT, O_RDONLY | O_NONBLOCK);
    if(fifo_out_fd < 0) {
        perror("Error opening FIFO_OUT");
        close(fifo_in_fd);
        exit(EXIT_FAILURE);
    }

    char command[256];
    char response[2048];

    while (1) {
        printf("> ");
        if (fgets(command, sizeof(command), stdin) == NULL)
            break;

        // Καταγραφή εντολής
        fprintf(console_fp, "Command: %s", command);
        fflush(console_fp);

        // Αποστολή εντολής
        if (write(fifo_in_fd, command, strlen(command)) < 0) {
            perror("Error writing to FIFO_IN");
            break;
        }

        // Ανάγνωση απάντησης σε loop
        int total = 0, n;
        while (1) {
            n = read(fifo_out_fd, response + total,
                     sizeof(response) - 1 - total);
            if (n > 0) {
                total += n;
                // αν το buffer γεμίσει, κόβουμε
                if (total >= (int)sizeof(response) - 1) break;
                // συνεχίζουμε να διαβάζουμε ό,τι περίσσεψε
                continue;
            }
            if (n == 0 || (n < 0 && errno == EAGAIN)) {
                // EOF ή δεν υπάρχουν άλλα δεδομένα
                break;
            }
            // άλλο σφάλμα
            if (n < 0) {
                perror("Error reading from FIFO_OUT");
                break;
            }
        }

        if (total > 0) {
            response[total] = '\0';
            printf("%s\n", response);
            fprintf(console_fp, "%s\n", response);
            fflush(console_fp);

            // Έλεγχος για shutdown
            if (strstr(command, "shutdown") != NULL)
                break;
        }
    }

    close(fifo_in_fd);
    close(fifo_out_fd);
    fclose(console_fp);
    return 0;
}
