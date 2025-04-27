# Σύστημα Συγχρονισμού Αρχείων (FSS) - README

## 1. Επισκόπηση
Αυτό το έργο υλοποιεί ένα **Σύστημα Συγχρονισμού Αρχείων (FSS)** σε C και Bash, με τέσσερα κύρια στοιχεία:

- **fss_manager**: διαχειριστής που αρχικοποιεί το σύστημα, διαβάζει το αρχείο config, διαχειρίζεται inotify, δημιουργεί διεργασίες εργαζομένων, χειρίζεται εντολές κονσόλας και αναφορές εργαζομένων.
- **fss_console**: διεπαφή CLI για αποστολή εντολών (`add`, `status`, `sync`, `cancel`, `shutdown`) στον manager μέσω named pipes, και καταγραφή της δραστηριότητας του χρήστη.
- **worker**: πρόγραμμα που εκτελείται από τον manager με `fork()`+`exec()`, εκτελεί εργασίες συγχρονισμού αρχείων (full ή ανά αρχείο) με χαμηλού επιπέδου syscalls και παράγει δομημένο `exec_report`.
- **fss_script.sh**: Bash script για δημιουργία αναφορών (`listAll`, `listMonitored`, `listStopped`) από τα αρχεία καταγραφής και για ασφαλή διαγραφή (purge) φακέλων ή log files.

Όλα τα στοιχεία επικοινωνούν μέσω δύο named pipes: `fss_in` (console ➔ manager) και `fss_out` (manager ➔ console). Οι worker διεργασίες αναφέρουν τα αποτελέσματά τους στον manager μέσω ανώνυμων pipes.

## 2. Αρχιτεκτονική & Σχεδιαστικές Επιλογές

### Δομές Δεδομένων
- **sync_info list**: μοναδική συνδεδεμένη λίστα με κόμβους που περιέχουν στοιχεία για κάθε φάκελο πηγής (`source_dir`, `target_dir`, `active`, `last_result`, `last_sync_time`, `error_count`, `inotify_watch`).
- **task queue**: FIFO ουρά εργασιών συγχρονισμού όταν ο αριθμός εργαζομένων φτάνει το όριο.
- **worker_pipe list**: λίστα που παρακολουθεί κάθε ενεργό worker (PID, pipe FD, source) για ανάγνωση των `exec_report`.

### Ταυτόχρονες Λειτουργίες & Non‑blocking I/O
- Ο manager χρησιμοποιεί `select()` στα file descriptors του inotify, του `fss_in` και όλων των pipes εργαζομένων για multiplexing γεγονότων χωρίς busy‑wait.
- Το `fss_in` ανοίγει αρχικά με `O_NONBLOCK` και μετά το `fcntl()` το κάνει blocking ώστε το `read()` να περιμένει δεδομένα εντός του `select()`.
- Το `fss_out` ανοίγει non‑blocking για εγγραφή.

### Inotify
- Χρήση `inotify_init()`, `inotify_add_watch()` για `IN_CREATE|IN_MODIFY|IN_DELETE` σε κάθε φάκελο πηγής.
- Αφαίρεση παρακολούθησης με `inotify_rm_watch()` όταν εκτελείται `cancel`.

### Εντολές & Απαντήσεις
- **add**: αποστέλλει δύο μηνύματα (“Added directory…” και “Monitoring started…”) σε ένα atomic `write()` για συνέπεια στην κονσόλα.
- Όλες οι ημερομηνίες/ώρες μορφοποιούνται με `strftime("[%Y-%m-%d %H:%M:%S]")`.

### Εκτέλεση Worker
- Ο manager εκτελεί `fork()` + `execl()` για το `worker`, ανακατευθύνει το stdout σε pipe, και χειρίζεται το `SIGCHLD` με `SA_RESTART`.
- Μετά τον τερματισμό, γίνεται αναμονή με `waitpid()` και update των δομών δεδομένων.

### Διαχείριση Σφαλμάτων
- Έλεγχος όλων των syscalls (`open`, `read`, `write`, `unlink`, `inotify_*`, `fork`, `exec`, `pipe`, `select`, `mkfifo`), με `perror()` και μετρητές σφαλμάτων.
- Οι worker χρησιμοποιούν `errno` + `strerror()` για λεπτομερή αναφορά σφαλμάτων.

## 3. Μεταγλώττιση

```bash
# Μεμονωμένη μεταγλώττιση
gcc -Wall -Wextra -std=gnu11 -o fss_manager fss_manager.c
gcc -Wall -Wextra -std=gnu11 -o fss_console fss_console.c
gcc -Wall -Wextra -std=gnu11 -o worker worker.c
chmod +x fss_script.sh

# Ή μέσω Makefile
gmake all    # ή απλά make
make clean
```

## 4. Εκτέλεση

1. **Δημιουργία φακέλων & config**:
   ```bash
   mkdir src dst
   echo "src dst" > config.txt
   ```
2. **Τερματικό #1**:
   ```bash
   ./fss_manager -l manager.log -c config.txt &
   ```
3. **Τερματικό #2**:
   ```bash
   ./fss_console -l console.log
   ```
4. **Εντολές στο console**:
   ```text
   > add src dst
   > status src
   > sync src
   > cancel src
   > shutdown
   ```
5. **Δοκιμή inotify** (Τερματικό #3):
   ```bash
   mkdir src2 dst2
   echo hello > src2/hello.txt
   sleep 1
   ls dst2  # πρέπει να δείξει hello.txt
   ```

## 5. fss_script.sh Usage

```bash
./fss_script.sh -p manager.log -c listAll
./fss_script.sh -p manager.log -c listMonitored
./fss_script.sh -p manager.log -c listStopped
./fss_script.sh -p dst2 -c purge
```

*README στα ελληνικά με οδηγίες, αρχιτεκτονική και παραδείγματα.*

