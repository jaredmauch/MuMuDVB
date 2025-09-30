#!/bin/bash

# Real-time monitor for MuMuDVB unified mode scanning
# This script helps track progress and generates the final table

echo "=== MuMuDVB Unified Mode Real-Time Monitor ==="
echo ""

# Initialize tracking variables
declare -A card_frequencies
declare -A card_channel_counts
declare -A card_status
declare -A frequency_tested

# Known frequencies from your configuration
FREQUENCIES=(509000000 551000000 593000000 635000000 677000000 719000000 761000000)
CARDS=(0 1 2 3)

# Initialize tracking
for card in "${CARDS[@]}"; do
    card_status[$card]="PENDING"
    card_frequencies[$card]=""
    card_channel_counts[$card]=""
done

for freq in "${FREQUENCIES[@]}"; do
    frequency_tested[$freq]="false"
done

# Function to update the display table
update_table() {
    clear
    echo "=== MuMuDVB Unified Mode Scan Progress ==="
    echo ""
    echo "Card | Frequency (Hz) | Channels | Status"
    echo "-----|----------------|----------|--------"
    
    for card in "${CARDS[@]}"; do
        if [ -n "${card_frequencies[$card]}" ]; then
            printf "card-%d | %-12s | %-7s | %s\n" "$card" "${card_frequencies[$card]}" "${card_channel_counts[$card]}" "${card_status[$card]}"
        else
            printf "card-%d | TBD           | TBD     | %s\n" "$card" "${card_status[$card]}"
        fi
    done
    
    echo ""
    echo "=== Frequency Testing Progress ==="
    for freq in "${FREQUENCIES[@]}"; do
        if [ "${frequency_tested[$freq]}" = "true" ]; then
            echo "✓ $freq Hz - Tested"
        else
            echo "○ $freq Hz - Pending"
        fi
    done
    
    echo ""
    echo "Press Ctrl+C to exit monitoring"
    echo "=========================================="
}

# Function to parse a line of MuMuDVB output
parse_line() {
    local line="$1"
    
    # Check for tuning events
    if echo "$line" | grep -q "Tuning card.*to frequency"; then
        card=$(echo "$line" | sed 's/.*Tuning card \([0-9]*\).*/\1/')
        freq=$(echo "$line" | sed 's/.*frequency \([0-9]*\) Hz.*/\1/')
        card_frequencies[$card]="$freq"
        card_status[$card]="TUNING"
        frequency_tested[$freq]="true"
        update_table
    fi
    
    # Check for successful tuning
    if echo "$line" | grep -q "Card.*tuned"; then
        card=$(echo "$line" | sed 's/.*Card \([0-9]*\).*/\1/')
        card_status[$card]="TUNED"
        update_table
    fi
    
    # Check for channel counts
    if echo "$line" | grep -q "Diffusion.*channels"; then
        channels=$(echo "$line" | sed 's/.*Diffusion \([0-9]*\) channels.*/\1/')
        # Find which card is currently active
        for card in "${CARDS[@]}"; do
            if [ "${card_status[$card]}" = "TUNED" ]; then
                card_channel_counts[$card]="$channels"
                card_status[$card]="ACTIVE"
                break
            fi
        done
        update_table
    fi
    
    # Check for frequency changes
    if echo "$line" | grep -q "Event:.*Frequency:"; then
        freq=$(echo "$line" | sed 's/.*Frequency: \([0-9]*\).*/\1/')
        echo "Frequency event detected: $freq Hz"
    fi
}

# Initial display
update_table

echo ""
echo "Monitoring MuMuDVB output..."
echo "Pipe MuMuDVB output to this script to see real-time updates"
echo ""

# If input is piped, process it line by line
if [ -t 0 ]; then
    echo "No input detected. To use this monitor:"
    echo "  src/mumudvb -s -t -d -c auto.conf | ./real_time_monitor.sh"
    echo ""
    echo "Or save MuMuDVB output to a file and run:"
    echo "  ./real_time_monitor.sh < output_file"
else
    # Process input line by line
    while IFS= read -r line; do
        parse_line "$line"
    done
fi
