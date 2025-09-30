/**
 * @file dvb_events_example.c
 * @brief Example integration of multi-frontend event monitoring
 * 
 * This file demonstrates how to integrate the new event-based DVB system
 * into the existing MuMuDVB codebase to replace usleep polling loops.
 */

#include "dvb_events.h"
#include "mumudvb.h"
#include "log.h"
#include "parallel_card_manager.h"

static char *log_module = "DVB-Events-Example: ";

// Global multi-frontend monitor for the entire system
static multi_fe_event_monitor_t *global_fe_monitor = NULL;

/**
 * @brief Event callback function for frontend events
 */
static void frontend_event_callback(int card_id, const fe_event_t *event, void *user_data)
{
    // Log significant events
    if (event->event_type & FE_EVENT_LOCK) {
        log_message(log_module, MSG_INFO, "card-%d achieved lock (strength: %d, SNR: %d)", 
                    card_id, event->signal_strength, event->snr);
    } else if (event->event_type & FE_EVENT_ERROR) {
        log_message(log_module, MSG_WARN, "card-%d frontend error (code: %d)", 
                    card_id, event->error_code);
    }
    
    // Update parallel card manager with new status
    // This replaces the old polling mechanism
    if (global_fe_monitor) {
        // Update card availability based on lock status
        int has_lock = (event->event_type & FE_EVENT_LOCK) ? 1 : 0;
        // You could call parallel card manager functions here to update status
    }
}

/**
 * @brief Initialize the global multi-frontend event monitor
 * @param max_cards Maximum number of cards to monitor
 * @return 0 on success, -1 on error
 */
int init_global_fe_event_monitor(int max_cards)
{
    if (global_fe_monitor) {
        log_message(log_module, MSG_WARN, "Global FE monitor already initialized");
        return 0;
    }
    
    global_fe_monitor = malloc(sizeof(multi_fe_event_monitor_t));
    if (!global_fe_monitor) {
        log_message(log_module, MSG_ERROR, "Cannot allocate global FE monitor");
        return -1;
    }
    
    if (create_multi_fe_event_monitor(max_cards, global_fe_monitor) < 0) {
        log_message(log_module, MSG_ERROR, "Cannot create global FE monitor");
        free(global_fe_monitor);
        global_fe_monitor = NULL;
        return -1;
    }
    
    // Start continuous monitoring with callback
    if (start_multi_fe_monitoring(global_fe_monitor, frontend_event_callback, NULL) < 0) {
        log_message(log_module, MSG_ERROR, "Cannot start global FE monitoring");
        destroy_multi_fe_event_monitor(global_fe_monitor);
        free(global_fe_monitor);
        global_fe_monitor = NULL;
        return -1;
    }
    
    log_message(log_module, MSG_INFO, "Global FE event monitor initialized for %d cards", max_cards);
    return 0;
}

/**
 * @brief Cleanup the global multi-frontend event monitor
 */
void cleanup_global_fe_event_monitor(void)
{
    if (global_fe_monitor) {
        stop_multi_fe_monitoring(global_fe_monitor);
        destroy_multi_fe_event_monitor(global_fe_monitor);
        free(global_fe_monitor);
        global_fe_monitor = NULL;
        log_message(log_module, MSG_INFO, "Global FE event monitor cleaned up");
    }
}

/**
 * @brief Add a card to the global event monitor
 * @param card_id The card ID to add
 * @param has_clients Whether this card has active clients
 * @return 0 on success, -1 on error
 */
int add_card_to_global_monitor(int card_id, int has_clients)
{
    if (!global_fe_monitor) {
        log_message(log_module, MSG_ERROR, "Global FE monitor not initialized");
        return -1;
    }
    
    return add_frontend_to_monitor(global_fe_monitor, card_id, has_clients, 1);
}

/**
 * @brief Remove a card from the global event monitor
 * @param card_id The card ID to remove
 * @return 0 on success, -1 on error
 */
int remove_card_from_global_monitor(int card_id)
{
    if (!global_fe_monitor) {
        return -1;
    }
    
    return remove_frontend_from_monitor(global_fe_monitor, card_id);
}

/**
 * @brief Update client status for a card
 * @param card_id The card ID
 * @param has_clients Whether this card has active clients
 * @return 0 on success, -1 on error
 */
int update_card_client_status(int card_id, int has_clients)
{
    if (!global_fe_monitor) {
        return -1;
    }
    
    return update_frontend_client_status(global_fe_monitor, card_id, has_clients);
}

/**
 * @brief Get current status for a card (non-blocking)
 * @param card_id The card ID
 * @param event Output event structure
 * @return 0 on success, -1 on error
 */
int get_card_status(int card_id, fe_event_t *event)
{
    if (!global_fe_monitor) {
        return -1;
    }
    
    return get_frontend_status(global_fe_monitor, card_id, event);
}

/**
 * @brief Event-based tuning replacement for old polling method
 * @param card_id The card ID to tune
 * @param tune_params Tuning parameters
 * @param timeout_ms Timeout in milliseconds
 * @return 0 on success, -1 on error
 */
int event_based_tune_card(int card_id, tune_p_t *tune_params, int timeout_ms)
{
    tuning_result_t result;
    
    if (event_based_tune(card_id, tune_params, timeout_ms, &result) < 0) {
        log_message(log_module, MSG_ERROR, "card-%d tuning failed", card_id);
        return -1;
    }
    
    if (result.success) {
        log_message(log_module, MSG_INFO, "card-%d tuning successful (strength: %d, SNR: %d)", 
                    card_id, result.signal_strength, result.snr);
        return 0;
    } else {
        log_message(log_module, MSG_INFO, "card-%d tuning failed (event: 0x%02x)", 
                    card_id, result.final_event);
        return -1;
    }
}

/**
 * @brief Event-based lock check replacement for old polling method
 * @param card_id The card ID to check
 * @param frequency The frequency to check
 * @param timeout_ms Timeout in milliseconds
 * @return 1 if locked, 0 if not locked, -1 on error
 */
int event_based_check_card_lock(int card_id, double frequency, int timeout_ms)
{
    tuning_result_t result;
    
    if (event_based_check_lock(card_id, frequency, timeout_ms, &result) < 0) {
        log_message(log_module, MSG_ERROR, "card-%d lock check failed", card_id);
        return -1;
    }
    
    return result.success ? 1 : 0;
}

/**
 * @brief Example: Replace the old background scanner polling loop
 * This shows how to replace the usleep-based polling in unified_channels.c
 */
int event_based_background_scanner(unified_channel_system_t *unified_system)
{
    if (!unified_system || !global_fe_monitor) {
        return -1;
    }
    
    log_message(log_module, MSG_INFO, "Starting event-based background scanner");
    
    // Add all cards to the monitor
    for (int i = 0; i < unified_system->num_cards; i++) {
        int card_id = unified_system->cards[i].card_id;
        
        // Check if card is available for testing
        if (!is_card_available_for_testing(card_id)) {
            log_message(log_module, MSG_DEBUG, "Skipping card-%d (currently in use)", card_id);
            continue;
        }
        
        // Add to monitor
        if (add_card_to_global_monitor(card_id, 0) < 0) {
            log_message(log_module, MSG_WARN, "Cannot add card-%d to monitor", card_id);
            continue;
        }
        
        log_message(log_module, MSG_DEBUG, "Added card-%d to background scanner monitor", card_id);
    }
    
    // Test each frequency on available cards
    for (int freq_idx = 1; freq_idx < unified_system->num_frequencies; freq_idx++) {
        double frequency = unified_system->frequencies[freq_idx];
        log_message(log_module, MSG_INFO, "Testing frequency %.0f Hz on all cards...", frequency);
        
        for (int card_idx = 0; card_idx < unified_system->num_cards; card_idx++) {
            int card_id = unified_system->cards[card_idx].card_id;
            
            // Check if card is available
            if (!is_card_available_for_testing(card_id)) {
                continue;
            }
            
            log_message(log_module, MSG_INFO, "Testing card-%d for frequency %.0f Hz", card_id, frequency);
            
            // Use event-based lock check instead of polling
            if (event_based_check_card_lock(card_id, frequency, 2000) > 0) {
                log_message(log_module, MSG_INFO, "card-%d can access frequency %.0f Hz", card_id, frequency);
                // Found a working card for this frequency
                break;
            } else {
                log_message(log_module, MSG_DEBUG, "card-%d cannot access frequency %.0f Hz", card_id, frequency);
            }
        }
    }
    
    log_message(log_module, MSG_INFO, "Event-based background scanner completed");
    return 0;
}
