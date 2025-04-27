#!/usr/bin/env bash
# fss_script.sh - Generate reports and purge based on manager log or target directories
# Usage: ./fss_script.sh -p <path> -c <command>
# Commands: listAll, listMonitored, listStopped, purge

usage() {
    echo "Usage: $0 -p <path> -c <command>"
    exit 1
}

# Extract last sync timestamp and status for a source-target pair
# Args: log_file, source, target
get_last_sync() {
    local log_file="$1" src="$2" tgt="$3"
    # find sync entries for this pair (operations FULL, ADDED, MODIFIED, DELETED)
    local pattern="\[.*\] \[$src\] \[$tgt\]"
    # get last sync line
    local sync_line
    sync_line=$(grep -E "${pattern}" "$log_file" | grep -E "\[(FULL|ADDED|MODIFIED|DELETED)\]" | tail -n1)
    if [[ -z "$sync_line" ]]; then
        echo "Never|UNKNOWN"
        return
    fi
    # extract timestamp
    local last_ts
    last_ts=$(echo "$sync_line" | sed -E 's/^\[([^]]+)\].*/\1/')
    # find line number
    local ln
    ln=$(grep -nF "$sync_line" "$log_file" | cut -d: -f1 | tail -n1)
    # status on next line
    local status_line
    status_line=$(sed -n "$((ln+1))p" "$log_file")
    local status
    status=$(echo "$status_line" | sed -E 's/.*\[(SUCCESS|ERROR|PARTIAL)\].*/\1/')
    echo "${last_ts}|${status}"
}

# Parse arguments
while getopts ":p:c:" opt; do
    case "$opt" in
        p) path="$OPTARG" ;;
        c) command="$OPTARG" ;;
        *) usage ;;
    esac
done
[[ -z "$path" || -z "$command" ]] && usage

case "$command" in
    listAll)
        [[ ! -f "$path" ]] && { echo "Log file not found: $path"; exit 1; }
        echo "Listing all directories:"
        # get unique src->tgt pairs
        grep "Added directory:" "$path" \
            | sed -E 's/.*Added directory: ([^ ]+) -> ([^ ]+).*/\1|\2/' \
            | sort -u \
            | while IFS='|' read -r src tgt; do
                IFS='|' read -r last_sync status < <(get_last_sync "$path" "$src" "$tgt")
                echo "$src -> $tgt [Last Sync: $last_sync] [$status]"
            done
        ;;

    listMonitored)
        [[ ! -f "$path" ]] && { echo "Log file not found: $path"; exit 1; }
        echo "Listing monitored directories:"
        # gather added and stopped lists
        mapfile -t added < <(grep "Added directory:" "$path" \
            | sed -E 's/.*Added directory: ([^ ]+) -> ([^ ]+).*/\1|\2/' \
            | sort -u)
        mapfile -t stopped < <(grep "Monitoring stopped for" "$path" \
            | sed -E 's/.*Monitoring stopped for ([^ ]+).*/\1/')
        for pair in "${added[@]}"; do
            src=${pair%%|*}
            tgt=${pair##*|}
            # skip if stopped
            if printf '%s
' "${stopped[@]}" | grep -Fxq "$src"; then
                continue
            fi
            IFS='|' read -r last_sync status < <(get_last_sync "$path" "$src" "$tgt")
            echo "$src -> $tgt [Last Sync: $last_sync]"
        done
        ;;

    listStopped)
        [[ ! -f "$path" ]] && { echo "Log file not found: $path"; exit 1; }
        echo "Listing stopped directories:"
        mapfile -t stopped < <(grep "Monitoring stopped for" "$path" \
            | sed -E 's/.*Monitoring stopped for ([^ ]+).*/\1/')
        for src in "${stopped[@]}"; do
            # find last target for this src
            tgt=$(grep "Added directory: $src ->" "$path" \
                | sed -E 's/.*-> ([^ ]+).*/\1/' \
                | tail -n1)
            IFS='|' read -r last_sync status < <(get_last_sync "$path" "$src" "$tgt")
            echo "$src -> $tgt [Last Sync: $last_sync]"
        done
        ;;

    purge)
        if [[ -e "$path" ]]; then
            echo "Deleting $path..."
            rm -rf "$path"
            echo "Purge complete."
        else
            echo "Path not found: $path"
            exit 1
        fi
        ;;

    *)
        echo "Unknown command: $command"
        usage
        ;;
esac
