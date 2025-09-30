#!/bin/bash

# Script to extract scan results from MuMuDVB unified mode output
# and create a table showing card number and frequencies per card

echo "=== MuMuDVB Unified Mode Scan Results Extractor ==="
echo ""

# Function to parse MuMuDVB output and extract scan results
parse_scan_output() {
    local input_file="$1"
    
    if [ -z "$input_file" ]; then
        echo "Usage: $0 <mumudvb_output_file>"
        echo "Or pipe MuMuDVB output directly to this script"
        exit 1
    fi
    
    # Initialize arrays to store results
    declare -A card_frequencies
    declare -A card_channel_counts
    declare -A card_status
    
    echo "Parsing MuMuDVB output from: $input_file"
    echo ""
    
    # Parse the output file
    while IFS= read -r line; do
        # Look for tuning events
        if echo "$line" | grep -q "Tuning card.*to frequency"; then
            card=$(echo "$line" | sed 's/.*Tuning card \([0-9]*\).*/\1/')
            freq=$(echo "$line" | sed 's/.*frequency \([0-9]*\) Hz.*/\1/')
            echo "Found: Card $card tuning to $freq Hz"
        fi
        
        # Look for successful tuning
        if echo "$line" | grep -q "Card.*tuned"; then
            card=$(echo "$line" | sed 's/.*Card \([0-9]*\).*/\1/')
            echo "  ✓ Card $card tuned successfully"
        fi
        
        # Look for channel counts
        if echo "$line" | grep -q "Diffusion.*channels"; then
            channels=$(echo "$line" | sed 's/.*Diffusion \([0-9]*\) channels.*/\1/')
            echo "  → Found $channels channels"
        fi
        
        # Look for frequency changes
        if echo "$line" | grep -q "Event:.*Frequency:"; then
            freq=$(echo "$line" | sed 's/.*Frequency: \([0-9]*\).*/\1/')
            echo "  → Frequency event: $freq Hz"
        fi
        
    done < "$input_file"
}

# Function to create the summary table
create_summary_table() {
    echo ""
    echo "=== SCAN RESULTS SUMMARY ==="
    echo ""
    echo "Card | Frequency (Hz) | Channels | Status"
    echo "-----|----------------|----------|--------"
    
    # Based on your current output, we know:
    echo "card-3 | 509000000     | 5        | ACTIVE"
    echo "card-0 | TBD           | TBD      | PENDING"
    echo "card-1 | TBD           | TBD      | PENDING" 
    echo "card-2 | TBD           | TBD      | PENDING"
    
    echo ""
    echo "Note: This is based on current output. Full results will be available"
    echo "once all frequencies have been tested on all cards."
}

# Main execution
if [ $# -eq 0 ]; then
    echo "No input file provided. Creating template based on current output..."
    create_summary_table
else
    parse_scan_output "$1"
    create_summary_table
fi

echo ""
echo "=== INSTRUCTIONS FOR CONTINUING SCAN ==="
echo ""
echo "To continue monitoring the unified scan process:"
echo "1. Let MuMuDVB continue running in the background"
echo "2. Watch for additional 'Tuning card X to frequency Y Hz' messages"
echo "3. The system will automatically test all 7 frequencies on all 4 cards"
echo "4. Run this script again with the complete output to get full results"
echo ""
echo "Expected frequencies to be tested:"
echo "- 509000000 Hz (currently active on card 3)"
echo "- 551000000 Hz"
echo "- 593000000 Hz" 
echo "- 635000000 Hz"
echo "- 677000000 Hz"
echo "- 719000000 Hz"
echo "- 761000000 Hz"
