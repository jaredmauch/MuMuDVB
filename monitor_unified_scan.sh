#!/bin/bash

# Script to monitor MuMuDVB unified mode scanning and extract results
# This works with the currently running MuMuDVB instance

echo "Monitoring MuMuDVB unified mode scanning..."
echo "This script will help extract card and frequency information from the logs"
echo ""

# Function to extract card and frequency information from MuMuDVB output
extract_scan_info() {
    echo "=== EXTRACTING SCAN INFORMATION ==="
    echo ""
    
    # Look for tuning information
    echo "Tuning events found:"
    grep -E "(Tuning card|tuned to frequency)" /dev/stdin | while read line; do
        echo "  $line"
    done
    
    echo ""
    echo "Channel discovery events:"
    grep -E "(Diffusion.*channels|Channel number)" /dev/stdin | while read line; do
        echo "  $line"
    done
}

# Function to create a summary table
create_summary_table() {
    echo ""
    echo "=== SUMMARY TABLE ==="
    echo "Card | Frequency (Hz) | Channels Found | Status"
    echo "-----|----------------|----------------|--------"
    
    # This would be populated from the actual scan results
    # For now, showing the format
    echo "card-3 | 509000000     | 5             | ACTIVE"
    echo "card-0 | TBD           | TBD           | PENDING"
    echo "card-1 | TBD           | TBD           | PENDING"
    echo "card-2 | TBD           | TBD           | PENDING"
}

# Instructions for manual monitoring
echo "=== MANUAL MONITORING INSTRUCTIONS ==="
echo ""
echo "To monitor the unified scanning process:"
echo "1. Watch for 'Tuning card X to frequency Y Hz' messages"
echo "2. Look for 'Card X, tuner 0 tuned' confirmations"
echo "3. Count 'Diffusion X channels' messages per frequency"
echo "4. Note any error messages or timeouts"
echo ""
echo "Expected sequence:"
echo "- Card 3: 509000000 Hz (5 channels) ✓ DONE"
echo "- Card 0: Next frequency..."
echo "- Card 1: Next frequency..."
echo "- Card 2: Next frequency..."
echo "- Continue until all 7 frequencies tested on all 4 cards"
echo ""

# If running interactively, offer to monitor
if [ -t 0 ]; then
    echo "Press Enter to start monitoring (or Ctrl+C to exit)..."
    read
    
    echo "Monitoring MuMuDVB output (press Ctrl+C to stop)..."
    echo ""
    
    # Monitor the output and extract information
    tail -f /dev/stdin | extract_scan_info
else
    echo "Pipe MuMuDVB output to this script to extract scan information"
    extract_scan_info
fi
