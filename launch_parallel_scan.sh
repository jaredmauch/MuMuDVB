#!/bin/bash

# MuMuDVB Parallel Scanning Launcher
# Launches multiple MuMuDVB instances in parallel for true parallel scanning

# Configuration
CARDS="0,1,2,3"
FREQUENCIES="509000000,551000000,593000000,635000000,677000000,719000000,761000000"
RESULTS_DIR="/tmp/mumudvb_parallel_scan"
LOG_DIR="$RESULTS_DIR/logs"
RESULTS_FILE="$RESULTS_DIR/scan_results.txt"

# Create directories
mkdir -p "$LOG_DIR"

# Initialize results file
echo "Card,Frequency,Channels_Found,Channel_Names,Status,Signal_Strength,SNR" > "$RESULTS_FILE"

echo "=== MuMuDVB Parallel Scanning ==="
echo "Cards: $CARDS"
echo "Frequencies: $FREQUENCIES"
echo ""

# Convert to arrays
IFS=',' read -ra CARD_ARRAY <<< "$CARDS"
IFS=',' read -ra FREQ_ARRAY <<< "$FREQUENCIES"

# Function to create config for card/frequency combination
create_config() {
    local card=$1
    local freq=$2
    local config_file="$RESULTS_DIR/card${card}_freq${freq}.conf"
    
    cat > "$config_file" << EOF
# MuMuDVB Configuration for Card $card, Frequency $freq
card=$card
tuner=0
freq=$freq

# Auto-detect delivery system
delivery_system=auto

# Enable full autoconfiguration
autoconfiguration=full

# Network settings
unicast=1
port_http=$((10000 + card * 100 + freq % 1000))
unicast_queue_size=4194304

# PAT/SDT rewriting
rewrite_pat=1
rewrite_sdt=1
sort_eit=1

# Logging
log_type=console
log_file=$LOG_DIR/card${card}_freq${freq}.log

# Timeout settings
tuning_no_diff=5
timeout_no_diff=10
EOF
    echo "$config_file"
}

# Function to test single card/frequency
test_card_frequency() {
    local card=$1
    local freq=$2
    local config_file=$(create_config "$card" "$freq")
    local log_file="$LOG_DIR/card${card}_freq${freq}.log"
    
    echo "Testing card-$card at frequency $freq Hz..."
    
    # Run MuMuDVB with timeout
    timeout 30s src/mumudvb -s -t -d -c "$config_file" > "$log_file" 2>&1 &
    local pid=$!
    
    # Wait for completion
    wait $pid
    local exit_code=$?
    
    # Extract results
    local channel_count=0
    local channel_names=""
    local signal_strength=0
    local snr=0
    local status="ERROR"
    
    if [ -f "$log_file" ]; then
        # Extract channel count
        channel_count=$(grep "Diffusion.*channels" "$log_file" | tail -1 | sed 's/.*Diffusion \([0-9]*\) channels.*/\1/')
        if [ -z "$channel_count" ]; then
            channel_count=0
        fi
        
        # Extract channel names
        channel_names=$(grep "Channel number.*name" "$log_file" | sed 's/.*name : "\([^"]*\)".*/\1/' | tr '\n' ';' | sed 's/;$//')
        
        # Extract signal strength
        signal_strength=$(grep "Signal strength:" "$log_file" | tail -1 | sed 's/.*Signal strength: \([0-9]*\).*/\1/')
        if [ -z "$signal_strength" ]; then
            signal_strength=0
        fi
        
        # Extract SNR
        snr=$(grep "SNR:" "$log_file" | tail -1 | sed 's/.*SNR: \([0-9]*\).*/\1/')
        if [ -z "$snr" ]; then
            snr=0
        fi
        
        # Determine status
        if [ $exit_code -eq 124 ]; then
            status="TIMEOUT"
        elif [ $exit_code -eq 0 ] && [ "$channel_count" -gt 0 ]; then
            status="SUCCESS"
        elif [ "$channel_count" -eq 0 ]; then
            status="NO_CHANNELS"
        else
            status="ERROR"
        fi
    fi
    
    # Record results
    echo "card-$card,$freq,$channel_count,\"$channel_names\",$status,$signal_strength,$snr" >> "$RESULTS_FILE"
    
    # Clean up
    rm -f "$config_file"
    
    echo "  Result: $channel_count channels found ($status)"
    if [ "$channel_count" -gt 0 ]; then
        echo "  Channels: $channel_names"
    fi
    echo ""
}

# Launch all tests in parallel
echo "Launching parallel tests..."
echo ""

# Start all card/frequency combinations in parallel
pids=()
for card in "${CARD_ARRAY[@]}"; do
    for freq in "${FREQ_ARRAY[@]}"; do
        test_card_frequency "$card" "$freq" &
        pids+=($!)
        sleep 0.1  # Small delay to avoid overwhelming the system
    done
done

echo "All tests launched. Waiting for completion..."
echo ""

# Wait for all tests to complete
for pid in "${pids[@]}"; do
    wait $pid
done

echo "=== ALL TESTS COMPLETED ==="
echo ""

# Generate summary table
echo "=== FINAL RESULTS TABLE ==="
echo ""
echo "Card | Frequency (Hz) | Channels | Status | Signal | SNR | Channel Names"
echo "-----|----------------|----------|--------|--------|-----|---------------"

# Parse and display results
while IFS=',' read -r card freq channels names status signal snr; do
    # Skip header line
    if [ "$card" = "Card" ]; then
        continue
    fi
    
    # Clean up data
    card=$(echo "$card" | sed 's/card-//')
    names=$(echo "$names" | sed 's/"//g' | cut -c1-30) # Limit length
    
    printf "%-4s | %-12s | %-7s | %-6s | %-6s | %-3s | %s\n" \
        "$card" "$freq" "$channels" "$status" "$signal" "$snr" "$names"
done < "$RESULTS_FILE"

echo ""
echo "=== SUMMARY BY CARD ==="
for card in "${CARD_ARRAY[@]}"; do
    echo "Card $card:"
    grep "card-$card," "$RESULTS_FILE" | while IFS=',' read -r c f ch n s st sig snr; do
        echo "  $f Hz: $ch channels ($st)"
    done
    echo ""
done

echo "Results saved to: $RESULTS_FILE"
echo "Logs saved to: $LOG_DIR"
echo ""
echo "Parallel scanning completed!"
