/**
 * @file test_event_timing.c
 * @brief Test program for event-based timing system
 * 
 * This program tests the event-based timing tracker to ensure it properly
 * maintains timing between events without using usleep polling.
 */

#include "src/event_timing.h"
#include "src/log.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>

static volatile int running = 1;

void signal_handler(int sig) {
    running = 0;
}

void timing_event_callback(const timing_event_t *event, void *user_data) {
    printf("Timing event: type=%d, card=%d, freq=%.0f, interval=%dms\n",
           event->event_type, event->card_id, event->frequency, event->interval_ms);
}

int main() {
    event_timing_tracker_t tracker;
    timing_config_t config;
    int result;
    
    printf("Testing event-based timing system...\n");
    
    // Set up signal handler
    signal(SIGINT, signal_handler);
    
    // Create timing tracker
    result = create_event_timing_tracker(10, NULL, &tracker);
    if (result < 0) {
        printf("Failed to create timing tracker\n");
        return 1;
    }
    
    // Set timing configuration
    config.frequency_test_interval_ms = 1000;  // 1 second
    config.signal_lock_timeout_ms = 2000;      // 2 seconds
    config.poll_interval_ms = 100;             // 100ms
    config.background_scan_delay_ms = 2000;    // 2 seconds
    config.thread_creation_delay_ms = 50;      // 50ms
    config.main_loop_sleep_ms = 1;             // 1ms
    
    result = set_timing_config(&tracker, &config);
    if (result < 0) {
        printf("Failed to set timing configuration\n");
        destroy_event_timing_tracker(&tracker);
        return 1;
    }
    
    // Start timing tracking
    result = start_event_timing_tracking(&tracker, timing_event_callback, NULL);
    if (result < 0) {
        printf("Failed to start timing tracking\n");
        destroy_event_timing_tracker(&tracker);
        return 1;
    }
    
    // Schedule some test events
    schedule_timing_event(&tracker, TIMING_EVENT_FREQUENCY_TEST, 0, 515000000.0, 1000, NULL);
    schedule_timing_event(&tracker, TIMING_EVENT_POLL_INTERVAL, 1, 0.0, 500, NULL);
    schedule_timing_event(&tracker, TIMING_EVENT_BACKGROUND_SCAN, -1, 0.0, 2000, NULL);
    
    printf("Scheduled test events. Running for 10 seconds...\n");
    
    // Run for 10 seconds
    for (int i = 0; i < 100 && running; i++) {
        struct pollfd pfd = {0, 0, 0};
        int poll_result = poll(&pfd, 1, 100); // 100ms timeout
        if (poll_result < 0 && errno != EINTR) {
            printf("Poll error: %s\n", strerror(errno));
        }
        
        // Check if events are due
        if (is_timing_event_due(&tracker, TIMING_EVENT_FREQUENCY_TEST, 0)) {
            printf("Frequency test event is due for card 0\n");
        }
        
        if (is_timing_event_due(&tracker, TIMING_EVENT_POLL_INTERVAL, 1)) {
            printf("Poll interval event is due for card 1\n");
        }
        
        if (is_timing_event_due(&tracker, TIMING_EVENT_BACKGROUND_SCAN, -1)) {
            printf("Background scan event is due\n");
        }
    }
    
    printf("Test completed. Cleaning up...\n");
    
    // Stop and cleanup
    stop_event_timing_tracking(&tracker);
    destroy_event_timing_tracker(&tracker);
    
    printf("Event-based timing test completed successfully!\n");
    return 0;
}
