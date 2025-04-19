#!/bin/bash
# Updated fss_script.sh
# Αυτό το script χρησιμοποιείται για να δημιουργεί αναφορές και να καθαρίζει
# log files ή target directories σύμφωνα με τις εντολές:
#   listAll, listMonitored, listStopped, purge
#
# Χρήση: ./fss_script.sh -p <path> -c <command>
# Όπου το path είναι είτε το log file είτε ένας target κατάλογος και
# το command μία από τις παραπάνω εντολές.

usage() {
    echo "Usage: $0 -p <path> -c <command>"
    exit 1
}

while getopts ":p:c:" opt; do
    case $opt in
        p) path="$OPTARG" ;;
        c) command="$OPTARG" ;;
        *) usage ;;
    esac
done

if [ -z "$path" ] || [ -z "$command" ]; then
    usage
fi

case "$command" in
    listAll)
        if [ ! -f "$path" ]; then
            echo "Log file not found: $path"
            exit 1
        fi
        echo "Listing all directories from log file:"
        # Αναζητούμε τις γραμμές όπου προστέθηκε καταχώρηση (π.χ., "Added directory:")
        # και επιχειρούμε να εξάγουμε το source, το target, την τελευταία φορά συγχρονισμού
        # και το status (βάσει των εκτυπωμένων exec_report).
        grep "Added directory:" "$path" | while read line; do
            # Παράδειγμα log: 
            # [2025-02-10 10:00:01] Added directory: /home/user/docs -> /backup/docs (Worker PID: 1234)
            source=$(echo "$line" | sed -n 's/.*Added directory: \([^ ]*\) ->.*/\1/p')
            target=$(echo "$line" | sed -n 's/.*-> \([^ ]]*(Worker PID:.*)\?$/\1/p')
            # Εξαγωγή τελευταίας χρονικής στιγμής συγχρονισμού για το συγκεκριμένο source
            last_sync=$(grep "$source" "$path" | grep "Last Sync:" | tail -n 1 | sed -n 's/.*Last Sync: \([^]]*\).*/\1/p')
            # Εξαγωγή του τελευταίου status (STATUS: SUCCESS/ERROR/...)
            status=$(grep "$source" "$path" | grep "STATUS:" | tail -n 1 | sed -n 's/.*STATUS: \([^ ]*\).*/\1/p')
            if [ -z "$last_sync" ]; then last_sync="Never"; fi
            if [ -z "$status" ]; then status="Unknown"; fi
            echo "$source -> $target [Last Sync: $last_sync] [$status]"
        done
        ;;
    listMonitored)
        if [ ! -f "$path" ]; then
            echo "Log file not found: $path"
            exit 1
        fi
        echo "Listing monitored directories:"
        # Υποθέτουμε ότι η εμφάνιση της φράσης "Monitoring started for" σημαίνει ότι ο κατάλογος παρακολουθείται.
        grep "Monitoring started for" "$path"
        ;;
    listStopped)
        if [ ! -f "$path" ]; then
            echo "Log file not found: $path"
            exit 1
        fi
        echo "Listing stopped directories:"
        grep "Monitoring stopped for" "$path"
        ;;
    purge)
        if [ -f "$path" ]; then
            echo "Deleting file: $path..."
            rm "$path"
            echo "Purge complete."
        elif [ -d "$path" ]; then
            echo "Deleting directory: $path..."
            rm -r "$path"
            echo "Purge complete."
        else
            echo "Path not found: $path"
        fi
        ;;
    *)
        echo "Unknown command: $command"
        usage
        ;;
esac
