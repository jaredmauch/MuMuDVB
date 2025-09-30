#!/bin/bash

# Script to test all frequencies on all cards in MuMuDVB unified mode
# This will systematically test each card with each frequency and collect results

# Configuration
CARDS="0,1,2,3"
FREQUENCIES="509000000,551000000,593000000,635000000,677000000,719000000,761000000"
RESULTS_FILE="/tmp/mumudvb_scan_results.txt"
LOG_DIR="/tmp/mumudvb_logs"

# Create log directory
mkdir -p "$LOG_DIR"

# Initialize results file
echo "Card,Frequency,Channels_Found,Status" > "$RESULTS_FILE"

echo "Starting systematic scan of all cards and frequencies..."
echo "Cards: $CARDS"
echo "Frequencies: $FREQUENCIES"
echo ""

# Convert comma-separated strings to arrays
IFS=',' read -ra CARD_ARRAY <<< "$CARDS"
IFS=',' read -ra FREQ_ARRAY <<< "$FREQUENCIES"

# Function to create a test configuration for a specific card and frequency
create_test_config() {
    local card=$1
    local freq=$2
    local config_file="/tmp/test_card${card}_freq${freq}.conf"
    
    cat > "$config_file" << EOF
# Test configuration for card $card, frequency $freq
unified_cards=$card
unified_frequencies=$freq

autoconfiguration=full
unicast=1
port_http=10000

# Short timeout for testing
tuning_no_diff=3
timeout_no_diff=5

# Logging
log_type=console
log_file=$LOG_DIR/card${card}_freq${freq}.log
EOF
    echo "$config_file"
}

# Function to extract channel count from MuMuDVB output
extract_channel_count() {
    local log_file=$1
    local channel_count=0
    
    if [ -f "$log_file" ]; then
        # Look for "Diffusion X channels" pattern
        channel_count=$(grep "Diffusion.*channels" "$log_file" | tail -1 | sed 's/.*Diffusion \([0-9]*\) channels.*/\1/')
        if [ -z "$channel_count" ]; then
            channel_count=0
        fi
    fi
    
    echo "$channel_count"
}

# Function to test a single card/frequency combination
test_card_frequency() {
    local card=$1
    local freq=$2
    local config_file=$(create_test_config "$card" "$freq")
    local log_file="$LOG_DIR/card${card}_freq${freq}.log"
    
    echo "Testing card-$card at frequency $freq Hz..."
    
    # Run MuMuDVB with timeout
    timeout 30s src/mumudvb -s -t -d -c "$config_file" > "$log_file" 2>&1 &
    local pid=$!
    
    # Wait for the process to complete or timeout
    wait $pid
    local exit_code=$?
    
    # Extract channel count
    local channel_count=$(extract_channel_count "$log_file")
    
    # Determine status
    local status="SUCCESS"
    if [ $exit_code -eq 124 ]; then
        status="TIMEOUT"
    elif [ $exit_code -ne 0 ]; then
        status="ERROR"
    elif [ "$channel_count" -eq 0 ]; then
        status="NO_CHANNELS"
    fi
    
    # Record results
    echo "card-$card,$freq,$channel_count,$status" >> "$RESULTS_FILE"
    
    # Clean up
    rm -f "$config_file"
    
    echo "  Result: $channel_count channels found ($status)"
    echo ""
}

# Test each card with each frequency
for card in "${CARD_ARRAY[@]}"; do
    echo "=== Testing Card $card ==="
    for freq in "${FREQ_ARRAY[@]}"; do
        test_card_frequency "$card" "$freq"
        sleep 2  # Brief pause between tests
    done
    echo ""
done

echo "=== SCAN COMPLETE ==="
echo ""
echo "Results saved to: $RESULTS_FILE"
echo ""

# Generate summary table
echo "=== SUMMARY TABLE ==="
echo "Card | Frequencies (Hz) | Channels per Frequency"
echo "-----|------------------|----------------------"

# Group results by card
for card in "${CARD_ARRAY[@]}"; do
    echo -n "card-$card | "
    
    # Get all frequencies for this card
    card_freqs=()
    card_channels=()
    
    while IFS=',' read -r c f ch s; do
        if [ "$c" = "card-$card" ]; then
            card_freqs+=("$f")
            card_channels+=("$ch")
        fi
    done < "$RESULTS_FILE"
    
    # Display frequencies and channel counts
    for i in "${!card_freqs[@]}"; do
        if [ $i -gt 0 ]; then
            echo -n "           | "
        fi
        echo "${card_freqs[$i]} (${card_channels[$i]} ch)"
    done
    echo ""
done

echo ""
echo "Detailed results:"
cat "$RESULTS_FILE"
