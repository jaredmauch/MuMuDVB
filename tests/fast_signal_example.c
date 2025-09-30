/**
 * @file fast_signal_example.c
 * @brief Example integration of fast signal handling into existing threads
 * 
 * This file shows how to modify existing threads to use the optimized
 * signal handling for faster Ctrl-C response.
 */

#include "signal_optimization.h"
#include "mumudvb.h"
#include "log.h"

static char *log_module = "Fast-Signal-Example: ";

/**
 * @brief Example: Optimized card worker thread with fast signal response
 */
void *optimized_card_worker_thread(void *arg)
{
    int card_id = *(int*)arg;
    free(arg);
    
    // Register this thread for fast signal response
    thread_signal_context_t signal_context;
    if (register_thread_for_signals(card_id, 1, &signal_context) < 0) { // Priority 1 = normal
        log_message(log_module, MSG_ERROR, "Cannot register card %d for signals", card_id);
        return NULL;
    }
    
    log_message(log_module, MSG_INFO, "Optimized card worker thread started for card %d", card_id);
    
    while (1) {
        // Check for signal (non-blocking)
        if (check_signal_received(&signal_context)) {
            log_message(log_module, MSG_INFO, "Card %d worker shutting down due to signal", card_id);
            break;
        }
        
        // Check for interrupt (backward compatibility)
        if (get_interrupted()) {
            log_message(log_module, MSG_INFO, "Card %d worker shutting down due to interrupt", card_id);
            break;
        }
        
        // Do work here...
        // Process client requests, handle tuning, etc.
        
        // Wait for signal or timeout with optimized response time
        int wait_result = wait_for_signal_or_timeout(&signal_context, 0); // 0 = use recommended timeout
        
        if (wait_result == 0) {
            // Signal received, check again in next iteration
            continue;
        } else if (wait_result == -1) {
            // Timeout, continue with work
            continue;
        } else {
            // Error
            log_message(log_module, MSG_ERROR, "Card %d signal wait error: %d", card_id, wait_result);
            break;
        }
    }
    
    log_message(log_module, MSG_INFO, "Optimized card worker thread completed for card %d", card_id);
    return NULL;
}

/**
 * @brief Example: Optimized main loop with fast signal response
 */
int optimized_main_loop(void)
{
    // Register main thread for fast signal response (critical priority)
    thread_signal_context_t signal_context;
    if (register_thread_for_signals(0, 0, &signal_context) < 0) { // Priority 0 = critical
        log_message(log_module, MSG_ERROR, "Cannot register main thread for signals");
        return -1;
    }
    
    log_message(log_module, MSG_INFO, "Optimized main loop started");
    
    while (!get_interrupted()) {
        // Check for signal (non-blocking)
        if (check_signal_received(&signal_context)) {
            log_message(log_module, MSG_INFO, "Main loop shutting down due to signal");
            break;
        }
        
        // Do main processing...
        // Handle DVB operations, client requests, etc.
        
        // Wait for signal or timeout with critical thread response time
        int wait_result = wait_for_signal_or_timeout(&signal_context, TIMING_CRITICAL_THREAD_MS);
        
        if (wait_result == 0) {
            // Signal received, check again immediately
            continue;
        } else if (wait_result == -1) {
            // Timeout, continue with processing
            continue;
        } else {
            // Error
            log_message(log_module, MSG_ERROR, "Main loop signal wait error: %d", wait_result);
            break;
        }
    }
    
    log_message(log_module, MSG_INFO, "Optimized main loop completed");
    return 0;
}

/**
 * @brief Example: Optimized background scanner with fast signal response
 */
void *optimized_background_scanner(void *arg)
{
    // Register this thread for fast signal response (background priority)
    thread_signal_context_t signal_context;
    if (register_thread_for_signals(-1, 2, &signal_context) < 0) { // Priority 2 = background
        log_message(log_module, MSG_ERROR, "Cannot register background scanner for signals");
        return NULL;
    }
    
    log_message(log_module, MSG_INFO, "Optimized background scanner started");
    
    while (!get_interrupted()) {
        // Check for signal (non-blocking)
        if (check_signal_received(&signal_context)) {
            log_message(log_module, MSG_INFO, "Background scanner shutting down due to signal");
            break;
        }
        
        // Do background scanning work...
        // Test frequencies, discover channels, etc.
        
        // Wait for signal or timeout with background thread response time
        int wait_result = wait_for_signal_or_timeout(&signal_context, TIMING_BACKGROUND_THREAD_MS);
        
        if (wait_result == 0) {
            // Signal received, check again in next iteration
            continue;
        } else if (wait_result == -1) {
            // Timeout, continue with scanning
            continue;
        } else {
            // Error
            log_message(log_module, MSG_ERROR, "Background scanner signal wait error: %d", wait_result);
            break;
        }
    }
    
    log_message(log_module, MSG_INFO, "Optimized background scanner completed");
    return NULL;
}

/**
 * @brief Example: Optimized DVB read thread with fast signal response
 */
void *optimized_dvb_read_thread(void *arg)
{
    // Register this thread for fast signal response (normal priority)
    thread_signal_context_t signal_context;
    if (register_thread_for_signals(-2, 1, &signal_context) < 0) { // Priority 1 = normal
        log_message(log_module, MSG_ERROR, "Cannot register DVB read thread for signals");
        return NULL;
    }
    
    log_message(log_module, MSG_INFO, "Optimized DVB read thread started");
    
    while (!get_interrupted()) {
        // Check for signal (non-blocking)
        if (check_signal_received(&signal_context)) {
            log_message(log_module, MSG_INFO, "DVB read thread shutting down due to signal");
            break;
        }
        
        // Do DVB reading work...
        // Read from DVB device, process packets, etc.
        
        // Wait for signal or timeout with normal thread response time
        int wait_result = wait_for_signal_or_timeout(&signal_context, TIMING_NORMAL_THREAD_MS);
        
        if (wait_result == 0) {
            // Signal received, check again in next iteration
            continue;
        } else if (wait_result == -1) {
            // Timeout, continue with reading
            continue;
        } else {
            // Error
            log_message(log_module, MSG_ERROR, "DVB read thread signal wait error: %d", wait_result);
            break;
        }
    }
    
    log_message(log_module, MSG_INFO, "Optimized DVB read thread completed");
    return NULL;
}

/**
 * @brief Example: Integration into main application
 */
int integrate_fast_signal_handling(void)
{
    // Initialize fast signal handling
    if (init_fast_signal_handling() < 0) {
        log_message(log_module, MSG_ERROR, "Cannot initialize fast signal handling");
        return -1;
    }
    
    // Start optimized threads
    pthread_t card_thread, scanner_thread, dvb_thread;
    
    // Start card worker thread
    int card_id = 0;
    if (pthread_create(&card_thread, NULL, optimized_card_worker_thread, &card_id) != 0) {
        log_message(log_module, MSG_ERROR, "Cannot create card worker thread");
        cleanup_fast_signal_handling();
        return -1;
    }
    
    // Start background scanner thread
    if (pthread_create(&scanner_thread, NULL, optimized_background_scanner, NULL) != 0) {
        log_message(log_module, MSG_ERROR, "Cannot create background scanner thread");
        cleanup_fast_signal_handling();
        return -1;
    }
    
    // Start DVB read thread
    if (pthread_create(&dvb_thread, NULL, optimized_dvb_read_thread, NULL) != 0) {
        log_message(log_module, MSG_ERROR, "Cannot create DVB read thread");
        cleanup_fast_signal_handling();
        return -1;
    }
    
    // Run optimized main loop
    int result = optimized_main_loop();
    
    // Signal all threads to shutdown
    signal_all_threads();
    
    // Wait for threads to complete
    pthread_join(card_thread, NULL);
    pthread_join(scanner_thread, NULL);
    pthread_join(dvb_thread, NULL);
    
    // Cleanup
    cleanup_fast_signal_handling();
    
    return result;
}
