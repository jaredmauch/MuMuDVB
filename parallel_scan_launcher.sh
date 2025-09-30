#!/bin/bash

# MuMuDVB Parallel Scanning Launcher
# This script launches multiple MuMuDVB instances in parallel, one per card,
# to achieve true parallel scanning of all frequencies on all cards

# Configuration
CARDS="0,1,2,3"
FREQUENCIES="509000000,551000000,593000000,635000000,677000000,719000000,761000000"
RESULTS_DIR="/tmp/mumudvb_parallel_scan"
LOG_DIR="$RESULTS_DIR/logs"
CONFIG_DIR="$RESULTS_DIR/configs"
RESULTS_FILE="$RESULTS_DIR/scan_results.txt"
FINAL_TABLE="$RESULTS_DIR/final_channel_table.txt"

# Create directories
mkdir -p "$LOG_DIR" "$CONFIG_DIR"

# Initialize results file
echo "Card,Frequency,Channels_Found,Channel_Names,Service_IDs,Status,Signal_Strength,SNR" > "$RESULTS_FILE"

echo "=== MuMuDVB Parallel Scanning Launcher ==="
echo "Cards: $CARDS"
echo "Frequencies: $FREQUENCIES"
echo "Results will be saved to: $RESULTS_DIR"
echo ""

# Convert comma-separated strings to arrays
IFS=',' read -ra CARD_ARRAY <<< "$CARDS"
IFS=',' read -ra FREQ_ARRAY <<< "$FREQUENCIES"

# Function to create configuration for a specific card
create_card_config() {
    local card=$1
    local config_file="$CONFIG_DIR/card${card}.conf"
    
    cat > "$config_file" << EOF
# MuMuDVB Configuration for Card $card
# Parallel scanning configuration

# Card and tuner settings
card=$card
tuner=0

# Auto-detect delivery system
delivery_system=auto

# Enable full autoconfiguration
autoconfiguration=full

# Network settings
unicast=1
port_http=$((10000 + card))
unicast_queue_size=4194304

# PAT/SDT rewriting
rewrite_pat=1
rewrite_sdt=1
sort_eit=1

# Logging
log_type=console
log_file=$LOG_DIR/card${card}.log
log_level=info

# Timeout settings
tuning_no_diff=5
timeout_no_diff=10

# Channel collection
collect_all_channels=1
validate_channels=1
min_signal_strength=10000
EOF
    echo "$config_file"
}

# Function to test a single frequency on a card
test_frequency_on_card() {
    local card=$1
    local freq=$2
    local config_file=$(create_card_config "$card")
    local log_file="$LOG_DIR/card${card}_freq${freq}.log"
    
    echo "Testing card-$card at frequency $freq Hz..."
    
    # Create temporary config for this specific frequency
    local temp_config="$CONFIG_DIR/card${card}_freq${freq}.conf"
    cp "$config_file" "$temp_config"
    echo "freq=$freq" >> "$temp_config"
    
    # Run MuMuDVB with timeout
    timeout 30s src/mumudvb -s -t -d -c "$temp_config" > "$log_file" 2>&1 &
    local pid=$!
    
    # Wait for the process to complete or timeout
    wait $pid
    local exit_code=$?
    
    # Extract results from log
    local channel_count=0
    local channel_names=""
    local service_ids=""
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
        
        # Extract service IDs
        service_ids=$(grep "service id" "$log_file" | sed 's/.*service id \([0-9]*\).*/\1/' | tr '\n' ';' | sed 's/;$//')
        
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
    echo "card-$card,$freq,$channel_count,\"$channel_names\",\"$service_ids\",$status,$signal_strength,$snr" >> "$RESULTS_FILE"
    
    # Clean up
    rm -f "$temp_config"
    
    echo "  Result: $channel_count channels found ($status)"
    if [ "$channel_count" -gt 0 ]; then
        echo "  Channels: $channel_names"
    fi
    echo ""
}

# Function to run parallel tests for a single card
run_card_parallel_tests() {
    local card=$1
    echo "=== Starting parallel tests for Card $card ==="
    
    # Launch all frequency tests for this card in parallel
    local pids=()
    for freq in "${FREQ_ARRAY[@]}"; do
        test_frequency_on_card "$card" "$freq" &
        pids+=($!)
    done
    
    # Wait for all tests for this card to complete
    for pid in "${pids[@]}"; do
        wait $pid
    done
    
    echo "=== Completed all tests for Card $card ==="
    echo ""
}

# Function to generate final table
generate_final_table() {
    echo "=== GENERATING FINAL CHANNEL TABLE ==="
    echo ""
    
    # Create the final table
    cat > "$FINAL_TABLE" << EOF
=== MuMuDVB Parallel Scan Results ===
Generated: $(date)
Cards Tested: ${#CARD_ARRAY[@]}
Frequencies Tested: ${#FREQ_ARRAY[@]}

=== CARD-FREQUENCY MATRIX ===
EOF
    
    echo "Card | Frequency (Hz) | Channels | Status | Signal | SNR | Channel Names" >> "$FINAL_TABLE"
    echo "-----|----------------|----------|--------|--------|-----|---------------" >> "$FINAL_TABLE"
    
    # Parse results and create table
    while IFS=',' read -r card freq channels names services status signal snr; do
        # Clean up the data
        card=$(echo "$card" | sed 's/card-//')
        names=$(echo "$names" | sed 's/"//g' | cut -c1-50) # Limit length
        
        printf "%-4s | %-12s | %-7s | %-6s | %-6s | %-3s | %s\n" \
            "$card" "$freq" "$channels" "$status" "$signal" "$snr" "$names" >> "$FINAL_TABLE"
    done < "$RESULTS_FILE"
    
    echo "" >> "$FINAL_TABLE"
    echo "=== SUMMARY BY CARD ===" >> "$FINAL_TABLE"
    
    # Generate summary by card
    for card in "${CARD_ARRAY[@]}"; do
        echo "Card $card:" >> "$FINAL_TABLE"
        grep "card-$card," "$RESULTS_FILE" | while IFS=',' read -r c f ch n s st sig snr; do
            echo "  $f Hz: $ch channels ($st)" >> "$FINAL_TABLE"
        done
        echo "" >> "$FINAL_TABLE"
    done
    
    # Display the table
    cat "$FINAL_TABLE"
}

# Main execution
echo "Starting parallel scanning process..."
echo ""

# Run tests for each card in parallel
for card in "${CARD_ARRAY[@]}"; do
    run_card_parallel_tests "$card" &
done

# Wait for all card tests to complete
wait

echo "=== ALL PARALLEL TESTS COMPLETED ==="
echo ""

# Generate final table
generate_final_table

echo ""
echo "=== FILES GENERATED ==="
echo "Results CSV: $RESULTS_FILE"
echo "Final Table: $FINAL_TABLE"
echo "Logs Directory: $LOG_DIR"
echo "Configs Directory: $CONFIG_DIR"
echo ""
echo "Scan completed successfully!"
