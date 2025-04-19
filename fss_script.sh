#!/bin/bash
# Updated fss_script.sh
# Αυτό το script χρησιμοποιείται για να δημιουργεί αναφορές και να καθαρίζει
# log files ή target directories σύμφωνα με τις εντολές:
#   listAll, listMonitored, listStopped, purge
#
# Χρήση: ./fss_script.sh -p <path> -c <command>
# Όπου το path είναι είτε το log file (manager.log ή console.log) είτε ένας target κατάλογος
# και το command μία από τις παραπάνω εντολές.

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
        grep "Added directory:" "$path" | while IFS= read -r line; do
            # extract source and target using shell parameter expansion
            src_part=${line#*Added directory: }
            source=${src_part%% *}
            tgt_part=${line#*-> }
            target=${tgt_part%% *}
            # find latest STATUS for this source
            status=$(grep "$source" "$path" \
                     | grep "STATUS:" \
                     | tail -n1 \
                     | sed -n 's/.*STATUS: *\([^ ]*\).*/\1/p')
            [ -z "$status" ] && status="Unknown"
            echo "$source -> $target [Status: $status]"
        done
        ;;
    listMonitored)
        if [ ! -f "$path" ]; then
            echo "Log file not found: $path"
            exit 1
        fi
        echo "Listing monitored directories:"
        # το manager.log καταγράφει "Monitoring started for <dir>"
        grep "Monitoring started for" "$path"
        ;;
    listStopped)
        if [ ! -f "$path" ]; then
            echo "Log file not found: $path"
            exit 1
        fi
        echo "Listing stopped directories:"
        # το console.log καταγράφει "Monitoring stopped for <dir>"
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
