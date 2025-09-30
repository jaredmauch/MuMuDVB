#!/bin/bash

# Complete frequency scanning script with detailed channel reporting
# This script will run MuMuDVB and capture all channel discovery results

echo "=== MuMuDVB Complete Frequency Scan ==="
echo "This will test all frequencies on all cards and report channels found"
echo ""

# Configuration
CONFIG_FILE="parallel_worker_config.conf"
LOG_FILE="/tmp/mumudvb_complete_scan.log"
RESULTS_FILE="/tmp/mumudvb_scan_results.txt"

# Create a comprehensive configuration for scanning
cat > scan_config.conf << 'EOF'
# Complete Frequency Scanning Configuration
unified_cards=auto
unified_frequencies=509000000,515000000,521000000,533000000,539000000,581000000,593000000

# Enable full autoconfiguration
autoconfiguration=full

# Network settings
unicast=1
port_http=10000
unicast_queue_size=4194304

# PAT/SDT rewriting
rewrite_pat=1
rewrite_sdt=1
sort_eit=1

# Logging settings
log_level=info
log_module=Unified,Parallel,Background,Autoconf

# Timeout settings
tuning_no_diff=10
timeout_no_diff=15

# Enable channel validation
validate_channels=1
min_signal_strength=5000
EOF

echo "Configuration created: scan_config.conf"
echo ""

# Function to extract channel information from logs
extract_channel_info() {
    local log_file="$1"
    local results_file="$2"
    
    echo "=== EXTRACTING CHANNEL INFORMATION ==="
    echo ""
    
    # Initialize results file
    echo "Card,Frequency,Channels_Found,Channel_Names,Service_IDs,Signal_Strength,SNR,Status" > "$results_file"
    
    # Extract tuning results
    echo "=== TUNING RESULTS ==="
    grep -E "(Tuning card|Card.*tuned|Testing card.*for frequency)" "$log_file" | while read line; do
        echo "$line"
    done
    
    echo ""
    echo "=== CHANNEL DISCOVERY RESULTS ==="
    
    # Extract channel counts and names
    grep -E "(Diffusion.*channels|Channel number.*name)" "$log_file" | while read line; do
        echo "$line"
        
        # Extract channel information for CSV
        if echo "$line" | grep -q "Diffusion.*channels"; then
            local channels=$(echo "$line" | sed 's/.*Diffusion \([0-9]*\) channels.*/\1/')
            echo "Found $channels channels"
        fi
        
        if echo "$line" | grep -q "Channel number.*name"; then
            local channel_name=$(echo "$line" | sed 's/.*name : "\([^"]*\)".*/\1/')
            local service_id=$(echo "$line" | sed 's/.*service id \([0-9]*\).*/\1/')
            echo "  Channel: $channel_name (SID: $service_id)"
        fi
    done
    
    echo ""
    echo "=== SIGNAL QUALITY RESULTS ==="
    
    # Extract signal quality information
    grep -E "(Signal strength|SNR|FE_STATUS)" "$log_file" | while read line; do
        echo "$line"
    done
}

# Function to run the scan
run_scan() {
    echo "Starting MuMuDVB with complete scanning..."
    echo "Log file: $LOG_FILE"
    echo "Results file: $RESULTS_FILE"
    echo ""
    
    # Run MuMuDVB with timeout to allow scanning to complete
    timeout 120s src/mumudvb -s -t -d -c scan_config.conf > "$LOG_FILE" 2>&1 &
    local pid=$!
    
    echo "MuMuDVB started (PID: $pid)"
    echo "Scanning in progress... (timeout: 120 seconds)"
    echo ""
    
    # Monitor progress
    local count=0
    while kill -0 $pid 2>/dev/null && [ $count -lt 120 ]; do
        sleep 5
        count=$((count + 5))
        echo "Scanning... ${count}s elapsed"
        
        # Show recent activity
        tail -5 "$LOG_FILE" 2>/dev/null | grep -E "(Testing|Diffusion|Channel)" || true
    done
    
    # Wait for process to complete
    wait $pid
    local exit_code=$?
    
    echo ""
    echo "=== SCAN COMPLETED ==="
    echo "Exit code: $exit_code"
    echo ""
    
    # Extract results
    extract_channel_info "$LOG_FILE" "$RESULTS_FILE"
    
    echo ""
    echo "=== SUMMARY ==="
    echo "Log file: $LOG_FILE"
    echo "Results file: $RESULTS_FILE"
    echo ""
    
    # Show summary table
    if [ -f "$RESULTS_FILE" ]; then
        echo "=== CHANNEL SUMMARY TABLE ==="
        cat "$RESULTS_FILE"
    fi
}

# Check if MuMuDVB is built
if [ ! -f "src/mumudvb" ]; then
    echo "Error: MuMuDVB not built. Please run 'make' first."
    exit 1
fi

# Run the scan
run_scan

echo ""
echo "Complete scan finished!"
echo "Check the log and results files for detailed information."
