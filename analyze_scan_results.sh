#!/bin/bash

# Analyze MuMuDVB scan results and extract channel information
# This script helps parse existing logs to find channel discovery results

echo "=== MuMuDVB Scan Results Analyzer ==="
echo ""

# Function to analyze log file
analyze_log() {
    local log_file="$1"
    
    if [ ! -f "$log_file" ]; then
        echo "Error: Log file '$log_file' not found"
        echo "Usage: $0 <log_file>"
        exit 1
    fi
    
    echo "Analyzing log file: $log_file"
    echo ""
    
    # Extract frequency testing information
    echo "=== FREQUENCY TESTING SUMMARY ==="
    echo "Card | Frequency (Hz) | Status | Channels | Signal | SNR"
    echo "-----|----------------|--------|----------|--------|-----"
    
    # Look for tuning attempts
    grep -E "Testing card.*for frequency" "$log_file" | while read line; do
        local card=$(echo "$line" | sed 's/.*Testing card \([0-9]*\).*/\1/')
        local freq=$(echo "$line" | sed 's/.*frequency \([0-9]*\) Hz.*/\1/')
        echo "Card $card | $freq | TESTING | - | - | -"
    done
    
    echo ""
    echo "=== CHANNEL DISCOVERY RESULTS ==="
    
    # Look for channel discovery
    local channel_count=0
    grep -E "Diffusion.*channels" "$log_file" | while read line; do
        local channels=$(echo "$line" | sed 's/.*Diffusion \([0-9]*\) channels.*/\1/')
        channel_count=$((channel_count + channels))
        echo "Found: $channels channels"
    done
    
    echo ""
    echo "=== INDIVIDUAL CHANNELS ==="
    
    # Look for individual channel names
    grep -E "Channel number.*name" "$log_file" | while read line; do
        local channel_num=$(echo "$line" | sed 's/.*Channel number.*: *\([0-9]*\).*/\1/')
        local service_id=$(echo "$line" | sed 's/.*service id \([0-9]*\).*/\1/')
        local channel_name=$(echo "$line" | sed 's/.*name : "\([^"]*\)".*/\1/')
        echo "Channel $channel_num: $channel_name (SID: $service_id)"
    done
    
    echo ""
    echo "=== SIGNAL QUALITY INFORMATION ==="
    
    # Look for signal quality
    grep -E "(Signal strength|SNR|FE_STATUS)" "$log_file" | while read line; do
        echo "$line"
    done
    
    echo ""
    echo "=== CARD TUNING STATUS ==="
    
    # Look for successful tuning
    grep -E "(Card.*tuned|Tuning.*successful)" "$log_file" | while read line; do
        echo "$line"
    done
    
    echo ""
    echo "=== ERROR MESSAGES ==="
    
    # Look for errors
    grep -E "(ERROR|WARN|Failed)" "$log_file" | while read line; do
        echo "$line"
    done
}

# Function to create a summary table
create_summary_table() {
    local log_file="$1"
    local output_file="$2"
    
    echo "Creating summary table: $output_file"
    
    cat > "$output_file" << 'EOF'
=== MuMuDVB Frequency Scan Summary ===
Generated: $(date)

=== FREQUENCY TESTING RESULTS ===
EOF
    
    # Extract frequency testing results
    grep -E "Testing.*frequency.*Hz" "$log_file" | while read line; do
        local card=$(echo "$line" | sed 's/.*Testing card \([0-9]*\).*/\1/')
        local freq=$(echo "$line" | sed 's/.*frequency \([0-9]*\) Hz.*/\1/')
        echo "Card $card tested frequency $freq Hz" >> "$output_file"
    done
    
    echo "" >> "$output_file"
    echo "=== CHANNEL DISCOVERY ===" >> "$output_file"
    
    # Extract channel information
    grep -E "Diffusion.*channels" "$log_file" | while read line; do
        echo "$line" >> "$output_file"
    done
    
    echo "" >> "$output_file"
    echo "=== INDIVIDUAL CHANNELS ===" >> "$output_file"
    
    grep -E "Channel number.*name" "$log_file" | while read line; do
        echo "$line" >> "$output_file"
    done
    
    echo "Summary table created: $output_file"
}

# Main execution
if [ $# -eq 0 ]; then
    echo "Usage: $0 <log_file> [output_file]"
    echo ""
    echo "Examples:"
    echo "  $0 /var/log/mumudvb.log"
    echo "  $0 /tmp/mumudvb_scan.log /tmp/summary.txt"
    echo ""
    echo "This script analyzes MuMuDVB logs to extract:"
    echo "- Which cards tested which frequencies"
    echo "- Which frequencies had good locks"
    echo "- What channels were discovered"
    echo "- Signal quality information"
    exit 1
fi

LOG_FILE="$1"
OUTPUT_FILE="${2:-/tmp/mumudvb_analysis_$(date +%Y%m%d_%H%M%S).txt}"

analyze_log "$LOG_FILE"
create_summary_table "$LOG_FILE" "$OUTPUT_FILE"

echo ""
echo "Analysis complete!"
echo "Summary saved to: $OUTPUT_FILE"
