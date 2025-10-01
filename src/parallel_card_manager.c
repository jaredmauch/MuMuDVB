/*
 * MuMuDVB - Parallel Card Manager
 * 
 * This module implements parallel card management for testing all cards
 * against all frequencies simultaneously and tracking signal quality.
 *
 * (C) 2024 MuMuDVB Project
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#define _CRT_SECURE_NO_WARNINGS
#define _GNU_SOURCE  // For pthread_timedjoin_np

#include "mumudvb.h"
#include "log.h"
#include "errors.h"
#include "dvb.h"
#include "event_timing.h"
#include "main_thread_poll.h"
#include "tune.h"
#include "autoconf.h"
#include "unified_storage_adapter.h"
#include <pthread.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#ifndef DISABLE_DVB_API
#include <linux/dvb/frontend.h>
#endif

static char *log_module = "Parallel: ";

// card_frequency_result_t is now defined in event_timing.h

// Timing utility functions
static int get_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static int get_elapsed_time_ms(int start_time_ms) {
    return get_time_ms() - start_time_ms;
}

// Forward declaration for global_parallel_manager (will be defined after the type)

// Forward declarations
// Forward declaration for unified_channels.c function
extern int parse_transport_stream_with_autoconf(uint8_t *ts_data, ssize_t data_size, unified_card_t *card, double frequency);
static void store_frequency_channel_info(int card_id, double frequency, card_frequency_result_t *result);

// Forward declarations for idle card rotation functions
static int push_idle_card(int card_idx);
static int pop_idle_card(void);
static int peek_idle_card(void);
static int rotate_idle_card(int current_card_idx);

/** Structure to track card availability and usage */
typedef struct {
    int card_id;
    double current_frequency;
    int is_available;
    int is_tuning;
    int client_count;
    time_t last_used;
    pthread_mutex_t card_mutex;
} card_availability_t;

/** Structure to manage parallel card testing */
typedef struct {
    unified_channel_system_t *unified_system;
    card_frequency_result_t *results;
    int max_results;
    int current_result_count;
    pthread_mutex_t results_mutex;
    pthread_cond_t scan_complete;
    int scan_in_progress;
    int cards_completed_initial_scan;
    int initial_scan_complete;
    
    // Card availability tracking
    card_availability_t *card_availability;
    pthread_mutex_t availability_mutex;
    
    // Per-card autoconf instances for proper channel discovery
    auto_p_t *card_autoconf_instances;
    
    // Client request queue
    struct client_request {
        double frequency;
        int priority;
        time_t request_time;
        int assigned_card;
        int status; // 0=pending, 1=assigned, 2=completed, 3=failed
    } *client_requests;
    int max_client_requests;
    int current_client_requests;
    pthread_mutex_t client_requests_mutex;
    pthread_cond_t client_request_ready;
    
    // Thread management - one thread per card
    pthread_t *card_threads;  // Array of threads, one per card
    pthread_t client_processor_thread;
    volatile int shutdown_requested;
    pthread_mutex_t shutdown_mutex;
    
    // Idle card rotation system (LIFO)
    int *idle_card_stack;     // Stack of idle card indices (LIFO)
    int idle_stack_top;       // Top of idle card stack (-1 = empty)
    int idle_stack_size;      // Size of idle card stack
    pthread_mutex_t idle_rotation_mutex;  // Mutex for idle card rotation
} parallel_card_manager_t;

// Global parallel card manager (declared above)

// Note: We use the existing card_mutex in card_availability_t structures
// instead of separate frontend locks to maintain consistency with the
// parallel card manager's existing locking scheme

// Global event timing tracker
static event_timing_tracker_t *global_timing_tracker = NULL;

// Global parallel card manager
static parallel_card_manager_t *global_parallel_manager = NULL;

// Function to check if parallel operations are active (for timing system)
int is_parallel_operation_active(void) {
    if (!global_parallel_manager) {
        return 0;
    }
    
    pthread_mutex_lock(&global_parallel_manager->results_mutex);
    int is_active = global_parallel_manager->scan_in_progress;
    pthread_mutex_unlock(&global_parallel_manager->results_mutex);
    
    return is_active;
}

/** @brief Get card mutex for a specific card
 * @param card_id The card to get mutex for
 * @return Pointer to mutex, or NULL on error
 */
static pthread_mutex_t *get_card_mutex(int card_id)
{
    if (!global_parallel_manager) {
        log_message(log_module, MSG_ERROR, "Parallel manager not initialized");
        return NULL;
    }
    
    // Find the card in the availability array
    for (int i = 0; i < global_parallel_manager->unified_system->num_cards; i++) {
        if (global_parallel_manager->card_availability[i].card_id == card_id) {
            return &global_parallel_manager->card_availability[i].card_mutex;
        }
    }
    
    log_message(log_module, MSG_ERROR, "Card ID %d not found in parallel manager", card_id);
    return NULL;
}


/** @brief Initialize parallel card manager
 * @param unified_system The unified channel system
 * @return 0 on success, -1 on error
 */
int init_parallel_card_manager(unified_channel_system_t *unified_system)
{
    if (!unified_system || unified_system->num_cards == 0) {
        log_message(log_module, MSG_ERROR, "No unified system or cards available");
        return -1;
    }
    
    global_parallel_manager = malloc(sizeof(parallel_card_manager_t));
    if (!global_parallel_manager) {
        log_message(log_module, MSG_ERROR, "Memory allocation failed for parallel manager");
        return -1;
    }
    
    global_parallel_manager->unified_system = unified_system;
    global_parallel_manager->max_results = unified_system->num_cards * unified_system->num_frequencies;
    global_parallel_manager->current_result_count = 0;
    global_parallel_manager->scan_in_progress = 0;
    global_parallel_manager->cards_completed_initial_scan = 0;
    global_parallel_manager->initial_scan_complete = 0;
    global_parallel_manager->shutdown_requested = 0;
    
    // Initialize results array
    global_parallel_manager->results = malloc(sizeof(card_frequency_result_t) * global_parallel_manager->max_results);
    if (!global_parallel_manager->results) {
        log_message(log_module, MSG_ERROR, "Memory allocation failed for results array");
        free(global_parallel_manager);
        return -1;
    }
    
    // Initialize card availability tracking
    global_parallel_manager->card_availability = malloc(sizeof(card_availability_t) * unified_system->num_cards);
    if (!global_parallel_manager->card_availability) {
        log_message(log_module, MSG_ERROR, "Memory allocation failed for card availability array");
        free(global_parallel_manager->results);
        free(global_parallel_manager);
        return -1;
    }
    
    // Initialize per-card autoconf instances
    global_parallel_manager->card_autoconf_instances = malloc(sizeof(auto_p_t) * unified_system->num_cards);
    if (!global_parallel_manager->card_autoconf_instances) {
        log_message(log_module, MSG_ERROR, "Memory allocation failed for autoconf instances array");
        free(global_parallel_manager->card_availability);
        free(global_parallel_manager->results);
        free(global_parallel_manager);
        return -1;
    }
    
    // Initialize client request queue
    global_parallel_manager->max_client_requests = 100; // Allow up to 100 concurrent client requests
    global_parallel_manager->current_client_requests = 0;
    global_parallel_manager->client_requests = malloc(sizeof(struct client_request) * global_parallel_manager->max_client_requests);
    if (!global_parallel_manager->client_requests) {
        log_message(log_module, MSG_ERROR, "Memory allocation failed for client requests array");
        free(global_parallel_manager->card_availability);
        free(global_parallel_manager->results);
        free(global_parallel_manager);
        return -1;
    }
    
    // Initialize mutexes and conditions
    if (pthread_mutex_init(&global_parallel_manager->results_mutex, NULL) != 0) {
        log_message(log_module, MSG_ERROR, "Failed to initialize results mutex");
        goto cleanup_init;
    }
    
    if (pthread_cond_init(&global_parallel_manager->scan_complete, NULL) != 0) {
        log_message(log_module, MSG_ERROR, "Failed to initialize scan complete condition");
        goto cleanup_init;
    }
    
    if (pthread_mutex_init(&global_parallel_manager->availability_mutex, NULL) != 0) {
        log_message(log_module, MSG_ERROR, "Failed to initialize availability mutex");
        goto cleanup_init;
    }
    
    if (pthread_mutex_init(&global_parallel_manager->client_requests_mutex, NULL) != 0) {
        log_message(log_module, MSG_ERROR, "Failed to initialize client requests mutex");
        goto cleanup_init;
    }
    
    if (pthread_cond_init(&global_parallel_manager->client_request_ready, NULL) != 0) {
        log_message(log_module, MSG_ERROR, "Failed to initialize client request ready condition");
        goto cleanup_init;
    }
    
    if (pthread_mutex_init(&global_parallel_manager->shutdown_mutex, NULL) != 0) {
        log_message(log_module, MSG_ERROR, "Failed to initialize shutdown mutex");
        goto cleanup_init;
    }
    
    // Initialize event timing tracker
    global_timing_tracker = malloc(sizeof(event_timing_tracker_t));
    if (!global_timing_tracker) {
        log_message(log_module, MSG_ERROR, "Cannot allocate timing tracker");
        goto cleanup_init;
    }
    
    if (create_event_timing_tracker(64, NULL, global_timing_tracker) < 0) {
        log_message(log_module, MSG_ERROR, "Cannot create timing tracker");
        free(global_timing_tracker);
        global_timing_tracker = NULL;
        goto cleanup_init;
    }
    
    // Start timing tracking
    if (start_event_timing_tracking(global_timing_tracker, NULL, NULL) < 0) {
        log_message(log_module, MSG_ERROR, "Cannot start timing tracking");
        destroy_event_timing_tracker(global_timing_tracker);
        free(global_timing_tracker);
        global_timing_tracker = NULL;
        goto cleanup_init;
    }
    
    // Allocate card threads array
    global_parallel_manager->card_threads = malloc(sizeof(pthread_t) * unified_system->num_cards);
    if (!global_parallel_manager->card_threads) {
        log_message(log_module, MSG_ERROR, "Memory allocation failed for card threads");
        goto cleanup_init;
    }
    
    // Initialize all thread slots to 0 (invalid thread ID)
    memset(global_parallel_manager->card_threads, 0, sizeof(pthread_t) * unified_system->num_cards);
    
    // Initialize idle card rotation system
    global_parallel_manager->idle_stack_size = unified_system->num_cards;
    global_parallel_manager->idle_card_stack = malloc(sizeof(int) * unified_system->num_cards);
    if (!global_parallel_manager->idle_card_stack) {
        log_message(log_module, MSG_ERROR, "Memory allocation failed for idle card stack");
        goto cleanup_init;
    }
    global_parallel_manager->idle_stack_top = -1; // Empty stack initially
    
    // Initialize idle rotation mutex
    if (pthread_mutex_init(&global_parallel_manager->idle_rotation_mutex, NULL) != 0) {
        log_message(log_module, MSG_ERROR, "Failed to initialize idle rotation mutex");
        goto cleanup_init;
    }
    
    // Card mutexes are already initialized in the card availability structures above
    
    // Initialize card availability structures
    for (int i = 0; i < unified_system->num_cards; i++) {
        int card_id = unified_system->cards[i].card_id;
        global_parallel_manager->card_availability[i].card_id = card_id;
        global_parallel_manager->card_availability[i].current_frequency = 0.0;
        global_parallel_manager->card_availability[i].is_available = 1;
        global_parallel_manager->card_availability[i].is_tuning = 0;
        global_parallel_manager->card_availability[i].client_count = 0;
        global_parallel_manager->card_availability[i].last_used = 0;
        
        log_message(log_module, MSG_INFO, "Initializing card %d: card_id=%d (will use /dev/dvb/adapter%d/dvr0)", 
                    i, card_id, card_id);
        
        if (pthread_mutex_init(&global_parallel_manager->card_availability[i].card_mutex, NULL) != 0) {
            log_message(log_module, MSG_ERROR, "Failed to initialize card %d mutex", i);
            goto cleanup_init;
        }
        
        // Initialize autoconf instance for this card
        init_aconf_v(&global_parallel_manager->card_autoconf_instances[i]);
        global_parallel_manager->card_autoconf_instances[i].autoconfiguration = AUTOCONF_MODE_FULL;
        global_parallel_manager->card_autoconf_instances[i].autoconf_radios = 1;
        global_parallel_manager->card_autoconf_instances[i].autoconf_scrambled = 0;
        
        if (autoconf_init(&global_parallel_manager->card_autoconf_instances[i]) != 0) {
            log_message(log_module, MSG_ERROR, "Failed to initialize autoconf for card %d", i);
            goto cleanup_init;
        }
    }
    
    log_message(log_module, MSG_INFO, "Parallel card manager initialized for %d cards and %d frequencies", 
                unified_system->num_cards, unified_system->num_frequencies);
    
    return 0;

cleanup_init:
    // Cleanup on initialization failure
    if (global_timing_tracker) {
        stop_event_timing_tracking(global_timing_tracker);
        destroy_event_timing_tracker(global_timing_tracker);
        free(global_timing_tracker);
        global_timing_tracker = NULL;
    }
    
    if (global_parallel_manager) {
        if (global_parallel_manager->card_availability) {
            for (int i = 0; i < unified_system->num_cards; i++) {
                pthread_mutex_destroy(&global_parallel_manager->card_availability[i].card_mutex);
            }
            free(global_parallel_manager->card_availability);
        }
        if (global_parallel_manager->client_requests) {
            free(global_parallel_manager->client_requests);
        }
        if (global_parallel_manager->results) {
            free(global_parallel_manager->results);
        }
        pthread_mutex_destroy(&global_parallel_manager->results_mutex);
        pthread_cond_destroy(&global_parallel_manager->scan_complete);
        pthread_mutex_destroy(&global_parallel_manager->availability_mutex);
        pthread_mutex_destroy(&global_parallel_manager->client_requests_mutex);
        pthread_cond_destroy(&global_parallel_manager->client_request_ready);
        free(global_parallel_manager);
        global_parallel_manager = NULL;
    }
    return -1;
}

/** @brief Cleanup parallel card manager
 */
void cleanup_parallel_card_manager(void)
{
    if (global_parallel_manager) {
        log_message(log_module, MSG_INFO, "Signaling parallel card manager shutdown...");
        
        // Add Event-Timing message for parallel card manager cleanup
        if (global_timing_tracker) {
            log_message(log_module, MSG_INFO, "Event-Timing: Parallel card manager cleanup started");
        }
        
        // Signal shutdown to all threads (they will check this flag)
        pthread_mutex_lock(&global_parallel_manager->shutdown_mutex);
        global_parallel_manager->shutdown_requested = 1;
        pthread_mutex_unlock(&global_parallel_manager->shutdown_mutex);
        
        // Signal condition variables to wake up waiting threads
        pthread_cond_signal(&global_parallel_manager->client_request_ready);
        
        // Wait for all card threads to complete with timeout
        if (global_parallel_manager->card_threads) {
            // Use mutex to protect card_threads array during cleanup
            pthread_mutex_lock(&global_parallel_manager->shutdown_mutex);
            
            for (int i = 0; i < global_parallel_manager->unified_system->num_cards; i++) {
                pthread_t thread_id = global_parallel_manager->card_threads[i];
                
                // Check if thread is valid (not zero) and not the current thread
                if (thread_id != 0 && !pthread_equal(thread_id, pthread_self())) {
                    log_message(log_module, MSG_DEBUG, "Waiting for card %d thread to complete (thread_id=%lu)...", i, (unsigned long)thread_id);
                    
                    // Validate thread ID is reasonable (not obviously corrupted)
                    // Check for common corruption patterns like repeated bytes
                    unsigned long tid = (unsigned long)thread_id;
                    if (tid < 0x1000 || tid > 0x7fffffffffff || 
                        (tid & 0xFFFFFFFF) == (tid >> 32) ||  // Check for repeated 32-bit patterns
                        tid == 0x6464646464646461) {           // Check for specific corruption pattern seen in crash
                        log_message(log_module, MSG_ERROR, "Card %d thread ID appears corrupted: %lu (0x%lx), skipping join", 
                                   i, tid, tid);
                        continue;
                    }
                    
                    // Use timed join with 2 second timeout to avoid hanging
                    struct timespec timeout;
                    clock_gettime(CLOCK_REALTIME, &timeout);
                    timeout.tv_sec += 2; // 2 second timeout
                    
                    int result = pthread_timedjoin_np(thread_id, NULL, &timeout);
                    if (result == 0) {
                        log_message(log_module, MSG_DEBUG, "Card %d thread completed successfully", i);
                    } else if (result == ETIMEDOUT) {
                        log_message(log_module, MSG_WARN, "Card %d thread did not complete in time, continuing cleanup", i);
                    } else {
                        log_message(log_module, MSG_WARN, "Card %d thread join failed with error %d (%s), continuing cleanup", 
                                   i, result, strerror(result));
                    }
                } else {
                    log_message(log_module, MSG_DEBUG, "Skipping card %d thread (invalid or current thread)", i);
                }
            }
            
            // Unlock the mutex after all thread cleanup is complete
            pthread_mutex_unlock(&global_parallel_manager->shutdown_mutex);
        }
        
        // Signal the client processor thread to wake up and check shutdown
        pthread_cond_signal(&global_parallel_manager->client_request_ready);
        
        // Wait for client processor thread with longer timeout
        if (global_parallel_manager->client_processor_thread != 0 && 
            !pthread_equal(global_parallel_manager->client_processor_thread, pthread_self())) {
            log_message(log_module, MSG_DEBUG, "Waiting for client processor thread to complete...");
            
            // Use timed join with 5 second timeout to allow thread to finish
            struct timespec timeout;
            clock_gettime(CLOCK_REALTIME, &timeout);
            timeout.tv_sec += 5; // 5 second timeout
            
            int result = pthread_timedjoin_np(global_parallel_manager->client_processor_thread, NULL, &timeout);
            if (result != 0) {
                log_message(log_module, MSG_WARN, "Client processor thread did not complete in time (result=%d), continuing cleanup", result);
            } else {
                log_message(log_module, MSG_DEBUG, "Client processor thread completed successfully");
            }
        }
        
        // Cleanup card availability structures
        if (global_parallel_manager->card_availability) {
            for (int i = 0; i < global_parallel_manager->unified_system->num_cards; i++) {
                pthread_mutex_destroy(&global_parallel_manager->card_availability[i].card_mutex);
            }
            free(global_parallel_manager->card_availability);
        }
        
        // Cleanup client requests
        if (global_parallel_manager->client_requests) {
            free(global_parallel_manager->client_requests);
        }
        
        // Cleanup results
        if (global_parallel_manager->results) {
            free(global_parallel_manager->results);
        }
        
        // Cleanup autoconf instances
        if (global_parallel_manager->card_autoconf_instances) {
            for (int i = 0; i < global_parallel_manager->unified_system->num_cards; i++) {
                autoconf_freeing(&global_parallel_manager->card_autoconf_instances[i]);
            }
            free(global_parallel_manager->card_autoconf_instances);
        }
        
        // Cleanup card threads array
        if (global_parallel_manager->card_threads) {
            free(global_parallel_manager->card_threads);
        }
        
        // Cleanup idle card rotation system
        if (global_parallel_manager->idle_card_stack) {
            free(global_parallel_manager->idle_card_stack);
        }
        pthread_mutex_destroy(&global_parallel_manager->idle_rotation_mutex);
        
        // Cleanup mutexes and conditions
        pthread_mutex_destroy(&global_parallel_manager->results_mutex);
        pthread_cond_destroy(&global_parallel_manager->scan_complete);
        pthread_mutex_destroy(&global_parallel_manager->availability_mutex);
        pthread_mutex_destroy(&global_parallel_manager->client_requests_mutex);
        pthread_cond_destroy(&global_parallel_manager->client_request_ready);
        pthread_mutex_destroy(&global_parallel_manager->shutdown_mutex);
        
        // Card mutexes are cleaned up with the card availability structures
        
        free(global_parallel_manager);
        global_parallel_manager = NULL;
        
        // Add Event-Timing message for parallel card manager cleanup completion
        if (global_timing_tracker) {
            log_message(log_module, MSG_INFO, "Event-Timing: Parallel card manager cleanup completed");
        }
        
        log_message(log_module, MSG_INFO, "Parallel card manager shutdown signaled");
    }
}

/** @brief Process transport stream using the same approach as main system
 * @param card Pointer to unified card structure
 * @param frequency Frequency being scanned
 * @return Number of channels found
 */
static int process_transport_stream_simple(unified_card_t *card, double frequency)
{
    int card_id = card->card_id;
    int channel_count = 0;
    int fd_dvr = -1;
    
    // Use the already initialized DVR device from the card's fds structure
    // This is the same approach as the main system - use the properly initialized device
    if (!card->fds || card->fds->fd_dvr <= 0) {
        log_message(log_module, MSG_INFO, "Card %d DVR device not properly initialized (fds=%p, fd_dvr=%d)", 
                    card_id, card->fds, card->fds ? card->fds->fd_dvr : -1);
        return 0;
    }
    
    fd_dvr = card->fds->fd_dvr;
    log_message(log_module, MSG_INFO, "Card %d using initialized DVR device (fd=%d), starting TS processing for frequency %.1f MHz", 
                card_id, fd_dvr, frequency / 1000000.0);
    
    // Reset autoconf state for this frequency - this is critical!
    // When tuning to a new frequency on the same card, we need to reset all autoconf state
    
    // Ensure auto_p is properly initialized for this card
    if (!card->auto_p || (uintptr_t)card->auto_p < 0x1000) { // Check for obviously invalid pointers
        log_message(log_module, MSG_INFO, "Card %d auto_p not initialized, initializing now", card_id);
        
        // Allocate autoconf structure for this card
        card->auto_p = malloc(sizeof(auto_p_t));
        if (!card->auto_p) {
            log_message(log_module, MSG_ERROR, "Memory allocation failed for autoconf on card %d", card_id);
            return 0;
        }
        
        // Initialize autoconf structure for this card
        card->auto_p->autoconfiguration = AUTOCONF_MODE_FULL;
        card->auto_p->autoconf_radios = 1;
        card->auto_p->autoconf_scrambled = 0;
        card->auto_p->time_start_autoconfiguration = time(NULL);
        
        // Initialize autoconf structures
        if (autoconf_init(card->auto_p) != 0) {
            log_message(log_module, MSG_ERROR, "Failed to initialize autoconf for card %d", card_id);
            free(card->auto_p);
            card->auto_p = NULL;
            return 0;
        }
        
        log_message(log_module, MSG_INFO, "Card %d auto_p initialized successfully", card_id);
    }
    
    if (card->auto_p) {
        // Reset PAT state
        card->auto_p->pat_version = -1;
        card->auto_p->pat_all_sections_seen = 0;
        card->auto_p->pat_need_update = 0;
        memset(card->auto_p->pat_sections_seen, 0, sizeof(card->auto_p->pat_sections_seen));
        
        // Reset SDT state
        card->auto_p->sdt_version = -1;
        card->auto_p->sdt_all_sections_seen = 0;
        card->auto_p->sdt_need_update = 0;
        memset(card->auto_p->sdt_sections_seen, 0, sizeof(card->auto_p->sdt_sections_seen));
        
        // Reset PSIP state
        card->auto_p->psip_version = -1;
        card->auto_p->psip_all_sections_seen = 0;
        card->auto_p->psip_need_update = 0;
        memset(card->auto_p->psip_sections_seen, 0, sizeof(card->auto_p->psip_sections_seen));
        
        // Reset NIT state
        card->auto_p->nit_version = -1;
        card->auto_p->nit_all_sections_seen = 0;
        card->auto_p->nit_need_update = 0;
        memset(card->auto_p->nit_sections_seen, 0, sizeof(card->auto_p->nit_sections_seen));
        
        // Reset channel state
        card->auto_p->need_filter_chan_update = 0;
        
        log_message(log_module, MSG_INFO, "Card %d reset autoconf state for frequency %.1f MHz", card_id, frequency / 1000000.0);
    }
    
    // Read 6 seconds of transport stream data and process it with existing parser
    uint8_t ts_buffer[188 * 32000]; // Buffer for ~6 seconds at ~5Mbps (32000 packets)
    ssize_t total_bytes_read = 0;
    ssize_t bytes_read;
    
    // Timer-based approach: read for 6 seconds from signal lock
    int acquisition_start_time = get_time_ms();
    int acquisition_timeout_ms = TIMING_CHANNEL_ACQUISITION_DELAY_MS; // 6000ms from signal lock
    
    log_message(log_module, MSG_INFO, "Card %d reading 6 seconds of TS data for channel discovery", card_id);
    
    // Read data for 6 seconds
    while (get_elapsed_time_ms(acquisition_start_time) < acquisition_timeout_ms) {
        bytes_read = read(fd_dvr, ts_buffer + total_bytes_read, sizeof(ts_buffer) - total_bytes_read);
        
        if (bytes_read > 0) {
            total_bytes_read += bytes_read;
            int elapsed_ms = get_elapsed_time_ms(acquisition_start_time);
            log_message(log_module, MSG_DEBUG, "Card %d read %zd bytes (total: %zd, elapsed: %d ms)", 
                       card_id, bytes_read, total_bytes_read, elapsed_ms);
        } else if (bytes_read == 0) {
            // No data available, poll on DVR file descriptor instead of usleep
            struct pollfd pfd = {fd_dvr, POLLIN, 0};
            int poll_result = poll(&pfd, 1, 10); // 10ms timeout
            if (poll_result < 0) {
                if (errno == EINTR) {
                    continue; // Interrupted, retry
                }
                log_message(log_module, MSG_ERROR, "Card %d DVR poll error: %s", card_id, strerror(errno));
                break;
            }
            if (poll_result == 0) {
                // Timeout, continue to next iteration
                continue;
            }
        } else {
            // Error reading from DVR
            if (errno != EAGAIN) {
                log_message(log_module, MSG_ERROR, "Card %d DVR read error: %s", card_id, strerror(errno));
                break;
            }
            // EAGAIN - no data available, poll on DVR file descriptor instead of usleep
            struct pollfd pfd = {fd_dvr, POLLIN, 0};
            int poll_result = poll(&pfd, 1, 10); // 10ms timeout
            if (poll_result < 0) {
                if (errno == EINTR) {
                    continue; // Interrupted, retry
                }
                log_message(log_module, MSG_ERROR, "Card %d DVR poll error: %s", card_id, strerror(errno));
                break;
            }
            if (poll_result == 0) {
                // Timeout, continue to next iteration
                continue;
            }
        }
    }
    
    // Process the collected buffer using the existing parser
    if (total_bytes_read > 0) {
        log_message(log_module, MSG_INFO, "Card %d collected %zd bytes, processing with existing parser", 
                   card_id, total_bytes_read);
        
        // Use the existing parse_transport_stream_with_autoconf function from unified_channels.c
        channel_count = parse_transport_stream_with_autoconf(ts_buffer, total_bytes_read, card, frequency);
        
        log_message(log_module, MSG_INFO, "Card %d parser found %d channels", card_id, channel_count);
    } else {
        log_message(log_module, MSG_WARN, "Card %d no data collected during 6-second read", card_id);
    }
    
    // Note: We don't close fd_dvr here because it's the card's initialized DVR device
    // The card's cleanup function will handle closing it properly
    return channel_count;
}

/** @brief Test a single card/frequency combination
 * @param card_id The card ID to test
 * @param frequency The frequency to test
 * @param result Output structure for results
 * @param is_initial_scan Whether this is for initial scan (no assignment) or client request
 * @return 0 on success, -1 on error
 */
static int test_card_frequency_parallel(int card_id, double frequency, card_frequency_result_t *result, int is_initial_scan)
{
    if (!result) {
        return -1;
    }
    
    // Initialize result structure
    memset(result, 0, sizeof(card_frequency_result_t));
    result->card_id = card_id;
    result->frequency = frequency;
    result->test_time = time(NULL);
    result->error_code = 0;
    strcpy(result->error_message, "No error");
    
    // Initialize timing measurements
    int test_start_time = get_time_ms();
    result->tune_start_time_ms = test_start_time;
    result->tune_complete_time_ms = 0;
    result->signal_lock_time_ms = 0;
    result->carrier_lock_time_ms = 0;
    result->viterbi_lock_time_ms = 0;
    result->sync_lock_time_ms = 0;
    result->full_lock_time_ms = 0;
    result->lock_achieved_time_ms = 0;
    result->ts_stabilization_time_ms = 0;
    result->channel_acquisition_time_ms = 0;
    result->total_test_time_ms = 0;
    result->lock_time_ms = 0; // Legacy field
    
    // Always check if card is in use by any system (main or parallel)
    int card_in_use = is_card_in_use(card_id);
    log_message(log_module, MSG_INFO, "card-%d is_card_in_use() returned: %d", card_id, card_in_use);
    
    if (card_in_use) {
        log_message(log_module, MSG_INFO, "card-%d is currently in use, skipping frequency %.1f MHz", 
                    card_id, frequency/1000000.0);
        result->status = 0; // Failed - card busy
        result->error_code = -1;
        strcpy(result->error_message, "Card is currently in use by another system");
        return -1;
    }
    
    // For non-initial scans, also check card availability and reserve the card
    if (!is_initial_scan) {
        // Check if card is available for testing
        if (!is_card_available_for_testing(card_id)) {
            log_message(log_module, MSG_DEBUG, "card-%d is not available for testing frequency %.1f MHz", 
                        card_id, frequency/1000000.0);
            result->status = 0; // Failed - card busy
            result->error_code = -1;
            strcpy(result->error_message, "Card is busy/unavailable");
            return -1;
        }
        
        // Reserve the card for testing
        if (assign_parallel_card_to_frequency(card_id, frequency) != 0) {
            log_message(log_module, MSG_DEBUG, "Failed to reserve card-%d for testing frequency %.1f MHz", 
                        card_id, frequency/1000000.0);
            result->status = 0; // Failed - could not reserve
            result->error_code = -2;
            strcpy(result->error_message, "Failed to reserve card");
            return -1;
        }
    }
    
    // Create tuning parameters for this card/frequency
    tune_p_t tune_params;
    memset(&tune_params, 0, sizeof(tune_p_t));
    tune_params.card = card_id;
    tune_params.tuner = 0;
    tune_params.freq = frequency;
    tune_params.delivery_system = SYS_UNDEFINED; // Auto-detect
    
    // Open frontend for this specific card (each card needs its own FD)
    char frontend_path[256];
    snprintf(frontend_path, sizeof(frontend_path), "/dev/dvb/adapter%d/frontend0", card_id);
    
    log_message(log_module, MSG_INFO, "card-%d attempting to open frontend: %s", card_id, frontend_path);
    int fd_frontend = open(frontend_path, O_RDWR | O_NONBLOCK);
    if (fd_frontend < 0) {
        log_message(log_module, MSG_ERROR, "card-%d Cannot open frontend: %s (errno=%d)", card_id, strerror(errno), errno);
        // Release the card since we couldn't open the frontend (only if it was assigned)
        if (!is_initial_scan) {
            release_parallel_card_from_frequency(card_id, frequency);
        }
        result->status = 0; // Failed
        result->error_code = errno;
        snprintf(result->error_message, sizeof(result->error_message), "Cannot open frontend: %s", strerror(errno));
        return -1;
    }
    
    log_message(log_module, MSG_INFO, "card-%d successfully opened frontend (fd=%d)", card_id, fd_frontend);
    
    // Register card usage before tuning
    register_card_usage(card_id, frequency, "parallel_system_tuning");
    log_message(log_module, MSG_INFO, "card-%d registered for parallel system tuning", card_id);
    
    // Check if card is now marked as in use
    int card_in_use_after_register = is_card_in_use(card_id);
    log_message(log_module, MSG_INFO, "card-%d is_card_in_use() after register: %d", card_id, card_in_use_after_register);
    
    // Attempt to tune using non-blocking approach (bypass tune_it() blocking loop)
    log_message(log_module, MSG_INFO, "card-%d attempting to tune to frequency %.1f MHz...", 
                card_id, frequency/1000000.0);
    
    // Record tuning start time
    int tune_start_time = get_time_ms();
    result->tune_start_time_ms = tune_start_time;
    
    // Get card mutex to protect ioctl operations
    pthread_mutex_t *card_mutex = get_card_mutex(card_id);
    if (!card_mutex) {
        log_message(log_module, MSG_ERROR, "card-%d failed to get card mutex", card_id);
        close(fd_frontend);
        unregister_card_usage(card_id, "parallel_system_tuning");
        result->status = 0;
        result->error_code = -1;
        strcpy(result->error_message, "Failed to get card mutex");
        return -1;
    }
    
    // Lock the card for this operation
    pthread_mutex_lock(card_mutex);
    log_message(log_module, MSG_DEBUG, "card-%d acquired card mutex", card_id);
    
    // Get frontend info
    log_message(log_module, MSG_INFO, "card-%d calling FE_GET_INFO...", card_id);
    struct dvb_frontend_info fe_info;
    int res = ioctl(fd_frontend, FE_GET_INFO, &fe_info);
    if (res < 0) {
        log_message(log_module, MSG_ERROR, "card-%d FE_GET_INFO failed: %s (errno=%d)", card_id, strerror(errno), errno);
        pthread_mutex_unlock(card_mutex);
        close(fd_frontend);
        unregister_card_usage(card_id, "parallel_system_tuning");
        result->status = 0;
        result->error_code = errno;
        snprintf(result->error_message, sizeof(result->error_message), "Cannot get frontend info: %s", strerror(errno));
        return -1;
    }
    
    log_message(log_module, MSG_INFO, "card-%d FE_GET_INFO successful, Using DVB card \"%s\" tuner %d", 
                card_id, fe_info.name, tune_params.tuner);
    
    // Set up frontend parameters (simplified version of tune_it logic)
    struct dvb_frontend_parameters feparams;
    memset(&feparams, 0, sizeof(struct dvb_frontend_parameters));
    feparams.frequency = (__u32)frequency;
    feparams.inversion = INVERSION_AUTO;
    
    // For ATSC (which this appears to be based on the logs)
    feparams.u.vsb.modulation = VSB_8;
    
    // Empty the event queue before tuning (same as main system)
    log_message(log_module, MSG_INFO, "card-%d clearing event queue...", card_id);
    struct dvb_frontend_event event;
    int event_count = 0;
    while(1) {
        // Check for interrupt signal
        if (get_interrupted()) {
            log_message(log_module, MSG_INFO, "card-%d tuning interrupted by signal", card_id);
            pthread_mutex_unlock(card_mutex);
            close(fd_frontend);
            unregister_card_usage(card_id, "parallel_system_tuning");
            result->status = 0; // Failed
            result->error_code = -3;
            strcpy(result->error_message, "Interrupted during event queue clearing");
            return -1;
        }
        
        if (ioctl(fd_frontend, FE_GET_EVENT, &event) < 0) { // EMPTY THE EVENT QUEUE
            break;
        }
        event_count++;
    }
    log_message(log_module, MSG_INFO, "card-%d cleared %d events from queue", card_id, event_count);
    
    // Log frontend parameters before tuning
    log_message(log_module, MSG_INFO, "card-%d tuning parameters: frequency=%u, inversion=%d, modulation=%d", 
                card_id, feparams.frequency, feparams.inversion, feparams.u.vsb.modulation);
    
    // Check for interrupt before tuning
    if (get_interrupted()) {
        log_message(log_module, MSG_INFO, "card-%d interrupted before tuning, skipping", card_id);
        pthread_mutex_unlock(card_mutex);
        close(fd_frontend);
        unregister_card_usage(card_id, "parallel_system_tuning");
        if (!is_initial_scan) {
            release_parallel_card_from_frequency(card_id, frequency);
        }
        result->status = 0; // Failed
        result->error_code = -3;
        strcpy(result->error_message, "Interrupted before tuning");
        return -1;
    }
    
    // Use the same tuning method as the main system - this includes the blocking wait for lock
    // For parallel system, we need to ensure tuning timeouts don't interfere
    log_message(log_module, MSG_INFO, "card-%d calling tune_it() (same as main system)...", card_id);
    
    // Temporarily disable tuning timeout for parallel operations to prevent race conditions
    // The parallel system manages its own timing and doesn't use the global card_tuned variable
    extern event_timing_tracker_t *global_unified_timing_tracker;
    int timeout_was_active = 0;
    if (global_unified_timing_tracker) {
        timeout_was_active = is_tuning_timeout_active(global_unified_timing_tracker, card_id);
        if (timeout_was_active) {
            cancel_tuning_timeout(global_unified_timing_tracker, card_id);
            log_message(log_module, MSG_DEBUG, "card-%d temporarily disabled tuning timeout for parallel operation", card_id);
        }
    }
    
    // Also clear any existing interrupt signals that might have been set by timing system
    // This prevents false interrupts during parallel operations
    if (get_interrupted()) {
        log_message(log_module, MSG_DEBUG, "card-%d clearing existing interrupt signal before parallel tuning", card_id);
        // Note: We can't clear the interrupt flag directly, but we'll be more lenient during parallel operations
    }
    
    int tune_result = tune_it(fd_frontend, &tune_params);
    
    // Record tuning completion time
    result->tune_complete_time_ms = get_time_ms();
    int tune_duration = get_elapsed_time_ms(tune_start_time);
    log_message(log_module, MSG_INFO, "card-%d tune_it() completed in %d ms with result: %d", 
                card_id, tune_duration, tune_result);
    
    // Restore tuning timeout if it was active
    if (global_unified_timing_tracker && timeout_was_active) {
        start_tuning_timeout(global_unified_timing_tracker, card_id, 6); // 6 second timeout
        log_message(log_module, MSG_DEBUG, "card-%d restored tuning timeout after parallel operation", card_id);
    }
    
    if (tune_result != 0) {
        log_message(log_module, MSG_INFO, "card-%d tune_it() failed with error code %d", card_id, tune_result);
        pthread_mutex_unlock(card_mutex);
        close(fd_frontend);
        // Unregister card usage
        unregister_card_usage(card_id, "parallel_system_tuning");
        // Release the card since tuning failed (only if it was assigned)
        if (!is_initial_scan) {
            release_parallel_card_from_frequency(card_id, frequency);
        }
        result->status = 0; // Failed
        result->error_code = tune_result;
        snprintf(result->error_message, sizeof(result->error_message), "Tuning failed (error code: %d)", tune_result);
        log_message(log_module, MSG_INFO, "card-%d cannot tune to frequency %.1f MHz (tune_result=%d)", 
                    card_id, frequency/1000000.0, tune_result);
        return -1;
    }
    
    // Check if we achieved lock within the 15-second timeout
    // tune_it() returns 0 even on timeout, so we need to check FE status
    fe_status_t fe_status;
    if (ioctl(fd_frontend, FE_READ_STATUS, &fe_status) >= 0) {
        if (!(fe_status & FE_HAS_LOCK)) {
            log_message(log_module, MSG_INFO, "card-%d timeout after 15 seconds on frequency %.1f Hz - marking as timeout", 
                        card_id, frequency/1000000.0);
            pthread_mutex_unlock(card_mutex);
            close(fd_frontend);
            // Unregister card usage
            unregister_card_usage(card_id, "parallel_system_tuning");
            // Release the card since tuning failed (only if it was assigned)
            if (!is_initial_scan) {
                release_parallel_card_from_frequency(card_id, frequency);
            }
            result->status = 2; // Timeout
            result->error_code = -2;
            strcpy(result->error_message, "15-second lock timeout");
            result->fe_status_flags = fe_status; // Store the last FE status flags
            return -1;
        }
    } else {
        log_message(log_module, MSG_ERROR, "card-%d cannot read final frontend status (errno: %d)", card_id, errno);
        pthread_mutex_unlock(card_mutex);
        close(fd_frontend);
        // Unregister card usage
        unregister_card_usage(card_id, "parallel_system_tuning");
        // Release the card since tuning failed (only if it was assigned)
        if (!is_initial_scan) {
            release_parallel_card_from_frequency(card_id, frequency);
        }
        result->status = 0; // Failed
        result->error_code = errno;
        snprintf(result->error_message, sizeof(result->error_message), "Cannot read frontend status: %s", strerror(errno));
        return -1;
    }
    
    log_message(log_module, MSG_INFO, "card-%d tune_it() completed successfully - lock achieved!", card_id);
    
    // Now we need to track the detailed lock phases with proper timing
    // Each stage can take up to 6 seconds to settle
    log_message(log_module, MSG_DEBUG, "card-%d starting detailed lock phase detection...", card_id);
    
    // Track each lock stage with individual timeouts
    int stage_timeout_ms = TIMING_SIGNAL_LOCK_TIMEOUT_MS; // 6 seconds per stage
    int poll_interval_ms = TIMING_POLL_INTERVAL_MS; // 50ms polling
    
    // First, check if all flags are already set (frontend reports all at once)
    if (ioctl(fd_frontend, FE_READ_STATUS, &fe_status) >= 0) {
        if ((fe_status & FE_HAS_SIGNAL) && (fe_status & FE_HAS_CARRIER) && 
            (fe_status & FE_HAS_VITERBI) && (fe_status & FE_HAS_SYNC) && (fe_status & FE_HAS_LOCK)) {
            // All flags set at once - proceed immediately
            int lock_duration = get_elapsed_time_ms(tune_start_time);
            result->signal_lock_time_ms = lock_duration;
            result->carrier_lock_time_ms = lock_duration;
            result->viterbi_lock_time_ms = lock_duration;
            result->sync_lock_time_ms = lock_duration;
            result->full_lock_time_ms = lock_duration;
            result->lock_time_ms = lock_duration; // Legacy field
            result->lock_achieved_time_ms = get_time_ms();
            result->fe_status_flags = fe_status;
            
            log_message(log_module, MSG_INFO, "card-%d      FE_HAS_SIGNAL : found something above the noise level", card_id);
            log_message(log_module, MSG_INFO, "card-%d      FE_HAS_CARRIER : found a DVB signal", card_id);
            log_message(log_module, MSG_INFO, "card-%d      FE_HAS_VITERBI : FEC is stable", card_id);
            log_message(log_module, MSG_INFO, "card-%d      FE_HAS_SYNC : found sync bytes", card_id);
            log_message(log_module, MSG_INFO, "card-%d      FE_HAS_LOCK : everything's working...", card_id);
            
            // Skip to signal quality reading
            goto signal_quality;
        }
    }
    
    // Wait for FE_HAS_SIGNAL (up to 6 seconds)
    log_message(log_module, MSG_DEBUG, "card-%d waiting for FE_HAS_SIGNAL...", card_id);
    int signal_start = get_time_ms();
    while (get_elapsed_time_ms(signal_start) < stage_timeout_ms) {
        if (ioctl(fd_frontend, FE_READ_STATUS, &fe_status) >= 0 && (fe_status & FE_HAS_SIGNAL)) {
            result->signal_lock_time_ms = get_elapsed_time_ms(tune_start_time);
            log_message(log_module, MSG_INFO, "card-%d      FE_HAS_SIGNAL : found something above the noise level", card_id);
            break;
        }
        if (event_sleep_interruptible(MS_TO_US(poll_interval_ms)) < 0) {
            log_message(log_module, MSG_INFO, "card-%d signal detection interrupted", card_id);
            return -1;
        }
    }
    
    // Wait for FE_HAS_CARRIER (up to 6 seconds)
    log_message(log_module, MSG_DEBUG, "card-%d waiting for FE_HAS_CARRIER...", card_id);
    int carrier_start = get_time_ms();
    while (get_elapsed_time_ms(carrier_start) < stage_timeout_ms) {
        if (ioctl(fd_frontend, FE_READ_STATUS, &fe_status) >= 0 && (fe_status & FE_HAS_CARRIER)) {
            result->carrier_lock_time_ms = get_elapsed_time_ms(tune_start_time);
            log_message(log_module, MSG_INFO, "card-%d      FE_HAS_CARRIER : found a DVB signal", card_id);
            break;
        }
        if (event_sleep_interruptible(MS_TO_US(poll_interval_ms)) < 0) {
            log_message(log_module, MSG_INFO, "card-%d carrier detection interrupted", card_id);
            return -1;
        }
    }
    
    // Wait for FE_HAS_VITERBI (up to 6 seconds)
    log_message(log_module, MSG_DEBUG, "card-%d waiting for FE_HAS_VITERBI...", card_id);
    int viterbi_start = get_time_ms();
    while (get_elapsed_time_ms(viterbi_start) < stage_timeout_ms) {
        if (ioctl(fd_frontend, FE_READ_STATUS, &fe_status) >= 0 && (fe_status & FE_HAS_VITERBI)) {
            result->viterbi_lock_time_ms = get_elapsed_time_ms(tune_start_time);
            log_message(log_module, MSG_INFO, "card-%d      FE_HAS_VITERBI : FEC is stable", card_id);
            break;
        }
        if (event_sleep_interruptible(MS_TO_US(poll_interval_ms)) < 0) {
            log_message(log_module, MSG_INFO, "card-%d viterbi detection interrupted", card_id);
            return -1;
        }
    }
    
    // Wait for FE_HAS_SYNC (up to 6 seconds)
    log_message(log_module, MSG_DEBUG, "card-%d waiting for FE_HAS_SYNC...", card_id);
    int sync_start = get_time_ms();
    while (get_elapsed_time_ms(sync_start) < stage_timeout_ms) {
        if (ioctl(fd_frontend, FE_READ_STATUS, &fe_status) >= 0 && (fe_status & FE_HAS_SYNC)) {
            result->sync_lock_time_ms = get_elapsed_time_ms(tune_start_time);
            log_message(log_module, MSG_INFO, "card-%d      FE_HAS_SYNC : found sync bytes", card_id);
            break;
        }
        if (event_sleep_interruptible(MS_TO_US(poll_interval_ms)) < 0) {
            log_message(log_module, MSG_INFO, "card-%d sync detection interrupted", card_id);
            return -1;
        }
    }
    
    // Wait for FE_HAS_LOCK (up to 6 seconds)
    log_message(log_module, MSG_DEBUG, "card-%d waiting for FE_HAS_LOCK...", card_id);
    int lock_start = get_time_ms();
    while (get_elapsed_time_ms(lock_start) < stage_timeout_ms) {
        if (ioctl(fd_frontend, FE_READ_STATUS, &fe_status) >= 0 && (fe_status & FE_HAS_LOCK)) {
            result->full_lock_time_ms = get_elapsed_time_ms(tune_start_time);
            result->lock_time_ms = result->full_lock_time_ms; // Legacy field
            result->lock_achieved_time_ms = get_time_ms(); // Store absolute time when lock was achieved
            log_message(log_module, MSG_INFO, "card-%d      FE_HAS_LOCK : everything's working...", card_id);
            break;
        }
        if (event_sleep_interruptible(MS_TO_US(poll_interval_ms)) < 0) {
            log_message(log_module, MSG_INFO, "card-%d lock detection interrupted", card_id);
            return -1;
        }
    }
    
    // Store final status flags
    result->fe_status_flags = fe_status;
    
signal_quality:
    // Get signal strength and quality (same as main process)
    uint16_t signal_strength = 0;
    uint16_t snr = 0;
    ioctl(fd_frontend, FE_READ_SIGNAL_STRENGTH, &signal_strength);
    ioctl(fd_frontend, FE_READ_SNR, &snr);
    
    result->signal_strength = signal_strength;
    result->snr = snr;
    result->ber = 0; // BER not easily available from frontend status
    
    log_message(log_module, MSG_INFO, "card-%d has signal lock on frequency %.1f MHz (strength: %d, SNR: %d)", 
                card_id, frequency/1000000.0, signal_strength, snr);
    
    // Mark as success since we achieved lock (same as main system)
    result->status = 1; // Success - we have lock
    
    // Get final frontend parameters after lock (same as main system)
    struct dvb_frontend_parameters parameters;
    int status;
    do {
        status = ioctl(fd_frontend, FE_GET_FRONTEND, &parameters);
    } while (status == -1 && errno == EINTR);
    
    if (status < 0) {
        log_message(log_module, MSG_WARN, "card-%d FE_GET_FRONTEND failed after lock: %s", card_id, strerror(errno));
        // Don't fail the scan, just log the warning
    } else {
        log_message(log_module, MSG_INFO, "card-%d final frontend parameters: frequency=%d Hz", card_id, parameters.frequency);
    }
    
    // Give transport stream time to stabilize after lock (increased from 50ms to 500ms)
    // Use multiple shorter delays to allow for interrupt checking while still providing adequate stabilization time
    log_message(log_module, MSG_INFO, "card-%d waiting for transport stream to stabilize...", card_id);
    log_message(log_module, MSG_DEBUG, "card-%d starting TS stabilization phase...", card_id);
    int ts_stabilization_start = get_time_ms();
    int ts_stabilization_attempts = 0;
    int max_ts_stabilization_attempts = 5; // 5 * 100ms = 500ms total
    int ts_stabilization_success = 0;
    
    while (ts_stabilization_attempts < max_ts_stabilization_attempts && !ts_stabilization_success) {
        ts_stabilization_attempts++;
        log_message(log_module, MSG_DEBUG, "card-%d TS stabilization attempt %d/%d", card_id, ts_stabilization_attempts, max_ts_stabilization_attempts);
        
        // Check for interrupt before each stabilization delay
        // In parallel mode, be more lenient with interrupt checking to avoid false positives
        // For TS stabilization, we should be very lenient since this is a critical phase
        if (get_interrupted()) {
            log_message(log_module, MSG_DEBUG, "card-%d interrupt signal detected during TS stabilization attempt %d/%d, but continuing...", 
                        card_id, ts_stabilization_attempts, max_ts_stabilization_attempts);
            // Continue with stabilization despite interrupt signal - TS stabilization is critical
            // Only fail if we get multiple consecutive interrupts
        }
        
        // Use shorter 100ms delays to allow for interrupt checking
        if (event_sleep_interruptible(MS_TO_US(100)) < 0) { // 100ms delay for TS stabilization
            log_message(log_module, MSG_DEBUG, "card-%d sleep interrupted during TS stabilization attempt %d/%d, but continuing...", 
                        card_id, ts_stabilization_attempts, max_ts_stabilization_attempts);
            // Continue with stabilization despite sleep interruption - TS stabilization is critical
        }
        
        ts_stabilization_success = 1; // Success if we get here
    }
    
    result->ts_stabilization_time_ms = get_elapsed_time_ms(ts_stabilization_start);
    log_message(log_module, MSG_INFO, "card-%d TS stabilization completed (%d attempts) in %d ms", 
                card_id, ts_stabilization_attempts, result->ts_stabilization_time_ms);
    
    // Use the new simple TS processing approach (same as main system)
    log_message(log_module, MSG_INFO, "card-%d attempting channel collection...", card_id);
    log_message(log_module, MSG_DEBUG, "card-%d starting channel acquisition phase...", card_id);
    int channel_acquisition_start = get_time_ms();
    
    // Find the unified card for this card_id
    unified_card_t *card = NULL;
    log_message(log_module, MSG_INFO, "card-%d looking for unified card (num_cards=%d)", 
                card_id, global_parallel_manager->unified_system->num_cards);
    
    // Check if cards array is properly initialized
    if (!global_parallel_manager->unified_system->cards) {
        log_message(log_module, MSG_ERROR, "card-%d unified_system->cards is NULL", card_id);
        return 0;
    }
    
    for (int i = 0; i < global_parallel_manager->unified_system->num_cards; i++) {
        log_message(log_module, MSG_INFO, "card-%d checking unified card %d (unified_card_id=%d)", 
                    card_id, i, global_parallel_manager->unified_system->cards[i].card_id);
        if (global_parallel_manager->unified_system->cards[i].card_id == card_id) {
            card = &global_parallel_manager->unified_system->cards[i];
            log_message(log_module, MSG_INFO, "card-%d found unified card at index %d", card_id, i);
            break;
        }
    }
    
    int channel_count = 0;
    if (card) {
        log_message(log_module, MSG_INFO, "card-%d calling process_transport_stream_simple", card_id);
        // Use the simple TS processing function (same as main system)
        channel_count = process_transport_stream_simple(card, frequency);
        log_message(log_module, MSG_INFO, "card-%d process_transport_stream_simple returned %d channels", card_id, channel_count);
        
        // Copy channel information to result structure
        result->channel_count = channel_count;  // CRITICAL: Set the channel count in result structure
        if (channel_count > 0) {
            for (int i = 0; i < channel_count && i < 128; i++) {
                snprintf(result->channel_names[i], 64, "%s", card->chan_p->channels[i].name);
                result->service_ids[i] = card->chan_p->channels[i].service_id;
            }
        }
    } else {
        log_message(log_module, MSG_ERROR, "No unified card found for card %d", card_id);
        log_message(log_module, MSG_DEBUG, "card-%d unified card lookup failed - num_cards=%d", 
                    card_id, global_parallel_manager->unified_system->num_cards);
    }
    
    result->channel_acquisition_time_ms = get_elapsed_time_ms(channel_acquisition_start);
    log_message(log_module, MSG_DEBUG, "card-%d channel acquisition completed, found %d channels", card_id, channel_count);
    
    if (channel_count < 0) {
        log_message(log_module, MSG_ERROR, "card-%d channel collection failed", card_id);
        pthread_mutex_unlock(card_mutex);
        close(fd_frontend);
        unregister_card_usage(card_id, "parallel_system_tuning");
        result->status = 0; // Failed
        result->error_code = -7;
        strcpy(result->error_message, "Channel collection failed");
        return -1;
    } else if (channel_count > 0) {
        log_message(log_module, MSG_INFO, "card-%d found %d channels on frequency %.1f MHz", 
                    card_id, channel_count, frequency/1000000.0);
    } else {
        log_message(log_module, MSG_INFO, "card-%d no channels found on frequency %.1f MHz (but lock achieved)", 
                    card_id, frequency/1000000.0);
    }
    
    // Unlock the card before closing
    pthread_mutex_unlock(card_mutex);
    log_message(log_module, MSG_DEBUG, "card-%d released card mutex", card_id);
    close(fd_frontend);
    
    // Unregister card usage
    unregister_card_usage(card_id, "parallel_system_tuning");
    
    // Release the card after testing is complete (only if it was assigned)
    if (!is_initial_scan) {
        release_parallel_card_from_frequency(card_id, frequency);
    }
    
    // Calculate total test time
    result->total_test_time_ms = get_elapsed_time_ms(test_start_time);
    
    // Comprehensive timing logging
    log_message(log_module, MSG_INFO, "card-%d frequency test completed successfully (channels: %d)", 
                card_id, channel_count);
    
    log_message(log_module, MSG_INFO, "card-%d timing breakdown: tune=%dms, signal=%dms, carrier=%dms, viterbi=%dms, sync=%dms, lock=%dms, ts_stab=%dms, channel_collection=%dms, total=%dms", 
                card_id, 
                result->tune_complete_time_ms - result->tune_start_time_ms,
                result->signal_lock_time_ms,
                result->carrier_lock_time_ms, 
                result->viterbi_lock_time_ms,
                result->sync_lock_time_ms,
                result->full_lock_time_ms,
                result->ts_stabilization_time_ms,
                result->channel_acquisition_time_ms,
                result->total_test_time_ms);
    
    return 0;
}



/** @brief Parse transport stream to find channels (parallel version)
 * @param ts_data Transport stream data (unused - we use build_channel_list_for_frequency instead)
 * @param data_size Size of data (unused - we use build_channel_list_for_frequency instead)
 * @param card_id Card ID
 * @param frequency Frequency being scanned
 * @param result Output structure to store channel information
 * @return Number of channels found
 */

/** @brief Store frequency and channel information in unified channel system
 * @param card_id Card ID
 * @param frequency Frequency
 * @param result Result data from parallel scan
 */
static void store_frequency_channel_info(int card_id, double frequency, card_frequency_result_t *result)
{
    log_message(log_module, MSG_INFO, "card-%d frequency %.1f MHz: Parallel scan found %d channels", 
                card_id, frequency/1000000.0, result->channel_count);
    
    // Get the unified channel storage from the global system
    if (!global_parallel_manager || !global_parallel_manager->unified_system) {
        log_message(log_module, MSG_ERROR, "No unified system available for storing channels");
        return;
    }
    
    // Find the unified card for this card_id
    unified_card_t *card = NULL;
    
    // Check if cards array is properly initialized
    if (!global_parallel_manager->unified_system->cards) {
        log_message(log_module, MSG_ERROR, "unified_system->cards is NULL");
        return;
    }
    
    for (int i = 0; i < global_parallel_manager->unified_system->num_cards; i++) {
        if (global_parallel_manager->unified_system->cards[i].card_id == card_id) {
            card = &global_parallel_manager->unified_system->cards[i];
            break;
        }
    }
    
    if (!card || !card->chan_p) {
        log_message(log_module, MSG_ERROR, "No channel parameters available for card %d", card_id);
        return;
    }
    
    // Store each discovered channel in the unified storage
    for (int j = 0; j < result->channel_count && j < 128; j++) {
        // Find the corresponding channel in the card's channel list
        mumudvb_channel_t *base_channel = NULL;
        for (int k = 0; k < card->chan_p->number_of_channels; k++) {
            if (card->chan_p->channels[k].service_id == result->service_ids[j]) {
                base_channel = &card->chan_p->channels[k];
                break;
            }
        }
        
        if (base_channel) {
            // Add to unified storage using new adapter (with deduplication)
            if (add_channel_to_storage_adapter(base_channel, frequency, card_id, 1) == 0) {
                log_message(log_module, MSG_INFO, "Card %d %.1f MHz: Stored channel %d: %s (service_id: %d) in unified storage v2", 
                           card_id, frequency/1000000.0, j + 1, result->channel_names[j], result->service_ids[j]);
            } else {
                log_message(log_module, MSG_ERROR, "Card %d %.1f MHz: Failed to store channel %d: %s (service_id: %d)", 
                           card_id, frequency/1000000.0, j + 1, result->channel_names[j], result->service_ids[j]);
            }
        } else {
            log_message(log_module, MSG_WARN, "Card %d %.1f MHz: Channel %d: %s (service_id: %d) not found in card's channel list", 
                       card_id, frequency/1000000.0, j + 1, result->channel_names[j], result->service_ids[j]);
        }
    }
}

/** @brief Card worker thread - handles all frequencies for one card and serves clients
 * @param arg Card ID as void pointer
 * @return NULL
 */
void *card_worker_thread(void *arg)
{
    int card_id = *(int*)arg;
    free(arg); // Free the allocated card_id
    
    if (!global_parallel_manager) {
        return NULL;
    }
    
    unified_channel_system_t *unified_system = global_parallel_manager->unified_system;
    
    log_message(log_module, MSG_INFO, "card-%d worker thread started", card_id);
    
    // Test all frequencies for this card and maintain availability
    for (int freq_idx = 0; freq_idx < unified_system->num_frequencies; freq_idx++) {
        // Check for shutdown before each frequency test
        if (get_interrupted()) {
            log_message(log_module, MSG_INFO, "card-%d worker shutting down due to interrupt signal", card_id);
            return NULL;
        }
        
        // Additional interrupt check before starting frequency test
        if (get_interrupted()) {
            log_message(log_module, MSG_INFO, "card-%d worker interrupted before frequency test", card_id);
            return NULL;
        }
        
        pthread_mutex_lock(&global_parallel_manager->shutdown_mutex);
        if (global_parallel_manager->shutdown_requested) {
            pthread_mutex_unlock(&global_parallel_manager->shutdown_mutex);
            log_message(log_module, MSG_INFO, "card-%d worker shutting down", card_id);
            return NULL;
        }
        pthread_mutex_unlock(&global_parallel_manager->shutdown_mutex);
        
        double frequency = unified_system->frequencies[freq_idx];
        
        log_message(log_module, MSG_INFO, "card-%d testing frequency %.1f MHz", card_id, frequency/1000000.0);
        
        // Add delay for hardware settling between frequency tests
        // Use event-based timing system for proper hardware settling
        extern event_timing_tracker_t *global_timing_tracker;
        if (global_timing_tracker) {
            timing_event_t event;
            int wait_result = wait_for_timing_event(global_timing_tracker, 
                                                  TIMING_EVENT_POLL_INTERVAL, 
                                                  TIMING_CARD_TEST_DELAY_MS, &event);
            if (wait_result < 0 && wait_result != -1) { // -1 is timeout, which is OK
                log_message(log_module, MSG_DEBUG, "card-%d card test delay timing event wait failed: %d", 
                            card_id, wait_result);
            }
        } else {
            // Fallback to interruptible sleep if timing tracker not available
            if (event_sleep_interruptible(MS_TO_US(TIMING_CARD_TEST_DELAY_MS)) < 0) {
                return NULL; // Interrupted
            }
        }
        
        // Test this card/frequency combination with retry logic
        // Allocate result on heap to avoid stack variable issues in multi-threaded context
        card_frequency_result_t *result = malloc(sizeof(card_frequency_result_t));
        if (!result) {
            log_message(log_module, MSG_ERROR, "card-%d failed to allocate memory for result structure", card_id);
            return NULL;
        }
        
        int test_attempts = 0;
        int max_attempts = 1; // Single attempt to avoid multiple retries
        int test_success = 0;
        
        log_message(log_module, MSG_INFO, "card-%d starting frequency test for %.1f MHz", card_id, frequency/1000000.0);
        
        // For initial scan, we don't assign cards - just test them
        // This prevents resource contention during the initial testing phase
        while (test_attempts < max_attempts && !test_success) {
            test_attempts++;
            
            log_message(log_module, MSG_INFO, "card-%d attempt %d/%d for frequency %.1f MHz", 
                        card_id, test_attempts, max_attempts, frequency/1000000.0);
            
            log_message(log_module, MSG_INFO, "card-%d calling test_card_frequency_parallel...", card_id);
            int test_result = test_card_frequency_parallel(card_id, frequency, result, 1);
            log_message(log_module, MSG_INFO, "card-%d test_card_frequency_parallel returned: %d", card_id, test_result);
            
            if (test_result >= 0) {  // Success if >= 0 (0 or more channels found)
                test_success = 1;
                log_message(log_module, MSG_INFO, "card-%d frequency test succeeded", card_id);
            } else {
                // Check if this was a TS stabilization failure that we can retry
                if (result->error_code == -3 && test_attempts < max_attempts) {
                    log_message(log_module, MSG_INFO, "card-%d attempt %d failed due to TS stabilization interruption, retrying in %d seconds...", 
                                card_id, test_attempts, TIMING_FREQUENCY_TEST_DELAY_MS/1000);
                } else if (test_attempts < max_attempts) {
                    log_message(log_module, MSG_DEBUG, "card-%d attempt %d failed for frequency %.1f MHz, retrying in %d seconds...", 
                                card_id, test_attempts, frequency/1000000.0, TIMING_FREQUENCY_TEST_DELAY_MS/1000);
                }
                
                // Card might be busy or TS stabilization failed, wait and retry
                if (test_attempts < max_attempts) {
                    // Use non-blocking event-based timing instead of sleep
                    extern event_timing_tracker_t *global_timing_tracker;
                    if (global_timing_tracker) {
                        timing_event_t event;
                        int wait_result = wait_for_timing_event(global_timing_tracker, 
                                                              TIMING_EVENT_POLL_INTERVAL, 
                                                              TIMING_FREQUENCY_TEST_DELAY_MS, &event);
                        if (wait_result < 0 && wait_result != -1) { // -1 is timeout, which is OK
                            log_message(log_module, MSG_DEBUG, "card-%d timing event wait failed: %d", 
                                        card_id, wait_result);
                        }
                    } else {
                        // Fallback to interruptible sleep if timing tracker not available
                        if (event_sleep_interruptible(MS_TO_US(TIMING_FREQUENCY_TEST_DELAY_MS)) < 0) {
                            return NULL; // Interrupted
                        }
                    }
                }
            }
        }
        
        // Store result in parallel manager (success, failure, or timeout)
        log_message(log_module, MSG_INFO, "card-%d about to store result: channels=%d, status=%d, freq=%.1f", 
                    card_id, result->channel_count, result->status, result->frequency/1000000.0);
        
        // Debug: Log the actual result structure before storing
        log_message(log_module, MSG_INFO, "card-%d result structure: card_id=%d, frequency=%.1f, status=%d, channel_count=%d, signal=%d, snr=%d", 
                    card_id, result->card_id, result->frequency/1000000.0, result->status, result->channel_count, result->signal_strength, result->snr);
        
        pthread_mutex_lock(&global_parallel_manager->results_mutex);
        if (global_parallel_manager->current_result_count < global_parallel_manager->max_results) {
            // Debug: Log what we're about to copy
            log_message(log_module, MSG_INFO, "card-%d copying result: channel_count=%d, status=%d, signal=%d, snr=%d", 
                       card_id, result->channel_count, result->status, result->signal_strength, result->snr);
            
            global_parallel_manager->results[global_parallel_manager->current_result_count] = *result;
            global_parallel_manager->current_result_count++;
            
            // Debug: Verify what was actually copied
            int stored_index = global_parallel_manager->current_result_count - 1;
            log_message(log_module, MSG_INFO, "card-%d stored at index %d: channel_count=%d, status=%d, signal=%d, snr=%d", 
                       card_id, stored_index, 
                       global_parallel_manager->results[stored_index].channel_count,
                       global_parallel_manager->results[stored_index].status,
                       global_parallel_manager->results[stored_index].signal_strength,
                       global_parallel_manager->results[stored_index].snr);
            
            const char *status_text = (result->status == 1) ? "SUCCESS" : 
                                    (result->status == 2) ? "TIMEOUT" : "FAILED";
            log_message(log_module, MSG_INFO, "card-%d frequency %.1f MHz: %s - %d channels found (strength: %d, SNR: %d, total time: %d ms)", 
                        card_id, frequency/1000000.0, status_text, result->channel_count, result->signal_strength, result->snr, result->total_test_time_ms);
            log_message(log_module, MSG_INFO, "card-%d frequency %.1f MHz: Stored result %d/%d (total channels: %d)", 
                        card_id, frequency/1000000.0, global_parallel_manager->current_result_count, global_parallel_manager->max_results, result->channel_count);
        } else {
            log_message(log_module, MSG_WARN, "Results array full (%d/%d), cannot store more results", 
                        global_parallel_manager->current_result_count, global_parallel_manager->max_results);
        }
        pthread_mutex_unlock(&global_parallel_manager->results_mutex);
        
        if (test_success) {
            // Also store in unified channel system for client requests
            store_frequency_channel_info(card_id, frequency, result);
            
            // Mark this card as available for this frequency
            pthread_mutex_lock(&global_parallel_manager->availability_mutex);
            for (int i = 0; i < unified_system->num_cards; i++) {
                if (global_parallel_manager->card_availability[i].card_id == card_id) {
                    global_parallel_manager->card_availability[i].current_frequency = frequency;
                    global_parallel_manager->card_availability[i].is_available = 1;
                    break;
                }
            }
            pthread_mutex_unlock(&global_parallel_manager->availability_mutex);
        } else {
            log_message(log_module, MSG_INFO, "card-%d frequency %.1f MHz: failed after %d attempts - %s (error code: %d)", 
                        card_id, frequency/1000000.0, max_attempts, result->error_message, result->error_code);
        }
        
        // Free the allocated result structure
        free(result);
        
        // Implement idle card rotation after each frequency test
        if (unified_system->num_cards > 1) {
            // Find the card index for this card_id
            int card_idx = -1;
            for (int i = 0; i < unified_system->num_cards; i++) {
                if (unified_system->cards[i].card_id == card_id) {
                    card_idx = i;
                    break;
                }
            }
            
            if (card_idx >= 0) {
                // Try to rotate with an idle card
                int next_active_card = rotate_idle_card(card_idx);
                if (next_active_card >= 0) {
                    log_message(log_module, MSG_INFO, "card-%d rotated to idle, card %d (adapter %d) now active", 
                               card_id, next_active_card, unified_system->cards[next_active_card].card_id);
                    
                    // The current thread should now become idle and wait
                    // The next active card will be handled by a new thread or existing thread
                    // For now, we'll continue with the current thread but mark it as idle
                    log_message(log_module, MSG_INFO, "card-%d becoming idle after frequency test", card_id);
                }
            }
        }
        
        // Wait for next frequency test timing event instead of usleep
        if (global_timing_tracker) {
            timing_event_t event;
            int wait_result = wait_for_timing_event(global_timing_tracker, 
                                                  TIMING_EVENT_FREQUENCY_TEST, 
                                                  TIMING_FREQUENCY_TEST_DELAY_MS, &event);
            if (wait_result < 0 && wait_result != -1) { // -1 is timeout, which is OK
                log_message(log_module, MSG_DEBUG, "card-%d timing event wait failed: %d", 
                            card_id, wait_result);
            }
        } else {
            // Fallback to interruptible sleep if timing tracker not available
            if (event_sleep_interruptible(MS_TO_US(TIMING_FREQUENCY_TEST_DELAY_MS)) < 0) {
                return NULL; // Interrupted
            }
        }
    }
    
    // Signal that this card has completed its initial scan
    pthread_mutex_lock(&global_parallel_manager->results_mutex);
    global_parallel_manager->cards_completed_initial_scan++;
    
    int expected_threads = unified_system->num_cards;
    if (unified_system->scan_limit > 0 && unified_system->scan_limit < unified_system->num_cards) {
        expected_threads = unified_system->scan_limit;
    }
    
    log_message(log_module, MSG_INFO, "card-%d completed initial scan (%d/%d active scan threads done)", 
                card_id, global_parallel_manager->cards_completed_initial_scan, expected_threads);
    
    // Check if all active scan threads have completed their initial scan
    // expected_threads already calculated above
    
    if (global_parallel_manager->cards_completed_initial_scan >= expected_threads) {
        global_parallel_manager->initial_scan_complete = 1;
        global_parallel_manager->scan_in_progress = 0;
        pthread_cond_signal(&global_parallel_manager->scan_complete);
        log_message(log_module, MSG_INFO, "Initial parallel scan completed - all %d active scan threads finished testing", 
                    expected_threads);
    }
    pthread_mutex_unlock(&global_parallel_manager->results_mutex);
    
    // Keep the thread alive to serve client requests for this card
    log_message(log_module, MSG_INFO, "card-%d worker ready to serve client requests", card_id);
    
    while (1) {
        // Check for interrupt signal first
        if (get_interrupted()) {
            log_message(log_module, MSG_INFO, "card-%d worker shutting down due to interrupt signal", card_id);
            break;
        }
        
        // Check for shutdown
        pthread_mutex_lock(&global_parallel_manager->shutdown_mutex);
        if (global_parallel_manager->shutdown_requested) {
            pthread_mutex_unlock(&global_parallel_manager->shutdown_mutex);
            break;
        }
        pthread_mutex_unlock(&global_parallel_manager->shutdown_mutex);
        
        // Process client requests for this card
        // This is where clients would be routed to this specific card thread
        // Use event-based timing instead of usleep
        if (global_timing_tracker) {
            timing_event_t event;
            int wait_result = wait_for_timing_event(global_timing_tracker, 
                                                  TIMING_EVENT_POLL_INTERVAL, 
                                                  TIMING_POLL_INTERVAL_MS * 2, &event);
            if (wait_result < 0 && wait_result != -1) { // -1 is timeout, which is OK
                log_message(log_module, MSG_DEBUG, "card-%d poll timing event wait failed: %d", 
                            card_id, wait_result);
            }
        } else {
            // Fallback to interruptible sleep if timing tracker not available
            if (event_sleep_interruptible(MS_TO_US(TIMING_POLL_INTERVAL_MS * 2)) < 0) {
                break; // Interrupted
            }
        }
    }
    
    log_message(log_module, MSG_INFO, "card-%d worker thread completed", card_id);
    return NULL;
}


/** @brief Set the scan limit for parallel scanning
 * @param scan_limit Maximum number of parallel scan threads (0 = unlimited, 1 = single thread)
 * @return 0 on success, -1 on error
 */
int set_parallel_scan_limit(int scan_limit)
{
    if (!global_parallel_manager || !global_parallel_manager->unified_system) {
        log_message(log_module, MSG_ERROR, "Parallel card manager not initialized");
        return -1;
    }
    
    global_parallel_manager->unified_system->scan_limit = scan_limit;
    log_message(log_module, MSG_INFO, "Parallel scan limit set to %d", scan_limit);
    return 0;
}

/** @brief Get the current scan limit
 * @return Current scan limit (0 = unlimited)
 */
int get_parallel_scan_limit(void)
{
    if (!global_parallel_manager || !global_parallel_manager->unified_system) {
        return 0;
    }
    
    return global_parallel_manager->unified_system->scan_limit;
}

/** @brief Check if there are reserved cards available for client requests
 * @return 1 if reserved cards are available, 0 if not
 */
int has_reserved_cards_for_clients(void)
{
    if (!global_parallel_manager || !global_parallel_manager->unified_system) {
        return 0;
    }
    
    unified_channel_system_t *unified_system = global_parallel_manager->unified_system;
    
    // With rotating idle card system, we always have at least one idle card available
    return (unified_system->num_cards > 1);
}

/** @brief Push a card onto the idle card stack (LIFO)
 * @param card_idx Card index to push onto idle stack
 * @return 0 on success, -1 on error
 */
static int push_idle_card(int card_idx)
{
    if (!global_parallel_manager) {
        return -1;
    }
    
    pthread_mutex_lock(&global_parallel_manager->idle_rotation_mutex);
    
    if (global_parallel_manager->idle_stack_top >= global_parallel_manager->idle_stack_size - 1) {
        pthread_mutex_unlock(&global_parallel_manager->idle_rotation_mutex);
        log_message(log_module, MSG_ERROR, "Idle card stack is full, cannot push card %d", card_idx);
        return -1;
    }
    
    global_parallel_manager->idle_stack_top++;
    global_parallel_manager->idle_card_stack[global_parallel_manager->idle_stack_top] = card_idx;
    
    log_message(log_module, MSG_DEBUG, "Pushed card %d onto idle stack (top=%d)", 
                card_idx, global_parallel_manager->idle_stack_top);
    
    pthread_mutex_unlock(&global_parallel_manager->idle_rotation_mutex);
    return 0;
}

/** @brief Pop a card from the idle card stack (LIFO)
 * @return Card index on success, -1 if stack is empty
 */
static int pop_idle_card(void)
{
    if (!global_parallel_manager) {
        return -1;
    }
    
    pthread_mutex_lock(&global_parallel_manager->idle_rotation_mutex);
    
    if (global_parallel_manager->idle_stack_top < 0) {
        pthread_mutex_unlock(&global_parallel_manager->idle_rotation_mutex);
        return -1; // Stack is empty
    }
    
    int card_idx = global_parallel_manager->idle_card_stack[global_parallel_manager->idle_stack_top];
    global_parallel_manager->idle_stack_top--;
    
    log_message(log_module, MSG_DEBUG, "Popped card %d from idle stack (top=%d)", 
                card_idx, global_parallel_manager->idle_stack_top);
    
    pthread_mutex_unlock(&global_parallel_manager->idle_rotation_mutex);
    return card_idx;
}

/** @brief Get the current idle card (top of stack without popping)
 * @return Card index on success, -1 if stack is empty
 */
static int peek_idle_card(void)
{
    if (!global_parallel_manager) {
        return -1;
    }
    
    pthread_mutex_lock(&global_parallel_manager->idle_rotation_mutex);
    
    int card_idx = -1;
    if (global_parallel_manager->idle_stack_top >= 0) {
        card_idx = global_parallel_manager->idle_card_stack[global_parallel_manager->idle_stack_top];
    }
    
    pthread_mutex_unlock(&global_parallel_manager->idle_rotation_mutex);
    return card_idx;
}

/** @brief Rotate idle card - make current card idle and activate next idle card
 * @param current_card_idx Current card index that wants to become idle
 * @return 0 on success, -1 on error or no rotation needed
 */
static int rotate_idle_card(int current_card_idx)
{
    if (!global_parallel_manager) {
        return -1;
    }
    
    pthread_mutex_lock(&global_parallel_manager->idle_rotation_mutex);
    
    // Check if there are any idle cards to rotate with
    if (global_parallel_manager->idle_stack_top < 0) {
        pthread_mutex_unlock(&global_parallel_manager->idle_rotation_mutex);
        return -1; // No idle cards to rotate with
    }
    
    // Get the next idle card
    int next_idle_card = global_parallel_manager->idle_card_stack[global_parallel_manager->idle_stack_top];
    
    // Push current card onto idle stack
    if (global_parallel_manager->idle_stack_top >= global_parallel_manager->idle_stack_size - 1) {
        pthread_mutex_unlock(&global_parallel_manager->idle_rotation_mutex);
        log_message(log_module, MSG_ERROR, "Idle card stack is full, cannot rotate");
        return -1;
    }
    
    global_parallel_manager->idle_stack_top++;
    global_parallel_manager->idle_card_stack[global_parallel_manager->idle_stack_top] = current_card_idx;
    
    // Pop the next idle card (it becomes active)
    global_parallel_manager->idle_stack_top--;
    
    log_message(log_module, MSG_INFO, "Card rotation: card %d becomes idle, card %d becomes active", 
                current_card_idx, next_idle_card);
    
    pthread_mutex_unlock(&global_parallel_manager->idle_rotation_mutex);
    return next_idle_card;
}

/** @brief Start parallel scanning of all cards and frequencies
 * @return 0 on success, -1 on error
 */
int start_parallel_card_scanning(void)
{
    if (!global_parallel_manager || !global_parallel_manager->unified_system) {
        log_message(log_module, MSG_ERROR, "Parallel card manager not initialized");
        return -1;
    }
    
    unified_channel_system_t *unified_system = global_parallel_manager->unified_system;
    
    // With rotating idle card system, create threads for all cards except one
    // The idle card will rotate using LIFO stack
    int threads_to_create = unified_system->num_cards;
    if (unified_system->num_cards > 1) {
        threads_to_create = unified_system->num_cards - 1; // Leave one card idle initially
        log_message(log_module, MSG_INFO, "Starting parallel scanning with %d card threads (1 card idle, will rotate)", 
                    threads_to_create);
        
        // Initialize idle card stack with all cards, then pop one for initial scanning
        for (int i = 0; i < unified_system->num_cards; i++) {
            push_idle_card(i);
        }
        // Pop one card to make it available for scanning
        int initial_idle_card = pop_idle_card();
        log_message(log_module, MSG_INFO, "Initial idle card: %d (adapter %d)", 
                    initial_idle_card, unified_system->cards[initial_idle_card].card_id);
    } else {
        log_message(log_module, MSG_INFO, "Only 1 card available, using it for scanning (no idle card rotation)");
    }
    
    global_parallel_manager->scan_in_progress = 1;
    global_parallel_manager->current_result_count = 0;
    
    // Create threads for all cards except the idle one
    // Use mutex to protect card_threads array during creation
    pthread_mutex_lock(&global_parallel_manager->shutdown_mutex);
    
    int thread_count = 0;
    for (int card_idx = 0; card_idx < unified_system->num_cards && thread_count < threads_to_create; card_idx++) {
        // Skip the idle card (it's at the top of the stack)
        int idle_card = peek_idle_card();
        if (idle_card == card_idx) {
            log_message(log_module, MSG_DEBUG, "Skipping card %d (currently idle)", card_idx);
            continue;
        }
        
        int *card_id = malloc(sizeof(int));
        if (!card_id) {
            log_message(log_module, MSG_ERROR, "Memory allocation failed for card ID");
            continue;
        }
        *card_id = unified_system->cards[card_idx].card_id;
        
        log_message(log_module, MSG_INFO, "Creating worker thread for card %d (adapter %d)", 
                    card_idx, *card_id);
        
        // Initialize thread slot to 0 before creating thread
        global_parallel_manager->card_threads[card_idx] = 0;
        
        if (pthread_create(&global_parallel_manager->card_threads[card_idx], NULL, 
                          card_worker_thread, card_id) != 0) {
            log_message(log_module, MSG_ERROR, "Failed to create worker thread for card %d", *card_id);
            free(card_id);
            // Keep thread slot as 0 (already set above)
            continue;
        }
        
        // Validate the created thread ID
        pthread_t thread_id = global_parallel_manager->card_threads[card_idx];
        if (thread_id == 0 || (unsigned long)thread_id < 0x1000 || (unsigned long)thread_id > 0x7fffffffffff) {
            log_message(log_module, MSG_ERROR, "Created thread for card %d has invalid thread ID: %lu", 
                       *card_id, (unsigned long)thread_id);
            global_parallel_manager->card_threads[card_idx] = 0;
            free(card_id);
            continue;
        }
        
        log_message(log_module, MSG_INFO, "Created worker thread for card %d (thread_id=%lu)", 
                   *card_id, (unsigned long)thread_id);
        
        thread_count++; // Increment thread count for this card
        
        // Small delay between thread creation using poll() instead of usleep()
        // Note: This is called from main thread context, so we can use poll()
        extern fds_t *global_main_fds;
        extern unicast_parameters_t *global_unicast_params;
        
        if (global_main_fds && global_unicast_params) {
            main_thread_sleep_with_poll(global_main_fds, global_unicast_params, TIMING_THREAD_CREATION_DELAY_MS);
        } else {
            // Fallback to interruptible sleep if main thread context not available
            if (event_sleep_interruptible(MS_TO_US(TIMING_THREAD_CREATION_DELAY_MS)) < 0) {
                return -1; // Interrupted
            }
        }
    }
    
    // Start idle card rotation thread if we have multiple cards
    if (unified_system->num_cards > 1) {
        log_message(log_module, MSG_INFO, "Starting idle card rotation system");
        // The rotation will be handled by the card worker threads themselves
        // Each thread will check for rotation opportunities when it completes a frequency
    }
    
    // Unlock the mutex after all thread creation is complete
    pthread_mutex_unlock(&global_parallel_manager->shutdown_mutex);
    
    log_message(log_module, MSG_INFO, "All card worker threads created - scanning in progress");
    
    return 0;
}

/** @brief Route client to appropriate card thread for a channel-frequency pair
 * @param frequency The frequency the client wants
 * @param channel_name The channel name the client wants
 * @return Card ID that can handle this request, or -1 if none available
 */
int route_client_to_card_thread(double frequency, const char *channel_name)
{
    if (!global_parallel_manager) {
        return -1;
    }
    
    // Find the best card for this frequency (force assignment for client requests)
    int best_card = get_available_parallel_card_for_frequency(frequency, 1);
    if (best_card == -1) {
        log_message(log_module, MSG_INFO, "No card available for frequency %.0f Hz", frequency);
        return -1;
    }
    
    // Check if the card is available and not currently serving another client
    pthread_mutex_lock(&global_parallel_manager->availability_mutex);
    for (int i = 0; i < global_parallel_manager->unified_system->num_cards; i++) {
        if (global_parallel_manager->card_availability[i].card_id == best_card) {
            if (global_parallel_manager->card_availability[i].is_available && 
                global_parallel_manager->card_availability[i].client_count < 1) { // Max 1 client per card for now
                
                // Assign this client to this card
                global_parallel_manager->card_availability[i].client_count++;
                global_parallel_manager->card_availability[i].is_available = 0; // Mark as busy
                global_parallel_manager->card_availability[i].last_used = time(NULL);
                
                log_message(log_module, MSG_INFO, "Client routed to card %d for frequency %.0f Hz, channel %s", 
                           best_card, frequency, channel_name ? channel_name : "unknown");
                
                pthread_mutex_unlock(&global_parallel_manager->availability_mutex);
                return best_card;
            }
            break;
        }
    }
    pthread_mutex_unlock(&global_parallel_manager->availability_mutex);
    
    log_message(log_module, MSG_INFO, "card-%d is busy, cannot serve client for frequency %.1f MHz", 
               best_card, frequency/1000000.0);
    return -1;
}

/** @brief Release a card from client service
 * @param card_id The card ID to release
 * @return 0 on success, -1 on error
 */
int release_card_from_client(int card_id)
{
    if (!global_parallel_manager) {
        return -1;
    }
    
    pthread_mutex_lock(&global_parallel_manager->availability_mutex);
    for (int i = 0; i < global_parallel_manager->unified_system->num_cards; i++) {
        if (global_parallel_manager->card_availability[i].card_id == card_id) {
            if (global_parallel_manager->card_availability[i].client_count > 0) {
                global_parallel_manager->card_availability[i].client_count--;
                global_parallel_manager->card_availability[i].is_available = 1; // Mark as available
                global_parallel_manager->card_availability[i].last_used = time(NULL);
                
                log_message(log_module, MSG_INFO, "card-%d released from client service", card_id);
                pthread_mutex_unlock(&global_parallel_manager->availability_mutex);
                return 0;
            }
            break;
        }
    }
    pthread_mutex_unlock(&global_parallel_manager->availability_mutex);
    
    return -1;
}

/** @brief Get the best card for a given frequency based on signal quality
 * @param frequency The frequency to find the best card for
 * @return Card ID of the best card, or -1 if none found
 */
int get_best_card_for_frequency(double frequency)
{
    if (!global_parallel_manager || global_parallel_manager->current_result_count == 0) {
        return -1;
    }
    
    int best_card = -1;
    int best_signal = 0;
    int best_snr = 0;
    
    pthread_mutex_lock(&global_parallel_manager->results_mutex);
    
    for (int i = 0; i < global_parallel_manager->current_result_count; i++) {
        card_frequency_result_t *result = &global_parallel_manager->results[i];
        
        if (result->frequency == frequency && result->status == 1) { // Success
            // Use a combination of signal strength and SNR to determine best card
            int quality_score = result->signal_strength + (result->snr / 10);
            
            if (best_card == -1 || quality_score > (best_signal + (best_snr / 10))) {
                best_card = result->card_id;
                best_signal = result->signal_strength;
                best_snr = result->snr;
            }
        }
    }
    
    pthread_mutex_unlock(&global_parallel_manager->results_mutex);
    
    if (best_card != -1) {
        log_message(log_module, MSG_INFO, "Best card for frequency %.1f MHz: card-%d (Signal=%d, SNR=%d)", 
                    frequency/1000000.0, best_card, best_signal, best_snr);
    }
    
    return best_card;
}

/** @brief Generate a comprehensive card/frequency matrix
 * @param output_file Path to output file for the matrix
 * @return 0 on success, -1 on error
 */
int generate_card_frequency_matrix(const char *output_file)
{
    if (!global_parallel_manager || !output_file) {
        return -1;
    }
    
    FILE *fp = fopen(output_file, "w");
    if (!fp) {
        log_message(log_module, MSG_ERROR, "Cannot open output file: %s", output_file);
        return -1;
    }
    
    fprintf(fp, "=== MuMuDVB Parallel Card/Frequency Matrix ===\n");
    fprintf(fp, "Generated: %s\n", ctime(&(time_t){time(NULL)}));
    fprintf(fp, "Total Results: %d\n\n", global_parallel_manager->current_result_count);
    
    fprintf(fp, "Card | Frequency (Hz) | Status | Signal | SNR  | BER   | Channels | Quality\n");
    fprintf(fp, "-----|----------------|--------|--------|------|-------|----------|--------\n");
    
    pthread_mutex_lock(&global_parallel_manager->results_mutex);
    
    for (int i = 0; i < global_parallel_manager->current_result_count; i++) {
        card_frequency_result_t *result = &global_parallel_manager->results[i];
        
        const char *status_str = (result->status == 1) ? "SUCCESS" : 
                                (result->status == 2) ? "TIMEOUT" : "FAILED";
        
        int quality_score = result->signal_strength + (result->snr / 10);
        
        fprintf(fp, "%-4d | %-12.0f | %-6s | %-6d | %-4d | %-5d | %-8d | %-7d\n",
                result->card_id, result->frequency, status_str, 
                result->signal_strength, result->snr, result->ber, 
                result->channel_count, quality_score);
    }
    
    pthread_mutex_unlock(&global_parallel_manager->results_mutex);
    
    fclose(fp);
    
    log_message(log_module, MSG_INFO, "card/frequency matrix saved to: %s", output_file);
    
    return 0;
}

/** @brief Get scan results for a specific card
 * @param card_id The card ID
 * @param results Output array for results
 * @param max_results Maximum number of results to return
 * @return Number of results found
 */
int get_card_scan_results(int card_id, void *results, int max_results)
{
    if (!global_parallel_manager || !results || max_results <= 0) {
        return 0;
    }
    
    int count = 0;
    card_frequency_result_t *result_array = (card_frequency_result_t *)results;
    
    pthread_mutex_lock(&global_parallel_manager->results_mutex);
    
    for (int i = 0; i < global_parallel_manager->current_result_count && count < max_results; i++) {
        card_frequency_result_t *result = &global_parallel_manager->results[i];
        
        if (result->card_id == card_id) {
            result_array[count] = *result;
            count++;
        }
    }
    
    pthread_mutex_unlock(&global_parallel_manager->results_mutex);
    
    return count;
}

/** @brief Request a card for a specific frequency (non-blocking)
 * @param frequency The frequency to tune to
 * @param priority Priority of the request (higher = more important)
 * @return Request ID on success, -1 on error
 */
int request_card_for_frequency(double frequency, int priority)
{
    if (!global_parallel_manager) {
        return -1;
    }
    
    pthread_mutex_lock(&global_parallel_manager->client_requests_mutex);
    
    // Find an empty slot in the client requests array
    int request_id = -1;
    for (int i = 0; i < global_parallel_manager->max_client_requests; i++) {
        if (global_parallel_manager->client_requests[i].status == 0) { // Pending
            request_id = i;
            break;
        }
    }
    
    if (request_id == -1) {
        log_message(log_module, MSG_WARN, "No available slots for client request");
        pthread_mutex_unlock(&global_parallel_manager->client_requests_mutex);
        return -1;
    }
    
    // Fill in the request
    global_parallel_manager->client_requests[request_id].frequency = frequency;
    global_parallel_manager->client_requests[request_id].priority = priority;
    global_parallel_manager->client_requests[request_id].request_time = time(NULL);
    global_parallel_manager->client_requests[request_id].assigned_card = -1;
    global_parallel_manager->client_requests[request_id].status = 0; // Pending
    
    global_parallel_manager->current_client_requests++;
    
    pthread_mutex_unlock(&global_parallel_manager->client_requests_mutex);
    
    // Signal that a new request is ready
    pthread_cond_signal(&global_parallel_manager->client_request_ready);
    
    log_message(log_module, MSG_INFO, "Client request %d created for frequency %.0f Hz (priority %d)", 
                request_id, frequency, priority);
    
    return request_id;
}

/** @brief Get card quality score for a specific frequency
 * @param card_id The card ID
 * @param frequency The frequency
 * @return Quality score (higher is better), or -1 if not available
 */
static int get_card_quality_for_frequency(int card_id, double frequency)
{
    if (!global_parallel_manager) {
        return -1;
    }
    
    pthread_mutex_lock(&global_parallel_manager->results_mutex);
    
    int best_quality = -1;
    
    // Look through scan results for this card/frequency combination
    for (int i = 0; i < global_parallel_manager->current_result_count; i++) {
        card_frequency_result_t *result = &global_parallel_manager->results[i];
        
        if (result->card_id == card_id && result->frequency == frequency && result->status == 1) {
            // Calculate quality score based on signal strength and SNR
            int quality = result->signal_strength + (result->snr / 10) - (result->ber / 100);
            if (quality > best_quality) {
                best_quality = quality;
            }
        }
    }
    
    pthread_mutex_unlock(&global_parallel_manager->results_mutex);
    
    return best_quality;
}

/** @brief Get the best available card for a frequency (non-blocking)
 * @param frequency The frequency to find a card for
 * @param force_assignment If 1, assign even if initial scan not complete (for client requests)
 * @return Card ID of the best available card, or -1 if none available
 */
int get_available_parallel_card_for_frequency(double frequency, int force_assignment)
{
    if (!global_parallel_manager) {
        return -1;
    }
    
    // Check if initial scan is complete, unless forcing assignment for client request
    if (!force_assignment && !is_initial_scan_complete()) {
        log_message(log_module, MSG_DEBUG, "Initial scan not complete, not assigning cards yet");
        return -1;
    }
    
    int best_card = -1;
    int best_quality = -1;
    
    pthread_mutex_lock(&global_parallel_manager->availability_mutex);
    
    // First check if any card is already assigned to this frequency
    for (int i = 0; i < global_parallel_manager->unified_system->num_cards; i++) {
        card_availability_t *card = &global_parallel_manager->card_availability[i];
        
        pthread_mutex_lock(&card->card_mutex);
        
        if (card->current_frequency == frequency && card->is_tuning) {
            // This frequency already has a card assigned
            pthread_mutex_unlock(&card->card_mutex);
            log_message(log_module, MSG_DEBUG, "Frequency %.0f Hz already has card %d assigned", frequency, card->card_id);
            pthread_mutex_unlock(&global_parallel_manager->availability_mutex);
            return -1;
        }
        
        pthread_mutex_unlock(&card->card_mutex);
    }
    
    // Look for available cards that can handle this frequency
    for (int i = 0; i < global_parallel_manager->unified_system->num_cards; i++) {
        card_availability_t *card = &global_parallel_manager->card_availability[i];
        
        pthread_mutex_lock(&card->card_mutex);
        
        // Check if card is available and not currently tuning
        if (card->is_available && !card->is_tuning) {
            // Check if this card is already tuned to the requested frequency
            if (card->current_frequency == frequency) {
                // Perfect match - this card is already tuned to the right frequency
                best_card = card->card_id;
                pthread_mutex_unlock(&card->card_mutex);
                break;
            }
            
            // Check if this card can handle the frequency (based on scan results)
            int quality = get_card_quality_for_frequency(card->card_id, frequency);
            if (quality > 0 && quality > best_quality) {
                best_card = card->card_id;
                best_quality = quality;
            }
        }
        
        pthread_mutex_unlock(&card->card_mutex);
    }
    
    pthread_mutex_unlock(&global_parallel_manager->availability_mutex);
    
    if (best_card != -1) {
        log_message(log_module, MSG_INFO, "Found available card %d for frequency %.0f Hz (quality %d)", 
                    best_card, frequency, best_quality);
    } else {
        log_message(log_module, MSG_DEBUG, "No available cards for frequency %.0f Hz", frequency);
    }
    
    return best_card;
}

/** @brief Check if a card is available for testing/tuning
 * @param card_id The card to check
 * @return 1 if available, 0 if busy/in use
 */
int is_card_available_for_testing(int card_id)
{
    if (!global_parallel_manager) {
        return 0;
    }
    
    // First check if the card is being used by the main system
    char frontend_path[256];
    snprintf(frontend_path, sizeof(frontend_path), "/dev/dvb/adapter%d/frontend0", card_id);
    
    int fd_frontend = open(frontend_path, O_RDONLY | O_NONBLOCK);
    if (fd_frontend >= 0) {
        fe_status_t fe_status;
        int status_ret = ioctl(fd_frontend, FE_READ_STATUS, &fe_status);
        close(fd_frontend);
        
        if (status_ret >= 0 && (fe_status & FE_HAS_LOCK)) {
            log_message(log_module, MSG_DEBUG, "card-%d is locked by main system, not available for testing", card_id);
            return 0; // Card is locked and in use by main system
        }
    }
    
    pthread_mutex_lock(&global_parallel_manager->availability_mutex);
    
    for (int i = 0; i < global_parallel_manager->unified_system->num_cards; i++) {
        card_availability_t *card = &global_parallel_manager->card_availability[i];
        
        if (card->card_id == card_id) {
            pthread_mutex_lock(&card->card_mutex);
            int is_available = (card->is_available && !card->is_tuning && card->client_count == 0);
            pthread_mutex_unlock(&card->card_mutex);
            pthread_mutex_unlock(&global_parallel_manager->availability_mutex);
            return is_available;
        }
    }
    
    pthread_mutex_unlock(&global_parallel_manager->availability_mutex);
    return 0; // Card not found in parallel manager
}

/** @brief Assign a card to a frequency (non-blocking)
 * @param card_id The card to assign
 * @param frequency The frequency to tune to
 * @return 0 on success, -1 on error
 */
int assign_parallel_card_to_frequency(int card_id, double frequency)
{
    if (!global_parallel_manager) {
        return -1;
    }
    
    // Find the card in the availability array
    card_availability_t *card = NULL;
    for (int i = 0; i < global_parallel_manager->unified_system->num_cards; i++) {
        if (global_parallel_manager->card_availability[i].card_id == card_id) {
            card = &global_parallel_manager->card_availability[i];
            break;
        }
    }
    
    if (!card) {
        log_message(log_module, MSG_ERROR, "card-%d not found in availability array", card_id);
        return -1;
    }
    
    pthread_mutex_lock(&card->card_mutex);
    
    if (!card->is_available || card->is_tuning) {
        log_message(log_module, MSG_WARN, "card-%d is not available for assignment", card_id);
        pthread_mutex_unlock(&card->card_mutex);
        return -1;
    }
    
    // Mark card as tuning
    card->is_tuning = 1;
    card->current_frequency = frequency;
    card->last_used = time(NULL);
    
    pthread_mutex_unlock(&card->card_mutex);
    
    log_message(log_module, MSG_INFO, "Assigned card %d to frequency %.0f Hz", card_id, frequency);
    
    return 0;
}

/** @brief Release a card from frequency usage (non-blocking)
 * @param card_id The card to release
 * @param frequency The frequency being released
 * @return 0 on success, -1 on error
 */
int release_parallel_card_from_frequency(int card_id, double frequency)
{
    if (!global_parallel_manager) {
        return -1;
    }
    
    // Find the card in the availability array
    card_availability_t *card = NULL;
    for (int i = 0; i < global_parallel_manager->unified_system->num_cards; i++) {
        if (global_parallel_manager->card_availability[i].card_id == card_id) {
            card = &global_parallel_manager->card_availability[i];
            break;
        }
    }
    
    if (!card) {
        log_message(log_module, MSG_ERROR, "card-%d not found in availability array", card_id);
        return -1;
    }
    
    pthread_mutex_lock(&card->card_mutex);
    
    if (card->current_frequency == frequency) {
        card->is_tuning = 0;
        card->is_available = 1;
        card->client_count = 0;
        card->current_frequency = 0.0;
        
        log_message(log_module, MSG_INFO, "Released card-%d from frequency %.1f MHz", card_id, frequency/1000000.0);
    } else {
        log_message(log_module, MSG_WARN, "card-%d is not currently tuned to frequency %.1f MHz", card_id, frequency/1000000.0);
    }
    
    pthread_mutex_unlock(&card->card_mutex);
    
    return 0;
}


/** @brief Client request processing thread (runs in background)
 * @param arg Unused
 * @return NULL
 */
void *client_request_processor(void *arg)
{
    (void)arg; // Suppress unused parameter warning
    
    if (!global_parallel_manager) {
        return NULL;
    }
    
    log_message(log_module, MSG_INFO, "Client request processor started");
    
    while (1) {
        // Check for interrupt signal first
        if (get_interrupted()) {
            log_message(log_module, MSG_INFO, "Client request processor shutting down due to interrupt signal");
            break;
        }
        
        // Check for shutdown request
        if (!global_parallel_manager) {
            log_message(log_module, MSG_INFO, "Client request processor shutting down - manager freed");
            break;
        }
        
        pthread_mutex_lock(&global_parallel_manager->shutdown_mutex);
        if (global_parallel_manager->shutdown_requested) {
            pthread_mutex_unlock(&global_parallel_manager->shutdown_mutex);
            log_message(log_module, MSG_INFO, "Client request processor shutting down");
            break;
        }
        pthread_mutex_unlock(&global_parallel_manager->shutdown_mutex);
        
        if (!global_parallel_manager) {
            log_message(log_module, MSG_INFO, "Client request processor shutting down - manager freed");
            break;
        }
        
        pthread_mutex_lock(&global_parallel_manager->client_requests_mutex);
        
        // Wait for client requests with timeout
        struct timespec timeout;
        clock_gettime(CLOCK_REALTIME, &timeout);
        timeout.tv_sec += 1; // 1 second timeout
        
        while (global_parallel_manager && global_parallel_manager->current_client_requests == 0) {
            int ret = pthread_cond_timedwait(&global_parallel_manager->client_request_ready, 
                                           &global_parallel_manager->client_requests_mutex, &timeout);
            if (ret == ETIMEDOUT) {
                // Timeout - check for shutdown again
                pthread_mutex_unlock(&global_parallel_manager->client_requests_mutex);
                continue;
            }
        }
        
        if (!global_parallel_manager) {
            log_message(log_module, MSG_INFO, "Client request processor shutting down - manager freed during wait");
            break;
        }
        
        // Process pending requests - only assign one card per frequency
        if (!global_parallel_manager) {
            log_message(log_module, MSG_INFO, "Client request processor shutting down - manager freed before processing");
            break;
        }
        
        for (int i = 0; i < global_parallel_manager->max_client_requests; i++) {
            struct client_request *req = &global_parallel_manager->client_requests[i];
            
            if (req->status == 0) { // Pending
                // Check if this frequency already has a card assigned
                int already_assigned = 0;
                for (int j = 0; j < global_parallel_manager->unified_system->num_cards; j++) {
                    if (global_parallel_manager->card_availability[j].current_frequency == req->frequency &&
                        global_parallel_manager->card_availability[j].is_tuning) {
                        already_assigned = 1;
                        break;
                    }
                }
                
                if (already_assigned) {
                    log_message(log_module, MSG_DEBUG, "Frequency %.0f Hz already has a card assigned, skipping", req->frequency);
                    continue;
                }
                
                // Try to find an available card for this frequency (force assignment for client requests)
                int card_id = get_available_parallel_card_for_frequency(req->frequency, 1);
                
                if (card_id != -1) {
                    // Assign the card
                    if (assign_parallel_card_to_frequency(card_id, req->frequency) == 0) {
                        req->assigned_card = card_id;
                        req->status = 1; // Assigned
                        global_parallel_manager->current_client_requests--;
                        
                        log_message(log_module, MSG_INFO, "Client request %d assigned to card %d for frequency %.0f Hz", 
                                    i, card_id, req->frequency);
                    } else {
                        req->status = 3; // Failed
                        global_parallel_manager->current_client_requests--;
                    }
                } else {
                    // No available cards, keep request pending
                    log_message(log_module, MSG_DEBUG, "No available cards for client request %d (frequency %.0f Hz)", 
                                i, req->frequency);
                }
            }
        }
        
        if (global_parallel_manager) {
            pthread_mutex_unlock(&global_parallel_manager->client_requests_mutex);
        }
        
        // Small delay to prevent busy waiting
        if (event_sleep_interruptible(MS_TO_US(TIMING_POLL_INTERVAL_MS)) < 0) {
            return NULL; // Interrupted
        }
        
        // Check if manager was freed during processing
        if (!global_parallel_manager) {
            log_message(log_module, MSG_INFO, "Client request processor shutting down - manager freed during processing");
            break;
        }
    }
    
    return NULL;
}

/** @brief Get the number of scan results available
 * @return Number of scan results, or 0 if none available
 */
int get_parallel_scan_results_count(void)
{
    if (!global_parallel_manager) {
        log_message(log_module, MSG_DEBUG, "get_parallel_scan_results_count: No global manager");
        return 0;
    }
    
    pthread_mutex_lock(&global_parallel_manager->results_mutex);
    int count = global_parallel_manager->current_result_count;
    pthread_mutex_unlock(&global_parallel_manager->results_mutex);
    
    log_message(log_module, MSG_DEBUG, "get_parallel_scan_results_count: returning %d", count);
    return count;
}

/** @brief Wait for initial scan to complete
 * @param timeout_ms Timeout in milliseconds (0 = wait forever)
 * @return 0 on success, -1 on timeout or error
 */
int wait_for_initial_scan_complete(int timeout_ms)
{
    if (!global_parallel_manager) {
        return -1;
    }
    
    // Use a polling approach with interrupt checks instead of blocking wait
    int start_time = time(NULL);
    int timeout_sec = timeout_ms / 1000;
    
    while (1) {
        // Check for interrupt first
        if (get_interrupted()) {
            log_message(log_module, MSG_INFO, "Initial scan wait interrupted by signal");
            return -1;
        }
        
        pthread_mutex_lock(&global_parallel_manager->results_mutex);
        
        // If already complete, return immediately
        if (global_parallel_manager->initial_scan_complete) {
            pthread_mutex_unlock(&global_parallel_manager->results_mutex);
            return 0;
        }
        
        pthread_mutex_unlock(&global_parallel_manager->results_mutex);
        
        // Check timeout
        if (timeout_ms > 0 && (time(NULL) - start_time) >= timeout_sec) {
            log_message(log_module, MSG_WARN, "Initial scan wait timed out after %d seconds", timeout_sec);
            return -1;
        }
        
        // Poll on relevant file descriptors instead of usleep
        // Use a simple timeout poll since we don't have specific file descriptors to wait on
        struct pollfd pfd = {0, 0, 0}; // No file descriptor, just timeout
        int poll_result = poll(&pfd, 1, 100); // 100ms timeout
        if (poll_result < 0 && errno != EINTR) {
            log_message(log_module, MSG_ERROR, "Poll error in initial scan wait: %s", strerror(errno));
            return -1;
        }
    }
}

/** @brief Check if initial scan is complete
 * @return 1 if complete, 0 if still in progress
 */
int is_initial_scan_complete(void)
{
    if (!global_parallel_manager) {
        return 0;
    }
    
    pthread_mutex_lock(&global_parallel_manager->results_mutex);
    int complete = global_parallel_manager->initial_scan_complete;
    pthread_mutex_unlock(&global_parallel_manager->results_mutex);
    
    return complete;
}

/** @brief Get all scan results
 * @param results Output array for results
 * @param max_results Maximum number of results to return
 * @return Number of results copied
 */
int get_parallel_scan_results(card_frequency_result_t *results, int max_results)
{
    if (!global_parallel_manager || !results || max_results <= 0) {
        log_message(log_module, MSG_DEBUG, "get_parallel_scan_results: Invalid parameters");
        return 0;
    }
    
    pthread_mutex_lock(&global_parallel_manager->results_mutex);
    
    int count = 0;
    int available = global_parallel_manager->current_result_count;
    int to_copy = (available < max_results) ? available : max_results;
    
    for (int i = 0; i < to_copy; i++) {
        results[i] = global_parallel_manager->results[i];
        count++;
    }
    
    pthread_mutex_unlock(&global_parallel_manager->results_mutex);
    
    log_message(log_module, MSG_INFO, "get_parallel_scan_results: returning %d results (available: %d, max: %d)", 
                count, available, max_results);
    
    
    return count;
}

/** @brief Set the client processor thread for cleanup
 * @param thread The client processor thread
 */
void set_client_processor_thread(pthread_t thread)
{
    if (global_parallel_manager) {
        global_parallel_manager->client_processor_thread = thread;
    }
}


