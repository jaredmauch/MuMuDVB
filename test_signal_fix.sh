#!/bin/bash

# Test script to verify that MuMuDVB responds properly to Ctrl-C
# This script will start MuMuDVB and then send SIGINT to test signal handling

echo "Testing MuMuDVB signal handling fixes..."
echo "This test will start MuMuDVB and then send Ctrl-C to verify proper shutdown"
echo ""

# Check if we have a test configuration file
if [ ! -f "test_unified.conf" ]; then
    echo "Error: test_unified.conf not found. Please ensure you have a test configuration file."
    exit 1
fi

echo "Starting MuMuDVB with test configuration..."
echo "You should see the application start and then we'll send Ctrl-C in 5 seconds..."

# Start MuMuDVB in the background
./src/mumudvb -s -t -d -c test_unified.conf &
MUMUDVB_PID=$!

echo "MuMuDVB started with PID: $MUMUDVB_PID"

# Wait a bit for the application to start
sleep 5

echo ""
echo "Sending SIGINT (Ctrl-C) to MuMuDVB..."
kill -INT $MUMUDVB_PID

# Wait a bit for the application to shut down
sleep 3

# Check if the process is still running
if kill -0 $MUMUDVB_PID 2>/dev/null; then
    echo "WARNING: MuMuDVB is still running after SIGINT. This indicates a signal handling issue."
    echo "Sending SIGTERM to force shutdown..."
    kill -TERM $MUMUDVB_PID
    sleep 2
    
    if kill -0 $MUMUDVB_PID 2>/dev/null; then
        echo "ERROR: MuMuDVB is still running after SIGTERM. Force killing..."
        kill -9 $MUMUDVB_PID
        exit 1
    else
        echo "MuMuDVB shut down after SIGTERM (but not after SIGINT - signal handling still needs work)"
        exit 1
    fi
else
    echo "SUCCESS: MuMuDVB shut down properly after SIGINT (Ctrl-C)"
    echo "Signal handling fixes are working correctly!"
    exit 0
fi
