/* fss_console.c */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#define FIFO_IN "fss_in"
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
    
    // Άνοιγμα των named pipes για επικοινωνία με τον manager.
    int fifo_in_fd = open(FIFO_IN, O_WRONLY);
    int fifo_out_fd = open(FIFO_OUT, O_RDONLY);
    if(fifo_in_fd < 0 || fifo_out_fd < 0) {
        perror("Error opening FIFOs");
        exit(EXIT_FAILURE);
    }

    char command[256];
    char response[2048];  // Μεγαλύτερο buffer σε περίπτωση που τα μηνύματα είναι μεγαλύτερα.
    
    while(1) {
        printf("> ");
        if(fgets(command, sizeof(command), stdin) == NULL)
            break;
        
        // Καταγραφή εντολής στο console log.
        fprintf(console_fp, "Command: %s", command);
        fflush(console_fp);
        
        // Αποστολή της εντολής στον fss_manager.
        if(write(fifo_in_fd, command, strlen(command)) < 0) {
            perror("Error writing to FIFO_IN");
            break;
        }
        
        // Ανάγνωση της απάντησης από τον fss_manager.
        int n = read(fifo_out_fd, response, sizeof(response) - 1);
        if(n > 0) {
            response[n] = '\0';
            printf("%s\n", response);
            fprintf(console_fp, "%s\n", response);
            fflush(console_fp);
            
            // Αν η εντολή ήταν shutdown, τερματίζουμε την εφαρμογή.
            if(strstr(command, "shutdown") != NULL)
                break;
        }
        else if(n < 0) {
            perror("Error reading from FIFO_OUT");
        }
    }

    // Κλείσιμο των πόρων.
    close(fifo_in_fd);
    close(fifo_out_fd);
    fclose(console_fp);
    
    return 0;
}
