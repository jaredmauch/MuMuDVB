/*
 * MuMuDVB - Unified Channel System for Multiple DVB Cards
 * 
 * This module implements a unified channel system that allows automatic
 * assignment of clients to available DVB cards based on frequency availability.
 *
 * (C) 2024 MuMuDVB Project
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#define _CRT_SECURE_NO_WARNINGS

#include "mumudvb.h"
#include "log.h"
#include "errors.h"
#include "dvb.h"
#include "unified_channel_storage_v2.h"
#include "unified_storage_adapter.h"
#include "autoconf.h"
#include "event_timing.h"
#include "main_thread_poll.h"
#include "sap.h"
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#ifndef DISABLE_DVB_API
#include <linux/dvb/frontend.h>
#endif
#include "tune.h"
#include "autoconf.h"
#include "unicast_http.h"

#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <pthread.h>
#include <fcntl.h>
#include <unistd.h>

// Forward declarations
static int test_card_frequency(unified_card_t *card, double frequency);
static void cleanup_card_availability_tracking(void);

// External function declarations
extern void register_thread(pthread_t thread, const char *name, void *(*cleanup_func)(void *), void *cleanup_arg);

static char *log_module = "Unified: ";

/** @brief Global event timing tracker for unified channels */
event_timing_tracker_t *global_unified_timing_tracker = NULL;

/** @brief Optimize card assignment to use minimum number of cards
 * @param unified_system The unified channel system
 */
static void optimize_card_assignment(unified_channel_system_t *unified_system);
int build_channel_list_for_frequency(unified_card_t *card, double frequency, int fd_frontend);
static int parse_transport_stream_for_channels(uint8_t *ts_data, ssize_t data_size, 
                                               unified_card_t *card, double frequency);
int start_unified_channel_serving(unified_channel_system_t *unified_system, fds_t *fds, 
                                  tune_p_t *tune_p, mumu_chan_p_t *chan_p, auto_p_t *auto_p,
                                  unicast_parameters_t *unic_p, multi_p_t *multi_p, 
                                  sap_p_t *sap_p, stats_infos_t *stats_infos, int server_id);

/** @brief Parse CSV list of frequencies
 * @param freq_string Comma-separated frequency string
 * @param frequencies Output array for frequencies
 * @param max_freqs Maximum number of frequencies to parse
 * @return Number of frequencies parsed, or -1 on error
 */
static int parse_frequency_list(const char *freq_string, double *frequencies, int max_freqs)
{
    char *freq_copy = strdup(freq_string);
    if (!freq_copy) {
        log_message(log_module, MSG_ERROR, "Memory allocation failed for frequency parsing");
        return -1;
    }
    
    int count = 0;
    char *token = strtok(freq_copy, ",");
    
    while (token && count < max_freqs) {
        // Trim whitespace
        while (*token == ' ' || *token == '\t') token++;
        char *end = token + strlen(token) - 1;
        while (end > token && (*end == ' ' || *end == '\t')) *end-- = '\0';
        
        double freq = atof(token);
        if (freq > 0) {
            frequencies[count++] = freq;
        }
        
        token = strtok(NULL, ",");
    }
    
    free(freq_copy);
    return count;
}

/** @brief Parse CSV list of card IDs
 * @param card_string Comma-separated card string
 * @param cards Output array for card IDs
 * @param max_cards Maximum number of cards to parse
 * @return Number of cards parsed, or -1 on error
 */
static int parse_card_list(const char *card_string, int *cards, int max_cards)
{
    char *card_copy = strdup(card_string);
    if (!card_copy) {
        log_message(log_module, MSG_ERROR, "Memory allocation failed for card parsing");
        return -1;
    }
    
    int count = 0;
    char *token = strtok(card_copy, ",");
    
    while (token && count < max_cards) {
        // Trim whitespace
        while (*token == ' ' || *token == '\t') token++;
        char *end = token + strlen(token) - 1;
        while (end > token && (*end == ' ' || *end == '\t')) *end-- = '\0';
        
        int card = atoi(token);
        if (card >= 0) {
            cards[count++] = card;
        }
        
        token = strtok(NULL, ",");
    }
    
    free(card_copy);
    return count;
}

/** @brief Data structure for card capability testing thread */

int read_unified_channel_configuration(unified_channel_system_t *unified_system, char *substring)
{
    
    if (!unified_system || !substring) {
        return -1;
    }
    
    // Handle old unified_tuners directive (ignore it)
    if (strcmp(substring, "unified_tuners") == 0) {
        log_message(log_module, MSG_WARN, "DEPRECATED: The option 'unified_tuners' is deprecated and will be removed in a future version. Please use 'unified_cards' instead.");
        return 1; // Successfully handled (ignored)
    }
    
    // Parse unified_cards directive
    if (strcmp(substring, "unified_cards") == 0) {
        char *card_string = strtok(NULL, " =");
        int *card_ids = malloc(16 * sizeof(int)); // Max 16 cards
        if (!card_ids) {
            log_message(log_module, MSG_ERROR, "Memory allocation failed for card parsing");
            return -1;
        }
        
        int num_cards;
        
        // Check if "auto" is specified
        if (!strcmp(card_string, "auto")) {
            num_cards = detect_available_cards(card_ids, 16);
            if (num_cards <= 0) {
                log_message(log_module, MSG_ERROR, "No DVB cards detected for auto-selection");
                free(card_ids);
                return -1;
            }
            log_message(log_module, MSG_INFO, "Auto-detected %d cards for unified system", num_cards);
            // Debug: Show detected adapter numbers
            for (int i = 0; i < num_cards; i++) {
                log_message(log_module, MSG_DEBUG, "Detected adapter %d for card %d", card_ids[i], i);
            }
        } else {
            num_cards = parse_card_list(card_string, card_ids, 16);
            if (num_cards <= 0) {
                log_message(log_module, MSG_ERROR, "Failed to parse unified_cards directive");
                free(card_ids);
                return -1;
            }
        }
        
        // Allocate memory for cards
        unified_system->cards = malloc(num_cards * sizeof(unified_card_t));
        if (!unified_system->cards) {
            log_message(log_module, MSG_ERROR, "Memory allocation failed for cards");
            free(card_ids);
            return -1;
        }
        
        unified_system->num_cards = num_cards;
        
        // Initialize each card using common initialization function
        for (int i = 0; i < num_cards; i++) {
            log_message(log_module, MSG_INFO, "Initializing card %d with adapter %d", i, card_ids[i]);
            if (init_card_common(&unified_system->cards[i], card_ids[i], 
                                NULL, NULL, NULL, NULL, -1, 1) != 0) {
                log_message(log_module, MSG_ERROR, "Failed to initialize card %d", card_ids[i]);
                // Clean up previously allocated cards
                for (int j = 0; j < i; j++) {
                    cleanup_unified_card(&unified_system->cards[j]);
                }
                free(unified_system->cards);
                unified_system->cards = NULL;
                unified_system->num_cards = 0;
                free(card_ids);
                return -1;
            }
        }
        
        free(card_ids);
        log_message(log_module, MSG_INFO, "Parsed %d cards for unified system", unified_system->num_cards);
        return 1; // Successfully parsed
    }
    
    // Parse unified_frequencies directive
    if (strcmp(substring, "unified_frequencies") == 0) {
        char *freq_string = strtok(NULL, " =");
        double *frequencies = malloc(64 * sizeof(double)); // Max 64 frequencies
        if (!frequencies) {
            log_message(log_module, MSG_ERROR, "Memory allocation failed for frequency parsing");
            return -1;
        }
        
        int num_frequencies = parse_frequency_list(freq_string, frequencies, 64);
        if (num_frequencies <= 0) {
            log_message(log_module, MSG_ERROR, "Failed to parse unified_frequencies directive");
            free(frequencies);
            return -1;
        }
        
        unified_system->frequencies = frequencies;
        unified_system->num_frequencies = num_frequencies;
        log_message(log_module, MSG_INFO, "Parsed %d frequencies for unified system", unified_system->num_frequencies);
        return 1; // Successfully parsed
    }
    
    return 0; // Not a unified directive
}

/** @brief Comprehensive initialization function for all cards (single and unified)
 * @param card The card to initialize
 * @param card_id The card ID
 * @param unic_p Unicast parameters (can be NULL)
 * @param multi_p Multicast parameters (can be NULL)
 * @param auto_p Auto parameters (can be NULL)
 * @param scam_vars SCAM variables (can be NULL)
 * @param server_id Server ID (-1 if not set)
 * @param is_unified Whether this is for unified system (affects some initialization)
 * @return 0 on success, -1 on error
 */
int init_card_common(unified_card_t *card, int card_id, 
                    unicast_parameters_t *unic_p, multi_p_t *multi_p, 
                    auto_p_t *auto_p, void *scam_vars, int server_id, int is_unified)
{
    (void) is_unified; // Suppress unused parameter warning
    
    if (!card) {
        return -1;
    }
    
    // Initialize basic card properties
    card->card_id = card_id;
    card->in_use = 0;
    card->current_freq = 0.0;
    card->available_frequencies = NULL;
    card->num_frequencies = 0;
    card->card_thread = 0;
    
    // Allocate and initialize tuning parameters
    card->tune_params = malloc(sizeof(tune_p_t));
    if (!card->tune_params) {
        log_message(log_module, MSG_ERROR, "Memory allocation failed for card %d tune_params", card_id);
        return -1;
    }
    
    init_tune_v(card->tune_params);
    card->tune_params->card = card_id;
    
    // Set up device path for this card
    char number[10];
    sprintf(number, "%d", card_id);
    int l = sizeof(card->tune_params->card_dev_path);
    strcpy(card->tune_params->card_dev_path, "/dev/dvb/adapter%card");
    mumu_string_replace(card->tune_params->card_dev_path, &l, 0, "%card", number);
    
    // Debug: Show device path being constructed
    log_message(log_module, MSG_INFO, "Card %d using device path: %s", card_id, card->tune_params->card_dev_path);
    
    // Auto-detect delivery system for this card
    auto_detect_delivery_system(card->tune_params);
    
    // Log the detected frontend type for debugging
    log_message(log_module, MSG_INFO, "Card %d detected frontend type: %d (delivery system: %d)", 
                card_id, card->tune_params->fe_type, card->tune_params->delivery_system);
    
    // Allocate file descriptors
    card->fds = malloc(sizeof(fds_t));
    if (!card->fds) {
        log_message(log_module, MSG_ERROR, "Memory allocation failed for card %d fds", card_id);
        free(card->tune_params);
        return -1;
    }
    memset(card->fds, 0, sizeof(fds_t));
    
    // Allocate channel parameters
    card->chan_p = malloc(sizeof(mumu_chan_p_t));
    if (!card->chan_p) {
        log_message(log_module, MSG_ERROR, "Memory allocation failed for card %d chan_p", card_id);
        free(card->tune_params);
        free(card->fds);
        return -1;
    }
    memset(card->chan_p, 0, sizeof(mumu_chan_p_t));
    
    // Initialize channel parameters mutex
    pthread_mutex_init(&card->chan_p->lock, NULL);
    card->chan_p->psi_tables_filtering = PSI_TABLES_FILTERING_NONE;
    
    // Allocate monitor parameters
    card->monitor_params = malloc(sizeof(monitor_parameters_t));
    if (!card->monitor_params) {
        log_message(log_module, MSG_ERROR, "Memory allocation failed for card %d monitor_params", card_id);
        free(card->tune_params);
        free(card->fds);
        free(card->chan_p);
        return -1;
    }
    
    // Initialize monitor parameters with common values
    memset(card->monitor_params, 0, sizeof(monitor_parameters_t));
    card->monitor_params->threadshutdown = 0;
    card->monitor_params->wait_time = 10;
    card->monitor_params->tune_p = card->tune_params;
    card->monitor_params->fds = card->fds;
    card->monitor_params->chan_p = card->chan_p;
    
    // Set common parameters if provided
    if (unic_p) {
        card->monitor_params->unicast_vars = unic_p;
    }
    if (multi_p) {
        card->monitor_params->multi_p = multi_p;
    }
    if (auto_p) {
        card->monitor_params->auto_p = auto_p;
    }
    if (scam_vars) {
        card->monitor_params->scam_vars_v = scam_vars;
    }
    if (server_id >= 0) {
        card->monitor_params->server_id = server_id;
    }
    
    // Initialize mandatory PIDs (same as single card mode)
    for (int ipid = 0; ipid < 21; ipid++) {
        card->chan_p->asked_pid[ipid] = 0; // Initialize all to 0 first
    }
    
    // Set mandatory PIDs (same as single card mode)
    card->chan_p->asked_pid[0] = PID_ASKED;   // PAT
    card->chan_p->asked_pid[1] = PID_ASKED;   // CAT
    card->chan_p->asked_pid[2] = PID_ASKED;   // PMT
    card->chan_p->asked_pid[16] = PID_ASKED;  // NIT
    card->chan_p->asked_pid[17] = PID_ASKED;  // SDT
    card->chan_p->asked_pid[18] = PID_ASKED;  // EIT
    card->chan_p->asked_pid[20] = PID_ASKED;  // TDT
    
    // PSIP for ATSC (same as single card mode)
    if (card->tune_params->fe_type == FE_ATSC) {
        card->chan_p->asked_pid[PSIP_PID] = PID_ASKED;
    }
    
    // Initialize file descriptors for this card (same as single card mode)
    log_message(log_module, MSG_DEBUG, "Creating file descriptors for card %d using path: %s, tuner: %d", card_id, card->tune_params->card_dev_path, card->tune_params->tuner);
    if (create_card_fd(card->tune_params->card_dev_path, card->tune_params->tuner, card->chan_p->asked_pid, card->fds) != 0) {
        log_message(log_module, MSG_ERROR, "Failed to create file descriptors for card %d using path: %s", card_id, card->tune_params->card_dev_path);
        cleanup_unified_card(card);
        return -1;
    }
    log_message(log_module, MSG_DEBUG, "Successfully created file descriptors for card %d (fd_dvr=%d)", card_id, card->fds->fd_dvr);
    
    // Set up filters (same as single card mode)
    set_filters(card->chan_p->asked_pid, card->fds);
    
    // Initialize poll descriptors (same as single card mode)
    card->fds->pfds = NULL;
    card->fds->pfdsnum = 1;
    card->fds->pfds = realloc(card->fds->pfds, (card->fds->pfdsnum + 1) * sizeof(struct pollfd));
    if (!card->fds->pfds) {
        log_message(log_module, MSG_ERROR, "Memory allocation failed for poll descriptors");
        cleanup_unified_card(card);
        return -1;
    }
    
    // Set up poll descriptors (same as single card mode)
    card->fds->pfds[0].fd = card->fds->fd_dvr;
    card->fds->pfds[0].events = POLLIN | POLLPRI;
    card->fds->pfds[0].revents = 0;
    card->fds->pfds[1].fd = 0;
    card->fds->pfds[1].events = POLLIN | POLLPRI;
    card->fds->pfds[1].revents = 0;
    
    // Initialize auto_p if not already set (for parallel processing compatibility)
    if (!card->auto_p) {
        card->auto_p = malloc(sizeof(auto_p_t));
        if (!card->auto_p) {
            log_message(log_module, MSG_ERROR, "Memory allocation failed for card %d auto_p", card_id);
            cleanup_unified_card(card);
            return -1;
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
            cleanup_unified_card(card);
            return -1;
        }
        
        log_message(log_module, MSG_DEBUG, "Card %d auto_p initialized successfully", card_id);
    }
    
    log_message(log_module, MSG_DEBUG, "Card %d initialized successfully", card_id);
    return 0;
}

/** @brief Cleanup function for unified cards
 * @param card The card to cleanup
 */
void cleanup_unified_card(unified_card_t *card)
{
    if (!card) {
        return;
    }
    
    // Free tuning parameters
    if (card->tune_params) {
        free(card->tune_params);
        card->tune_params = NULL;
    }
    
    // Free file descriptors - close them first!
    if (card->fds) {
        close_card_fd(card->fds);
        free(card->fds);
        card->fds = NULL;
    }
    
    // Free channel parameters
    if (card->chan_p) {
        pthread_mutex_destroy(&card->chan_p->lock);
        free(card->chan_p);
        card->chan_p = NULL;
    }
    
    // Free monitor parameters
    if (card->monitor_params) {
        free(card->monitor_params);
        card->monitor_params = NULL;
    }
    
    // Free available frequencies
    if (card->available_frequencies) {
        free(card->available_frequencies);
        card->available_frequencies = NULL;
    }
    
    // Reset basic properties
    card->card_id = -1;
    card->in_use = 0;
    card->current_freq = 0.0;
    card->num_frequencies = 0;
    card->card_thread = 0;
}

int init_unified_channel_system(unified_channel_system_t *unified_system)
{
    if (!unified_system) {
        return -1;
    }
    
    // Initialize mutex
    if (pthread_mutex_init(&unified_system->lock, NULL) != 0) {
        log_message(log_module, MSG_ERROR, "Failed to initialize mutex");
        return -1;
    }
    
    // Allocate frequency to card mapping
    unified_system->freq_to_card = malloc(unified_system->num_frequencies * sizeof(int));
    if (!unified_system->freq_to_card) {
        log_message(log_module, MSG_ERROR, "Memory allocation failed for frequency mapping");
        return -1;
    }
    
    // Initialize mapping to -1 (unassigned)
    for (int i = 0; i < unified_system->num_frequencies; i++) {
        unified_system->freq_to_card[i] = -1;
    }
    
    // Initialize event timing tracker
    global_unified_timing_tracker = malloc(sizeof(event_timing_tracker_t));
    if (!global_unified_timing_tracker) {
        log_message(log_module, MSG_ERROR, "Cannot allocate timing tracker");
        return -1;
    }
    
    if (create_event_timing_tracker(32, NULL, global_unified_timing_tracker) < 0) {
        log_message(log_module, MSG_ERROR, "Cannot create timing tracker");
        free(global_unified_timing_tracker);
        global_unified_timing_tracker = NULL;
        return -1;
    }
    
    // Start timing tracking
    if (start_event_timing_tracking(global_unified_timing_tracker, NULL, NULL) < 0) {
        log_message(log_module, MSG_ERROR, "Cannot start timing tracking");
        destroy_event_timing_tracker(global_unified_timing_tracker);
        free(global_unified_timing_tracker);
        global_unified_timing_tracker = NULL;
        return -1;
    }
    
    // Initialize new hierarchical storage system
    unified_system->unified_storage_v2 = malloc(sizeof(unified_channel_storage_v2_t));
    if (!unified_system->unified_storage_v2) {
        log_message(log_module, MSG_ERROR, "Failed to allocate memory for unified storage v2");
        destroy_event_timing_tracker(global_unified_timing_tracker);
        free(global_unified_timing_tracker);
        global_unified_timing_tracker = NULL;
        return -1;
    }
    
    if (init_unified_channel_storage_v2(unified_system->unified_storage_v2, 
                                       unified_system->num_cards, 64, 128) != 0) {
        log_message(log_module, MSG_ERROR, "Failed to initialize unified channel storage v2");
        free(unified_system->unified_storage_v2);
        destroy_event_timing_tracker(global_unified_timing_tracker);
        free(global_unified_timing_tracker);
        global_unified_timing_tracker = NULL;
        return -1;
    }
    
    // Initialize storage adapter
    if (init_storage_adapter(unified_system->unified_storage_v2) != 0) {
        log_message(log_module, MSG_ERROR, "Failed to initialize storage adapter");
        cleanup_unified_channel_storage_v2(unified_system->unified_storage_v2);
        free(unified_system->unified_storage_v2);
        destroy_event_timing_tracker(global_unified_timing_tracker);
        free(global_unified_timing_tracker);
        global_unified_timing_tracker = NULL;
        return -1;
    }
    
    log_message(log_module, MSG_INFO, "Unified channel system initialized with %d cards and %d frequencies",
                unified_system->num_cards, unified_system->num_frequencies);
    
    return 0;
}

void cleanup_unified_channel_system(unified_channel_system_t *unified_system)
{
    if (!unified_system) {
        return;
    }
    
    // Cleanup event timing tracker
    if (global_unified_timing_tracker) {
        stop_event_timing_tracking(global_unified_timing_tracker);
        destroy_event_timing_tracker(global_unified_timing_tracker);
        free(global_unified_timing_tracker);
        global_unified_timing_tracker = NULL;
    }
    
    // Cleanup new hierarchical storage system
    if (unified_system->unified_storage_v2) {
        cleanup_unified_channel_storage_v2(unified_system->unified_storage_v2);
        free(unified_system->unified_storage_v2);
        unified_system->unified_storage_v2 = NULL;
    }
    
    
    // Cleanup card availability tracking
    cleanup_card_availability_tracking();
    
    // Cancel and join all card threads
    for (int i = 0; i < unified_system->num_cards; i++) {
        if (unified_system->cards[i].card_thread != 0) {
            // Validate thread ID before attempting to cancel/join
            pthread_t thread_id = unified_system->cards[i].card_thread;
            if ((unsigned long)thread_id >= 0x1000 && (unsigned long)thread_id <= 0x7fffffffffff) {
                pthread_cancel(thread_id);
                int join_result = pthread_join(thread_id, NULL);
                if (join_result != 0) {
                    log_message(log_module, MSG_WARN, "Card %d thread join failed: %s", 
                               unified_system->cards[i].card_id, strerror(join_result));
                }
            } else {
                log_message(log_module, MSG_WARN, "Card %d has invalid thread ID: %lu, skipping join", 
                           unified_system->cards[i].card_id, (unsigned long)thread_id);
            }
            unified_system->cards[i].card_thread = 0;
        }
        
        // Free allocated memory - close file descriptors first!
        if (unified_system->cards[i].available_frequencies) {
            free(unified_system->cards[i].available_frequencies);
        }
        if (unified_system->cards[i].tune_params) {
            free(unified_system->cards[i].tune_params);
        }
        if (unified_system->cards[i].fds) {
            close_card_fd(unified_system->cards[i].fds);
            free(unified_system->cards[i].fds);
        }
        if (unified_system->cards[i].chan_p) {
            pthread_mutex_destroy(&unified_system->cards[i].chan_p->lock);
            free(unified_system->cards[i].chan_p);
        }
        if (unified_system->cards[i].monitor_params) {
            free(unified_system->cards[i].monitor_params);
        }
    }
    
    if (unified_system->cards) {
        free(unified_system->cards);
    }
    
    if (unified_system->frequencies) {
        free(unified_system->frequencies);
    }
    
    if (unified_system->freq_to_card) {
        free(unified_system->freq_to_card);
    }
    
    pthread_mutex_destroy(&unified_system->lock);
    
    log_message(log_module, MSG_INFO, "Unified channel system cleaned up");
}



/** @brief Parse transport stream to find channels using autoconf
 * @param ts_data Transport stream data
 * @param data_size Size of data
 * @param card Pointer to unified card structure
 * @param frequency Frequency being scanned
 * @return Number of channels found
 */
int parse_transport_stream_with_autoconf(uint8_t *ts_data, ssize_t data_size, 
                                         unified_card_t *card, double frequency);

/** @brief Build channel list for a specific frequency by reading transport stream
 * @param card Pointer to unified card structure
 * @param frequency Frequency to scan
 * @param fd_frontend Frontend file descriptor
 * @return Number of channels found
 */
int build_channel_list_for_frequency(unified_card_t *card, double frequency, int fd_frontend) {
    (void)fd_frontend; // Suppress unused parameter warning
    int fd_dvr = -1;
    char dvr_path[64];
    int channel_count = 0;
    
    // Open DVR device for this card
    snprintf(dvr_path, sizeof(dvr_path), "/dev/dvb/adapter%d/dvr0", card->card_id);
    fd_dvr = open(dvr_path, O_RDONLY | O_NONBLOCK);
    if (fd_dvr < 0) {
        log_message(log_module, MSG_DEBUG, "Cannot open DVR device %s for card %d (errno: %d)", 
                    dvr_path, card->card_id, errno);
        return 0;
    }
    
    // Read transport stream data to find channels using autoconf
    uint8_t ts_buffer[188 * 1000]; // Read 1000 TS packets for better channel discovery
    ssize_t bytes_read = read(fd_dvr, ts_buffer, sizeof(ts_buffer));
    
    log_message(log_module, MSG_DEBUG, "Card %d read %zd bytes from DVR device %s", 
                card->card_id, bytes_read, dvr_path);
    
    if (bytes_read > 0) {
        // Use autoconf to properly parse TS and discover channels
        log_message(log_module, MSG_DEBUG, "Card %d calling parse_transport_stream_with_autoconf with %zd bytes", 
                    card->card_id, bytes_read);
        channel_count = parse_transport_stream_with_autoconf(ts_buffer, bytes_read, card, frequency);
        log_message(log_module, MSG_DEBUG, "Card %d parse_transport_stream_with_autoconf returned %d channels", 
                    card->card_id, channel_count);
        
        if (channel_count > 0) {
            log_message(log_module, MSG_INFO, "Card %d found %d channels on frequency %.0f Hz", 
                        card->card_id, channel_count, frequency);
        }
    } else {
        log_message(log_module, MSG_DEBUG, "Card %d no data read from DVR on frequency %.0f Hz (bytes_read=%zd, errno=%d)", 
                    card->card_id, frequency, bytes_read, errno);
    }
    
    close(fd_dvr);
    return channel_count;
}

/** @brief Parse transport stream to find channels using autoconf
 * @param ts_data Transport stream data
 * @param data_size Size of data
 * @param card Pointer to unified card structure
 * @param frequency Frequency being scanned
 * @return Number of channels found
 */
int parse_transport_stream_with_autoconf(uint8_t *ts_data, ssize_t data_size, 
                                         unified_card_t *card, double frequency) {
    int channel_count = 0;
    int ts_packets = data_size / 188;
    
    log_message(log_module, MSG_DEBUG, "Parsing %d TS packets for card %d frequency %.0f Hz using autoconf", 
                ts_packets, card->card_id, frequency);
    
    // Initialize autoconf for this card if not already done (thread-safe)
    if (!card->autoconf_initialized) {
        // Use a simple mutex to prevent race conditions during initialization
        static pthread_mutex_t init_mutex = PTHREAD_MUTEX_INITIALIZER;
        pthread_mutex_lock(&init_mutex);
        
        // Double-check after acquiring lock (double-checked locking pattern)
        if (!card->autoconf_initialized) {
            // Allocate autoconf structure for this card
            card->auto_p = malloc(sizeof(auto_p_t));
            if (!card->auto_p) {
                log_message(log_module, MSG_ERROR, "Memory allocation failed for autoconf on card %d", card->card_id);
                pthread_mutex_unlock(&init_mutex);
                return 0;
            }
            
            // Initialize autoconf structure for this card
            card->auto_p->autoconfiguration = AUTOCONF_MODE_FULL;
            card->auto_p->autoconf_radios = 1;
            card->auto_p->autoconf_scrambled = 0;
            card->auto_p->time_start_autoconfiguration = time(NULL);
            
            // Initialize autoconf structures
            if (autoconf_init(card->auto_p) != 0) {
                log_message(log_module, MSG_ERROR, "Failed to initialize autoconf for card %d", card->card_id);
                free(card->auto_p);
                card->auto_p = NULL;
                pthread_mutex_unlock(&init_mutex);
                return 0;
            }
            
            // Initialize channel structure for this card (protected by chan_p lock)
            pthread_mutex_lock(&card->chan_p->lock);
            card->chan_p->number_of_channels = 0;
            pthread_mutex_unlock(&card->chan_p->lock);
            
            card->autoconf_initialized = 1;
            log_message(log_module, MSG_DEBUG, "Autoconf initialized for card %d", card->card_id);
        }
        pthread_mutex_unlock(&init_mutex);
    }
    
    // Process each TS packet through autoconf
    for (int i = 0; i < ts_packets; i++) {
        uint8_t *packet = ts_data + (i * 188);
        
        // Check for TS packet sync byte
        if (packet[0] != 0x47) {
            continue;
        }
        
        // Extract PID from packet header
        uint16_t pid = ((packet[1] & 0x1F) << 8) | packet[2];
        
        // Check for scrambling control
        uint8_t scrambling_control = (packet[3] & 0xC0) >> 6;
        
        // Process packet through autoconf (only if not scrambled or if we want scrambled channels)
        if (scrambling_control == 0 || card->auto_p->autoconf_scrambled) {
            // Create dummy structures needed by autoconf_new_packet
            fds_t dummy_fds = {0};
            tune_p_t dummy_tune = {0};
            multi_p_t dummy_multi = {0};
            unicast_parameters_t dummy_unic = {0};
            
            // Set the correct frontend type for this card (critical for SDT/PSIP processing)
            if (card->tune_params) {
                dummy_tune.fe_type = card->tune_params->fe_type;
                dummy_tune.card = card->card_id;
                dummy_tune.tuner = 0;
                snprintf(dummy_tune.card_dev_path, sizeof(dummy_tune.card_dev_path), "/dev/dvb/adapter%d", card->card_id);
                
                // Debug: Log frontend type for first few packets
                if (i < 3) {
                    log_message(log_module, MSG_DEBUG, "Card %d packet %d: fe_type=%d, pid=%d", 
                               card->card_id, i, dummy_tune.fe_type, pid);
                }
            }
            
            // Validate card structure before calling autoconf
            if (!card || !card->chan_p) {
                log_message(log_module, MSG_ERROR, "card-%d Invalid card structure (card=%p, chan_p=%p) in parse_transport_stream_with_autoconf", 
                           card ? card->card_id : -1, (void*)card, card ? (void*)card->chan_p : NULL);
                continue;
            }
            
            // Call autoconf to process this packet
            int ret = autoconf_new_packet(pid, packet, card->auto_p, &dummy_fds, 
                                        card->chan_p, &dummy_tune, &dummy_multi, 
                                        &dummy_unic, 0, NULL, card->card_id);
            
            if (ret != 0) {
                log_message(log_module, MSG_DEBUG, "Autoconf error %d for card %d packet %d", 
                           ret, card->card_id, i);
            }
        }
    }
    
    // Update channel names after processing all packets
    if (card->auto_p && card->chan_p->number_of_channels > 0) {
        log_message(log_module, MSG_DEBUG, "Card %d updating channel names after TS processing", card->card_id);
        autoconf_update_chan_name(card->chan_p, card->auto_p);
    }
    
    // Count discovered channels (protected by mutex)
    pthread_mutex_lock(&card->chan_p->lock);
    channel_count = card->chan_p->number_of_channels;
    
    if (channel_count > 0) {
        log_message(log_module, MSG_INFO, "Card %d discovered %d channels on frequency %.1f MHz", 
                    card->card_id, channel_count, frequency / 1000000.0);
        
        // Log all channel details
        for (int i = 0; i < channel_count; i++) { // Log all channels
            log_message(log_module, MSG_INFO, "  Card %d Channel %d: %s (Service ID: %d) [%.1f MHz]", 
                       card->card_id, i, card->chan_p->channels[i].name, card->chan_p->channels[i].service_id, frequency / 1000000.0);
        }
    } else {
        log_message(log_module, MSG_DEBUG, "Card %d no channels discovered on frequency %.1f MHz", 
                    card->card_id, frequency / 1000000.0);
    }
    pthread_mutex_unlock(&card->chan_p->lock);
    
    return channel_count;
}

/** @brief Parse transport stream to find channels (legacy method - kept for compatibility)
 * @param ts_data Transport stream data
 * @param data_size Size of data
 * @param card Pointer to unified card structure
 * @param frequency Frequency being scanned
 * @return Number of channels found
 */
static int parse_transport_stream_for_channels(uint8_t *ts_data, ssize_t data_size, 
                                               unified_card_t *card, double frequency) {
    int channel_count = 0;
    int pat_found = 0;
    
    // Look for PAT (Program Association Table) - PID 0x0000
    for (ssize_t i = 0; i < data_size - 188; i += 188) {
        uint8_t *packet = &ts_data[i];
        
        // Check for TS packet sync byte
        if (packet[0] != 0x47) {
            continue;
        }
        
        // Extract PID from packet header
        uint16_t pid = ((packet[1] & 0x1F) << 8) | packet[2];
        
        // Look for PAT (PID 0x0000)
        if (pid == 0x0000) {
            pat_found = 1;
            log_message(log_module, MSG_DEBUG, "Card %d found PAT on frequency %.0f Hz", 
                        card->card_id, frequency);
            
            // For now, just count this as finding channels
            // In a full implementation, we'd parse the PAT to get actual service IDs
            channel_count++;
            break; // Found PAT, that's enough to confirm channels exist
        }
    }
    
    if (!pat_found) {
        log_message(log_module, MSG_DEBUG, "Card %d no PAT found on frequency %.0f Hz", 
                    card->card_id, frequency);
    }
    
    return channel_count;
}


int detect_card_capabilities(unified_channel_system_t *unified_system)
{
    if (!unified_system || unified_system->num_cards == 0) {
        return -1;
    }
    
    log_message(log_module, MSG_INFO, "Detecting card capabilities sequentially...");
    
    // Process cards sequentially since they may share the same physical tuner
    for (int i = 0; i < unified_system->num_cards; i++) {
        log_message(log_module, MSG_DEBUG, "Testing card %d capabilities...", i);
        
        // Test each frequency for this card
        int accessible_frequencies = 0;
        for (int j = 0; j < unified_system->num_frequencies; j++) {
            double frequency = unified_system->frequencies[j];
            if (test_card_frequency(&unified_system->cards[i], frequency)) {
                accessible_frequencies++;
                log_message(log_module, MSG_INFO, "Card %d can access frequency %.0f Hz", i, frequency);
            }
        }
        
        unified_system->cards[i].num_frequencies = accessible_frequencies;
        
        if (accessible_frequencies > 0) {
            log_message(log_module, MSG_INFO, "Card %d can access %d frequencies", i, accessible_frequencies);
        } else {
            log_message(log_module, MSG_WARN, "Card %d cannot access any frequencies!", i);
        }
    }
    
    log_message(log_module, MSG_INFO, "Sequential card capability detection completed");
    
    // Optimize card assignment to use minimum number of cards
    optimize_card_assignment(unified_system);
    
    return 0;
}

/** @brief Optimize card assignment to use minimum number of cards
 * @param unified_system The unified channel system
 */
static void optimize_card_assignment(unified_channel_system_t *unified_system)
{
    if (!unified_system) {
        return;
    }
    
    log_message(log_module, MSG_INFO, "Optimizing card assignment based on signal detection results...");
    
    // Initialize all frequencies as unassigned
    for (int i = 0; i < unified_system->num_frequencies; i++) {
        unified_system->freq_to_card[i] = -1;
    }
    
    // Track which cards are used
    int *card_used = calloc(unified_system->num_cards, sizeof(int));
    if (!card_used) {
        log_message(log_module, MSG_ERROR, "Memory allocation failed for card usage tracking");
        return;
    }
    
    int total_cards_used = 0;
    int frequencies_assigned = 0;
    
    // Create a matrix of card-frequency capabilities
    log_message(log_module, MSG_INFO, "Card-Frequency Signal Matrix:");
    for (int card_idx = 0; card_idx < unified_system->num_cards; card_idx++) {
        log_message(log_module, MSG_INFO, "  Card %d can receive %d frequencies:", 
                    unified_system->cards[card_idx].card_id, unified_system->cards[card_idx].num_frequencies);
        for (int j = 0; j < unified_system->cards[card_idx].num_frequencies; j++) {
            log_message(log_module, MSG_INFO, "    - %.0f Hz", 
                        unified_system->cards[card_idx].available_frequencies[j]);
        }
    }
    
    // Assign each frequency to the first available card that can access it
    for (int freq_idx = 0; freq_idx < unified_system->num_frequencies; freq_idx++) {
        double target_freq = unified_system->frequencies[freq_idx];
        int assigned = 0;
        
        // First pass: try to assign to already used cards (minimize card usage)
        for (int card_idx = 0; card_idx < unified_system->num_cards && !assigned; card_idx++) {
            if (card_used[card_idx]) {
                // Check if this card can access the target frequency
                for (int j = 0; j < unified_system->cards[card_idx].num_frequencies; j++) {
                    if (unified_system->cards[card_idx].available_frequencies[j] == target_freq) {
                        unified_system->freq_to_card[freq_idx] = card_idx;
                        assigned = 1;
                        frequencies_assigned++;
                        log_message(log_module, MSG_INFO, "Assigned frequency %.0f Hz to existing card %d", 
                                    target_freq, unified_system->cards[card_idx].card_id);
                        break;
                    }
                }
            }
        }
        
        // Second pass: if not assigned, use a new card
        if (!assigned) {
            for (int card_idx = 0; card_idx < unified_system->num_cards && !assigned; card_idx++) {
                for (int j = 0; j < unified_system->cards[card_idx].num_frequencies; j++) {
                    if (unified_system->cards[card_idx].available_frequencies[j] == target_freq) {
                        unified_system->freq_to_card[freq_idx] = card_idx;
                        card_used[card_idx] = 1;
                        total_cards_used++;
                        frequencies_assigned++;
                        assigned = 1;
                        log_message(log_module, MSG_INFO, "Assigned frequency %.0f Hz to new card %d", 
                                    target_freq, unified_system->cards[card_idx].card_id);
                        break;
                    }
                }
            }
        }
        
        if (!assigned) {
            log_message(log_module, MSG_WARN, "No card can receive frequency %.0f Hz", target_freq);
        }
    }
    
    free(card_used);
    
    log_message(log_module, MSG_INFO, "Optimization complete: %d cards used to cover %d/%d frequencies", 
                total_cards_used, frequencies_assigned, unified_system->num_frequencies);
}

int assign_card_to_frequency(unified_channel_system_t *unified_system, double frequency)
{
    if (!unified_system) {
        return -1;
    }
    
    pthread_mutex_lock(&unified_system->lock);
    
    // Find the frequency in our list and get its assigned card
    for (int i = 0; i < unified_system->num_frequencies; i++) {
        if (unified_system->frequencies[i] == frequency) {
            int card_idx = unified_system->freq_to_card[i];
            if (card_idx >= 0 && card_idx < unified_system->num_cards) {
                unified_system->cards[card_idx].current_freq = frequency;
                log_message(log_module, MSG_DEBUG, "Assigned frequency %.0f Hz to card %d", 
                            frequency, unified_system->cards[card_idx].card_id);
                pthread_mutex_unlock(&unified_system->lock);
                return card_idx;
            }
        }
    }
    
    pthread_mutex_unlock(&unified_system->lock);
    return -1; // Frequency not found or no card assigned
}

int get_available_card_for_frequency(unified_channel_system_t *unified_system, double frequency)
{
    if (!unified_system) {
        return -1;
    }
    
    pthread_mutex_lock(&unified_system->lock);
    
    // Find the frequency in our list and get its assigned card
    for (int i = 0; i < unified_system->num_frequencies; i++) {
        if (unified_system->frequencies[i] == frequency) {
            int card_idx = unified_system->freq_to_card[i];
            if (card_idx >= 0 && card_idx < unified_system->num_cards) {
                pthread_mutex_unlock(&unified_system->lock);
                return card_idx;
            }
        }
    }
    
    pthread_mutex_unlock(&unified_system->lock);
    return -1; // Frequency not found or no card assigned
}

/** @brief Card thread function (placeholder for future implementation)
 * @param arg Pointer to unified_card_t structure
 * @return NULL
 */
void *unified_card_thread(void *arg)
{
    unified_card_t *card = (unified_card_t *)arg;
    
    log_message(log_module, MSG_INFO, "Starting card thread for card %d", card->card_id);
    
    // TODO: Implement card thread logic
    // This would handle the actual tuning and streaming for the card
    
    while (1) {
        // Check for interrupt signal first
        if (get_interrupted()) {
            log_message(log_module, MSG_INFO, "Card %d polling thread shutting down due to interrupt signal", card->card_id);
            return NULL;
        }
        
        // Check if card is in use
        if (card->in_use == 0) {
            // Card is idle, wait for polling event instead of usleep
            if (global_unified_timing_tracker) {
                timing_event_t event;
                int wait_result = wait_for_timing_event(global_unified_timing_tracker, 
                                                      TIMING_EVENT_POLL_INTERVAL, 
                                                      TIMING_POLL_INTERVAL_MS, &event);
                if (wait_result < 0 && wait_result != -1) { // -1 is timeout, which is OK
                    log_message(log_module, MSG_DEBUG, "Card %d polling event wait failed: %d", 
                               card->card_id, wait_result);
                }
            } else {
                // Fallback to interruptible sleep if timing tracker not available
                if (event_sleep_interruptible(MS_TO_US(TIMING_POLL_INTERVAL_MS)) < 0) {
                    return NULL; // Interrupted
                }
            }
            continue;
        }
        
        // TODO: Implement actual tuning and streaming logic here
        // Use event-based timing instead of usleep
        if (global_unified_timing_tracker) {
            timing_event_t event;
            int wait_result = wait_for_timing_event(global_unified_timing_tracker, 
                                                  TIMING_EVENT_POLL_INTERVAL, 
                                                  TIMING_POLL_INTERVAL_MS * 2, &event);
            if (wait_result < 0 && wait_result != -1) { // -1 is timeout, which is OK
                log_message(log_module, MSG_DEBUG, "Card %d main loop event wait failed: %d", 
                           card->card_id, wait_result);
            }
        } else {
            // Fallback to interruptible sleep if timing tracker not available
            if (event_sleep_interruptible(MS_TO_US(TIMING_POLL_INTERVAL_MS * 2)) < 0) {
                return NULL; // Interrupted
            }
        }
    }
    
    log_message(log_module, MSG_INFO, "Card thread for card %d stopped", card->card_id);
    return NULL;
}

/** @brief Start serving channels discovered by the unified system
 * @param unified_system The unified channel system
 * @param fds File descriptors for DVB devices
 * @param tune_p Tuning parameters
 * @param chan_p Channel parameters
 * @param auto_p Autoconfiguration parameters
 * @param unic_p Unicast parameters
 * @param multi_p Multicast parameters
 * @param sap_p SAP parameters
 * @param stats_infos Statistics information
 * @param server_id Server ID
 * @return 0 on success, -1 on error
 */
// Note: frequency_channel_info_t is now defined in mumudvb.h

// Structure to track card/frequency compatibility
typedef struct {
    int card_id;
    double frequency;
    int is_usable; // 1 = usable, 0 = unusable, -1 = not tested yet
    time_t last_tested;
} card_frequency_compatibility_t;

// Structure to track card availability and status
typedef struct {
    int card_id;
    int is_available; // 1 = available for new frequencies, 0 = in use
    double current_frequency; // Current frequency being tuned (0.0 if idle)
    int client_count; // Number of clients using this card
    int is_streaming; // 1 = actively streaming data, 0 = idle
    time_t last_activity; // Last time this card was active
    pthread_mutex_t card_mutex; // Mutex for thread-safe access
} card_availability_t;


// Structure to track detailed card utilization
typedef struct {
    int card_id;
    double current_frequency;
    int total_clients; // Total clients using this card
    int is_tuning; // 1 if currently tuning, 0 if idle
    int is_streaming; // 1 if actively streaming, 0 if idle
    char usage_type[32]; // "main_system", "parallel_manager", "background_scanner", etc.
    time_t last_activity;
    time_t tuning_start_time;
    time_t streaming_start_time;
    pthread_mutex_t utilization_mutex;
} card_utilization_t;

// Structure to track channel-specific client counts
typedef struct {
    int card_id;
    double frequency;
    char channel_name[64];
    int service_id;
    int client_count;
    time_t last_client_activity;
} channel_client_info_t;

// Structure to track frequency scanning completion
typedef struct {
    int card_id;
    double frequency;
    int scan_completed;
    int has_clients;
    time_t scan_completion_time;
    time_t last_client_check;
} frequency_scan_status_t;

// Global arrays to track discovered channels and compatibility
static frequency_channel_info_t discovered_channels[64]; // Max 64 frequencies
static int discovered_channel_count = 0;
static card_frequency_compatibility_t card_freq_compatibility[256]; // Max 16 cards * 16 frequencies
static int compatibility_count = 0;
static card_availability_t card_availability[16]; // Max 16 cards
static int card_availability_count = 0;

// Global card utilization tracking
static card_utilization_t card_utilization[16]; // Max 16 cards
static int card_utilization_count = 0;
static channel_client_info_t channel_clients[128]; // Max 128 channel/client combinations
static int channel_client_count = 0;
static frequency_scan_status_t frequency_scan_status[256]; // Max 16 cards * 16 frequencies
static int frequency_scan_count = 0;
static pthread_mutex_t utilization_global_mutex = PTHREAD_MUTEX_INITIALIZER;

// Additional mutex for global static variables
static pthread_mutex_t global_static_mutex = PTHREAD_MUTEX_INITIALIZER;

// Helper function to check if a card/frequency combination is known to be unusable
static int is_card_frequency_unusable(int card_id, double frequency)
{
    for (int i = 0; i < compatibility_count; i++) {
        if (card_freq_compatibility[i].card_id == card_id && 
            card_freq_compatibility[i].frequency == frequency) {
            return (card_freq_compatibility[i].is_usable == 0);
        }
    }
    return 0; // Not tested yet, so assume usable
}

// Initialize card utilization tracking
static void init_card_utilization_tracking(void)
{
    pthread_mutex_lock(&utilization_global_mutex);
    
    for (int i = 0; i < 16; i++) {
        card_utilization[i].card_id = -1; // Mark as unused
        card_utilization[i].current_frequency = 0.0;
        card_utilization[i].total_clients = 0;
        card_utilization[i].is_tuning = 0;
        card_utilization[i].is_streaming = 0;
        strcpy(card_utilization[i].usage_type, "idle");
        card_utilization[i].last_activity = 0;
        card_utilization[i].tuning_start_time = 0;
        card_utilization[i].streaming_start_time = 0;
        pthread_mutex_init(&card_utilization[i].utilization_mutex, NULL);
    }
    
    card_utilization_count = 0;
    channel_client_count = 0;
    
    pthread_mutex_unlock(&utilization_global_mutex);
}

// Register a card as being used
void register_card_usage(int card_id, double frequency, const char *usage_type)
{
    pthread_mutex_lock(&utilization_global_mutex);
    
    // Find existing entry or create new one
    int idx = -1;
    for (int i = 0; i < 16; i++) {
        if (card_utilization[i].card_id == card_id) {
            idx = i;
            break;
        } else if (card_utilization[i].card_id == -1 && idx == -1) {
            idx = i; // Use first empty slot
        }
    }
    
    if (idx >= 0) {
        pthread_mutex_lock(&card_utilization[idx].utilization_mutex);
        
        if (card_utilization[idx].card_id == -1) {
            // New card
            card_utilization[idx].card_id = card_id;
            card_utilization_count++;
        } else if (strcmp(card_utilization[idx].usage_type, "idle") == 0) {
            // Card was idle, reactivate it
            log_message(log_module, MSG_DEBUG, "Reactivating idle card %d for %s", card_id, usage_type);
        }
        
        card_utilization[idx].current_frequency = frequency;
        strncpy(card_utilization[idx].usage_type, usage_type, sizeof(card_utilization[idx].usage_type) - 1);
        card_utilization[idx].usage_type[sizeof(card_utilization[idx].usage_type) - 1] = '\0';
        card_utilization[idx].last_activity = time(NULL);
        
        if (strcmp(usage_type, "tuning") == 0 || strcmp(usage_type, "main_system_tuning") == 0) {
            card_utilization[idx].is_tuning = 1;
            card_utilization[idx].tuning_start_time = time(NULL);
        } else if (strcmp(usage_type, "streaming") == 0) {
            card_utilization[idx].is_streaming = 1;
            card_utilization[idx].streaming_start_time = time(NULL);
        }
        
        pthread_mutex_unlock(&card_utilization[idx].utilization_mutex);
    }
    
    pthread_mutex_unlock(&utilization_global_mutex);
}

// Unregister a card usage
void unregister_card_usage(int card_id, const char *usage_type)
{
    pthread_mutex_lock(&utilization_global_mutex);
    
    for (int i = 0; i < 16; i++) {
        if (card_utilization[i].card_id == card_id) {
            pthread_mutex_lock(&card_utilization[i].utilization_mutex);
            
            if (strcmp(usage_type, "tuning") == 0 || strcmp(usage_type, "main_system_tuning") == 0) {
                card_utilization[i].is_tuning = 0;
            } else if (strcmp(usage_type, "streaming") == 0) {
                card_utilization[i].is_streaming = 0;
            } else if (strcmp(usage_type, "parallel_system_tuning") == 0) {
                card_utilization[i].is_tuning = 0; // parallel_system_tuning also sets is_tuning
            }
            
            // If card is completely idle, mark as unused
            if (!card_utilization[i].is_tuning && !card_utilization[i].is_streaming && 
                card_utilization[i].total_clients == 0) {
                strcpy(card_utilization[i].usage_type, "idle");
                card_utilization[i].current_frequency = 0.0;
                // Reset card_id to -1 to mark slot as unused
                card_utilization[i].card_id = -1;
                card_utilization_count--;
            }
            
            pthread_mutex_unlock(&card_utilization[i].utilization_mutex);
            break;
        }
    }
    
    pthread_mutex_unlock(&utilization_global_mutex);
}

// Check if a card is currently in use
int is_card_in_use(int card_id)
{
    pthread_mutex_lock(&utilization_global_mutex);
    
    int in_use = 0;
    for (int i = 0; i < 16; i++) {
        if (card_utilization[i].card_id == card_id) {
            pthread_mutex_lock(&card_utilization[i].utilization_mutex);
            // Allow parallel scanning to use cards that are only marked as "main_system_tuning"
            // but not actively streaming or serving clients
            if (strcmp(card_utilization[i].usage_type, "main_system_tuning") == 0) {
                // Main system tuning doesn't prevent parallel scanning
                in_use = (card_utilization[i].is_streaming || card_utilization[i].total_clients > 0);
            } else {
                // Other usage types (streaming, clients) do prevent parallel scanning
                in_use = (card_utilization[i].is_tuning || card_utilization[i].is_streaming || 
                         card_utilization[i].total_clients > 0);
            }
            pthread_mutex_unlock(&card_utilization[i].utilization_mutex);
            break;
        }
    }
    
    pthread_mutex_unlock(&utilization_global_mutex);
    return in_use;
}

// Mark a frequency scan as completed for a card
static void mark_frequency_scan_completed(int card_id, double frequency)
{
    pthread_mutex_lock(&utilization_global_mutex);
    
    // Find existing entry or create new one
    int idx = -1;
    for (int i = 0; i < 256; i++) {
        if (frequency_scan_status[i].card_id == card_id && 
            frequency_scan_status[i].frequency == frequency) {
            idx = i;
            break;
        } else if (frequency_scan_status[i].card_id == -1 && idx == -1) {
            idx = i; // Use first empty slot
        }
    }
    
    if (idx >= 0) {
        if (frequency_scan_status[idx].card_id == -1) {
            // New entry
            frequency_scan_status[idx].card_id = card_id;
            frequency_scan_status[idx].frequency = frequency;
            frequency_scan_count++;
        }
        
        frequency_scan_status[idx].scan_completed = 1;
        frequency_scan_status[idx].scan_completion_time = time(NULL);
        frequency_scan_status[idx].last_client_check = time(NULL);
        
        log_message(log_module, MSG_INFO, "Marked frequency %.0f Hz scan as completed for card %d", 
                   frequency, card_id);
    }
    
    pthread_mutex_unlock(&utilization_global_mutex);
}

// Check if a card/frequency combination has any clients
static int has_clients_on_frequency(int card_id, double frequency)
{
    pthread_mutex_lock(&utilization_global_mutex);
    
    int has_clients = 0;
    for (int i = 0; i < 128; i++) {
        if (channel_clients[i].card_id == card_id && 
            channel_clients[i].frequency == frequency &&
            channel_clients[i].client_count > 0) {
            has_clients = 1;
            break;
        }
    }
    
    pthread_mutex_unlock(&utilization_global_mutex);
    return has_clients;
}

// Update client count for a specific card/frequency/channel combination
void update_channel_client_count(int card_id, double frequency, const char *channel_name, 
                                int service_id, int client_count)
{
    pthread_mutex_lock(&utilization_global_mutex);
    
    // Find existing entry or create new one
    int idx = -1;
    for (int i = 0; i < 128; i++) {
        if (channel_clients[i].card_id == card_id && 
            channel_clients[i].frequency == frequency &&
            channel_clients[i].service_id == service_id) {
            idx = i;
            break;
        } else if (channel_clients[i].card_id == -1 && idx == -1) {
            idx = i; // Use first empty slot
        }
    }
    
    if (idx >= 0) {
        if (channel_clients[idx].card_id == -1) {
            // New entry
            channel_clients[idx].card_id = card_id;
            channel_clients[idx].frequency = frequency;
            channel_clients[idx].service_id = service_id;
            strncpy(channel_clients[idx].channel_name, channel_name, 
                   sizeof(channel_clients[idx].channel_name) - 1);
            channel_clients[idx].channel_name[sizeof(channel_clients[idx].channel_name) - 1] = '\0';
            channel_client_count++;
        }
        
        channel_clients[idx].client_count = client_count;
        channel_clients[idx].last_client_activity = time(NULL);
    }
    
    pthread_mutex_unlock(&utilization_global_mutex);
}

// Check if a card can be released (no clients on any frequency)
int can_release_card(int card_id)
{
    pthread_mutex_lock(&utilization_global_mutex);
    
    int can_release = 1;
    for (int i = 0; i < 128; i++) {
        if (channel_clients[i].card_id == card_id && 
            channel_clients[i].client_count > 0) {
            can_release = 0;
            break;
        }
    }
    
    pthread_mutex_unlock(&utilization_global_mutex);
    return can_release;
}

// Release a card from frequency scanning (mark as available for other frequencies)
static void release_card_from_frequency(int card_id, double frequency)
{
    pthread_mutex_lock(&utilization_global_mutex);
    
    // Update card utilization to mark as available
    for (int i = 0; i < 16; i++) {
        if (card_utilization[i].card_id == card_id) {
            pthread_mutex_lock(&card_utilization[i].utilization_mutex);
            
            // If no clients on any frequency, mark as idle
            if (can_release_card(card_id)) {
                strcpy(card_utilization[i].usage_type, "idle");
                card_utilization[i].current_frequency = 0.0;
                card_utilization[i].is_streaming = 0;
                card_utilization[i].is_tuning = 0;
                
                log_message(log_module, MSG_INFO, "Released card %d from frequency %.0f Hz - no clients", 
                           card_id, frequency);
            } else {
                log_message(log_module, MSG_DEBUG, "Card %d still has clients on other frequencies - keeping active", 
                           card_id);
            }
            
            pthread_mutex_unlock(&card_utilization[i].utilization_mutex);
            break;
        }
    }
    
    pthread_mutex_unlock(&utilization_global_mutex);
}

// Periodic check to release cards that have completed scanning and have no clients
void check_and_release_idle_cards(void)
{
    pthread_mutex_lock(&utilization_global_mutex);
    
    time_t now = time(NULL);
    int released_count = 0;
    
    for (int i = 0; i < 256; i++) {
        if (frequency_scan_status[i].card_id >= 0 && 
            frequency_scan_status[i].scan_completed &&
            !frequency_scan_status[i].has_clients) {
            
            // Check if enough time has passed since scan completion (e.g., 30 seconds)
            if (now - frequency_scan_status[i].scan_completion_time > 30) {
                
                // Double-check if there are any clients on this frequency
                if (!has_clients_on_frequency(frequency_scan_status[i].card_id, 
                                            frequency_scan_status[i].frequency)) {
                    
                    release_card_from_frequency(frequency_scan_status[i].card_id, 
                                              frequency_scan_status[i].frequency);
                    released_count++;
                    
                    // Mark this entry as processed
                    frequency_scan_status[i].card_id = -1;
                    frequency_scan_status[i].frequency = 0.0;
                    frequency_scan_status[i].scan_completed = 0;
                    frequency_scan_status[i].has_clients = 0;
                }
            }
        }
    }
    
    if (released_count > 0) {
        log_message(log_module, MSG_INFO, "Released %d idle cards for reuse", released_count);
    }
    
    pthread_mutex_unlock(&utilization_global_mutex);
}

// Helper function to check if a card is being used by the main streaming system
int is_card_used_by_main_system(int card_id)
{
    // Check if the card is currently tuned and streaming (has active traffic)
    // This is a simple check - in a full implementation, we'd track this more precisely
    char frontend_path[256];
    snprintf(frontend_path, sizeof(frontend_path), "/dev/dvb/adapter%d/frontend0", card_id);
    
    int fd_frontend = open(frontend_path, O_RDONLY | O_NONBLOCK);
    if (fd_frontend < 0) {
        return 0; // Card not accessible, assume not in use
    }
    
    // Check if frontend is locked (indicating active tuning)
    // Use non-blocking approach to avoid blocking the background scanner
    fe_status_t fe_status;
    int status_ret = ioctl(fd_frontend, FE_READ_STATUS, &fe_status);
    close(fd_frontend);
    
    if (status_ret >= 0 && (fe_status & FE_HAS_LOCK)) {
        return 1; // Card is locked and in use by main system
    }
    
    return 0; // Card is not locked, available for testing
}

// Generate card utilization status as JSON
int generate_card_utilization_json(char *buffer, size_t buffer_size)
{
    int written = 0;
    time_t now = time(NULL);
    int remaining;
    
    remaining = buffer_size - written;
    if (remaining > 0) {
        int result = snprintf(buffer + written, remaining, 
                           "{\n  \"card_utilization\": [\n");
        if (result > 0 && result < remaining) {
            written += result;
        }
    }
    
    pthread_mutex_lock(&utilization_global_mutex);
    
    int first_card = 1;
    for (int i = 0; i < 16; i++) {
        if (card_utilization[i].card_id >= 0) {
            pthread_mutex_lock(&card_utilization[i].utilization_mutex);
            
            if (!first_card) {
                remaining = buffer_size - written;
                if (remaining > 0) {
                    int result = snprintf(buffer + written, remaining, ",\n");
                    if (result > 0 && result < remaining) {
                        written += result;
                    }
                }
            }
            first_card = 0;
            
            remaining = buffer_size - written;
            if (remaining > 0) {
                int result = snprintf(buffer + written, remaining,
                                   "    {\n"
                                   "      \"card_id\": %d,\n"
                                   "      \"frequency\": %.0f,\n"
                                   "      \"usage_type\": \"%s\",\n"
                                   "      \"is_tuning\": %s,\n"
                                   "      \"is_streaming\": %s,\n"
                                   "      \"total_clients\": %d,\n"
                                   "      \"last_activity\": %ld,\n"
                                   "      \"tuning_duration\": %ld,\n"
                                   "      \"streaming_duration\": %ld\n"
                                   "    }",
                                   card_utilization[i].card_id,
                                   card_utilization[i].current_frequency,
                                   card_utilization[i].usage_type,
                                   card_utilization[i].is_tuning ? "true" : "false",
                                   card_utilization[i].is_streaming ? "true" : "false",
                                   card_utilization[i].total_clients,
                                   card_utilization[i].last_activity,
                                   card_utilization[i].is_tuning ? (now - card_utilization[i].tuning_start_time) : 0,
                                   card_utilization[i].is_streaming ? (now - card_utilization[i].streaming_start_time) : 0);
                if (result > 0 && result < remaining) {
                    written += result;
                } else {
                    break;
                }
            } else {
                break;
            }
            
            pthread_mutex_unlock(&card_utilization[i].utilization_mutex);
        }
    }
    
    pthread_mutex_unlock(&utilization_global_mutex);
    
    remaining = buffer_size - written;
    if (remaining > 0) {
        int result = snprintf(buffer + written, remaining, 
                           "\n  ],\n"
                           "  \"channel_clients\": [\n");
        if (result > 0 && result < remaining) {
            written += result;
        }
    }
    
    // Add channel client information
    first_card = 1;
    for (int i = 0; i < 128; i++) {
        if (channel_clients[i].card_id >= 0) {
            if (!first_card) {
                remaining = buffer_size - written;
                if (remaining > 0) {
                    int result = snprintf(buffer + written, remaining, ",\n");
                    if (result > 0 && result < remaining) {
                        written += result;
                    }
                }
            }
            first_card = 0;
            
            remaining = buffer_size - written;
            if (remaining > 0) {
                int result = snprintf(buffer + written, remaining,
                                   "    {\n"
                                   "      \"card_id\": %d,\n"
                                   "      \"frequency\": %.0f,\n"
                                   "      \"channel_name\": \"%s\",\n"
                                   "      \"service_id\": %d,\n"
                                   "      \"client_count\": %d,\n"
                                   "      \"last_client_activity\": %ld\n"
                                   "    }",
                                   channel_clients[i].card_id,
                                   channel_clients[i].frequency,
                                   channel_clients[i].channel_name,
                                   channel_clients[i].service_id,
                                   channel_clients[i].client_count,
                                   channel_clients[i].last_client_activity);
                if (result > 0 && result < remaining) {
                    written += result;
                } else {
                    // Buffer overflow, stop writing
                    break;
                }
            } else {
                break;
            }
        }
    }
    
    remaining = buffer_size - written;
    if (remaining > 0) {
        int result = snprintf(buffer + written, remaining, 
                           "\n  ]\n}\n");
        if (result > 0 && result < remaining) {
            written += result;
        }
    }
    
    return written;
}

// Helper function to mark a card/frequency combination as unusable
static void mark_card_frequency_unusable(int card_id, double frequency)
{
    // Check if already exists
    for (int i = 0; i < compatibility_count; i++) {
        if (card_freq_compatibility[i].card_id == card_id && 
            card_freq_compatibility[i].frequency == frequency) {
            card_freq_compatibility[i].is_usable = 0;
            card_freq_compatibility[i].last_tested = time(NULL);
            return;
        }
    }
    
    // Add new entry
    if (compatibility_count < 256) {
        card_freq_compatibility[compatibility_count].card_id = card_id;
        card_freq_compatibility[compatibility_count].frequency = frequency;
        card_freq_compatibility[compatibility_count].is_usable = 0;
        card_freq_compatibility[compatibility_count].last_tested = time(NULL);
        compatibility_count++;
    }
}

// Helper function to mark a card/frequency combination as usable
static void mark_card_frequency_usable(int card_id, double frequency)
{
    // Check if already exists
    for (int i = 0; i < compatibility_count; i++) {
        if (card_freq_compatibility[i].card_id == card_id && 
            card_freq_compatibility[i].frequency == frequency) {
            card_freq_compatibility[i].is_usable = 1;
            card_freq_compatibility[i].last_tested = time(NULL);
            return;
        }
    }
    
    // Add new entry
    if (compatibility_count < 256) {
        card_freq_compatibility[compatibility_count].card_id = card_id;
        card_freq_compatibility[compatibility_count].frequency = frequency;
        card_freq_compatibility[compatibility_count].is_usable = 1;
        card_freq_compatibility[compatibility_count].last_tested = time(NULL);
        compatibility_count++;
    }
}

/** @brief Cleanup card availability tracking
 */
static void cleanup_card_availability_tracking(void)
{
    for (int i = 0; i < card_availability_count; i++) {
        pthread_mutex_destroy(&card_availability[i].card_mutex);
    }
    card_availability_count = 0;
}

/** @brief Initialize card availability tracking
 * @param unified_system The unified channel system
 * @return 0 on success, -1 on error
 */
static int init_card_availability_tracking(unified_channel_system_t *unified_system)
{
    if (!unified_system) {
        return -1;
    }
    
    card_availability_count = unified_system->num_cards;
    
    for (int i = 0; i < unified_system->num_cards; i++) {
        card_availability[i].card_id = unified_system->cards[i].card_id;
        card_availability[i].is_available = 1;
        card_availability[i].current_frequency = 0.0;
        card_availability[i].client_count = 0;
        card_availability[i].is_streaming = 0;
        card_availability[i].last_activity = 0;
        
        if (pthread_mutex_init(&card_availability[i].card_mutex, NULL) != 0) {
            log_message(log_module, MSG_ERROR, "Failed to initialize card mutex for card %d", i);
            return -1;
        }
    }
    
    log_message(log_module, MSG_INFO, "Card availability tracking initialized for %d cards", card_availability_count);
    return 0;
}


/** @brief Check for signal lock on a card/frequency combination using event-driven polling
 * @param card_id The card ID to check
 * @param frequency The frequency to check
 * @return 1 if locked, 0 if not locked, -1 on error
 */
static int check_for_lock(int card_id, double frequency)
{
    char frontend_path[256];
    snprintf(frontend_path, sizeof(frontend_path), "/dev/dvb/adapter%d/frontend0", card_id);
    
    int fd_frontend = open(frontend_path, O_RDWR | O_NONBLOCK);
    if (fd_frontend < 0) {
        log_message(log_module, MSG_DEBUG, "Cannot open frontend %s for card %d (errno: %d)", 
                    frontend_path, card_id, errno);
        return -1;
    }
    
    // Use event-driven polling to check for lock status
    struct pollfd pfd;
    pfd.fd = fd_frontend;
    pfd.events = POLLIN | POLLPRI;
    pfd.revents = 0;
    
    // Poll with a short timeout to check for immediate lock status
    int poll_ret = poll(&pfd, 1, 100); // 100ms timeout
    
    fe_status_t fe_status;
    int res = ioctl(fd_frontend, FE_READ_STATUS, &fe_status);
    close(fd_frontend);
    
    if (res < 0) {
        log_message(log_module, MSG_DEBUG, "Cannot read frontend status for card %d (errno: %d)", 
                    card_id, errno);
        return -1;
    }
    
    int has_lock = (fe_status & FE_HAS_LOCK) ? 1 : 0;
    log_message(log_module, MSG_DEBUG, "Card %d frequency %.0f Hz lock status: %s (poll_ret: %d)", 
                card_id, frequency, has_lock ? "LOCKED" : "NO LOCK", poll_ret);
    
    return has_lock;
}

/** @brief Get frequency channels for a specific card and frequency using event-driven polling
 * @param card_id The card ID
 * @param frequency The frequency to scan
 * @param channel_info Output structure to store channel information
 * @return Number of channels found, or -1 on error
 */
static int get_frequency_channels(int card_id, double frequency, frequency_channel_info_t *channel_info)
{
    if (!channel_info) {
        return -1;
    }
    
    // Initialize channel info
    channel_info->frequency = frequency;
    channel_info->card_id = card_id;
    channel_info->channel_count = 0;
    channel_info->is_active = 0;
    channel_info->client_count = 0;
    channel_info->last_accessed = time(NULL);
    
    // Open DVR device for this card
    char dvr_path[64];
    snprintf(dvr_path, sizeof(dvr_path), "/dev/dvb/adapter%d/dvr0", card_id);
    
    int fd_dvr = open(dvr_path, O_RDONLY | O_NONBLOCK);
    if (fd_dvr < 0) {
        log_message(log_module, MSG_DEBUG, "Cannot open DVR device %s for card %d (errno: %d)", 
                    dvr_path, card_id, errno);
        return -1;
    }
    
    // Use event-driven polling to wait for data
    struct pollfd pfd;
    pfd.fd = fd_dvr;
    pfd.events = POLLIN | POLLPRI;
    pfd.revents = 0;
    
    // Poll for data availability with timeout
    int poll_ret = poll(&pfd, 1, 1000); // 1 second timeout
    
    if (poll_ret < 0) {
        log_message(log_module, MSG_DEBUG, "Poll error on DVR device %s for card %d (errno: %d)", 
                    dvr_path, card_id, errno);
        close(fd_dvr);
        return -1;
    }
    
    if (poll_ret == 0) {
        log_message(log_module, MSG_DEBUG, "Timeout waiting for data on DVR device %s for card %d", 
                    dvr_path, card_id);
        close(fd_dvr);
        return 0;
    }
    
    // Read transport stream data to find channels
    uint8_t ts_buffer[188 * 100]; // Read 100 TS packets
    ssize_t bytes_read = read(fd_dvr, ts_buffer, sizeof(ts_buffer));
    
    if (bytes_read > 0) {
        // Parse TS packets to find channels
        int channel_count = parse_transport_stream_for_channels(ts_buffer, bytes_read, NULL, frequency);
        
        if (channel_count > 0) {
            channel_info->channel_count = channel_count;
            
            // Generate URIs for each channel
            for (int i = 0; i < channel_count && i < 128; i++) {
                snprintf(channel_info->channel_names[i], sizeof(channel_info->channel_names[i]), 
                        "Channel_%d_%d", card_id, i);
                channel_info->service_ids[i] = 1000 + i; // Placeholder service ID
                
                // Generate URI for this channel
                snprintf(channel_info->channel_uris[i], sizeof(channel_info->channel_uris[i]), 
                        "/unified/frequency/%.0f/channel/%d", frequency, i);
            }
            
            log_message(log_module, MSG_INFO, "Card %d found %d channels on frequency %.0f Hz", 
                        card_id, channel_count, frequency);
        }
    } else {
        log_message(log_module, MSG_DEBUG, "Card %d no data read from DVR on frequency %.0f Hz", 
                    card_id, frequency);
    }
    
    close(fd_dvr);
    return channel_info->channel_count;
}

/** @brief Main flow process: iterate through cards and frequencies
 * @param unified_system The unified channel system
 * @return 0 on success, -1 on error
 */
int unified_card_frequency_flow(unified_channel_system_t *unified_system)
{
    if (!unified_system || unified_system->num_cards == 0 || unified_system->num_frequencies == 0) {
        log_message(log_module, MSG_ERROR, "Invalid unified system or no cards/frequencies");
        return -1;
    }
    
    log_message(log_module, MSG_INFO, "Starting unified card-frequency flow process...");
    
    // Initialize card availability tracking
    if (init_card_availability_tracking(unified_system) != 0) {
        log_message(log_module, MSG_ERROR, "Failed to initialize card availability tracking");
        return -1;
    }
    
    // Reset discovered channels
    discovered_channel_count = 0;
    
    // Main flow: for each card, for each frequency
    for (int c = 0; c < unified_system->num_cards; c++) {
        int card_id = unified_system->cards[c].card_id;
        log_message(log_module, MSG_INFO, "Processing card %d...", card_id);
        
        for (int f = 0; f < unified_system->num_frequencies; f++) {
            double frequency = unified_system->frequencies[f];
            log_message(log_module, MSG_INFO, "Testing card %d on frequency %.0f Hz...", card_id, frequency);
            
            // Check for lock
            int lock_status = check_for_lock(card_id, frequency);
            if (lock_status < 0) {
                log_message(log_module, MSG_DEBUG, "Card %d cannot check lock on frequency %.0f Hz", card_id, frequency);
                continue;
            }
            
            if (lock_status == 0) {
                log_message(log_module, MSG_DEBUG, "Card %d no lock on frequency %.0f Hz", card_id, frequency);
                continue;
            }
            
            // Get frequency channels
            frequency_channel_info_t channel_info;
            int channel_count = get_frequency_channels(card_id, frequency, &channel_info);
            
            if (channel_count > 0) {
                // Store the channel information
                if (discovered_channel_count < 64) {
                    discovered_channels[discovered_channel_count] = channel_info;
                    discovered_channel_count++;
                    
                    log_message(log_module, MSG_INFO, "Stored %d channels for card %d frequency %.0f Hz", 
                                channel_count, card_id, frequency);
                } else {
                    log_message(log_module, MSG_WARN, "Maximum number of discovered channels reached");
                }
            }
        }
    }
    
    log_message(log_module, MSG_INFO, "Unified card-frequency flow completed. Found %d frequency/card combinations", 
                discovered_channel_count);
    
    return 0;
}

/** @brief Find the best available card for a given frequency
 * @param frequency The frequency to find a card for
 * @return Card ID if found, -1 if no card available
 */
int find_best_card_for_frequency(double frequency)
{
    int best_card = -1;
    int best_score = -1;
    
    for (int i = 0; i < card_availability_count; i++) {
        pthread_mutex_lock(&card_availability[i].card_mutex);
        
        // Check if this card is available or already tuned to this frequency
        if (card_availability[i].is_available || 
            (card_availability[i].current_frequency == frequency && card_availability[i].client_count > 0)) {
            
            // Calculate score based on availability and current usage
            int score = 0;
            if (card_availability[i].is_available) {
                score = 100; // Available cards get highest priority
            } else {
                score = 50 - card_availability[i].client_count; // Already tuned cards get lower priority
            }
            
            if (score > best_score) {
                best_score = score;
                best_card = card_availability[i].card_id;
            }
        }
        
        pthread_mutex_unlock(&card_availability[i].card_mutex);
    }
    
    log_message(log_module, MSG_DEBUG, "Best card for frequency %.0f Hz: %d (score: %d)", 
                frequency, best_card, best_score);
    
    return best_card;
}

/** @brief Assign a card to a frequency and mark it as in use
 * @param card_id The card ID to assign
 * @param frequency The frequency to assign to
 * @return 0 on success, -1 on error
 */
int assign_card_to_frequency_usage(int card_id, double frequency)
{
    for (int i = 0; i < card_availability_count; i++) {
        if (card_availability[i].card_id == card_id) {
            pthread_mutex_lock(&card_availability[i].card_mutex);
            
            card_availability[i].is_available = 0;
            card_availability[i].current_frequency = frequency;
            card_availability[i].client_count++;
            card_availability[i].last_activity = time(NULL);
            
            pthread_mutex_unlock(&card_availability[i].card_mutex);
            
            log_message(log_module, MSG_INFO, "Assigned card %d to frequency %.0f Hz (client count: %d)", 
                        card_id, frequency, card_availability[i].client_count);
            return 0;
        }
    }
    
    log_message(log_module, MSG_ERROR, "Card %d not found in availability tracking", card_id);
    return -1;
}

/** @brief Release a card from frequency usage
 * @param card_id The card ID to release
 * @param frequency The frequency to release from
 * @return 0 on success, -1 on error
 */
int release_card_from_frequency_usage(int card_id, double frequency)
{
    for (int i = 0; i < card_availability_count; i++) {
        if (card_availability[i].card_id == card_id) {
            pthread_mutex_lock(&card_availability[i].card_mutex);
            
            if (card_availability[i].current_frequency == frequency) {
                card_availability[i].client_count--;
                
                if (card_availability[i].client_count <= 0) {
                    // No more clients on this frequency, mark card as available
                    card_availability[i].is_available = 1;
                    card_availability[i].current_frequency = 0.0;
                    card_availability[i].client_count = 0;
                    
                    log_message(log_module, MSG_INFO, "Card %d released from frequency %.0f Hz and marked as available", 
                                card_id, frequency);
                } else {
                    log_message(log_module, MSG_INFO, "Card %d client count reduced to %d on frequency %.0f Hz", 
                                card_id, card_availability[i].client_count, frequency);
                }
            }
            
            pthread_mutex_unlock(&card_availability[i].card_mutex);
            return 0;
        }
    }
    
    log_message(log_module, MSG_ERROR, "Card %d not found in availability tracking", card_id);
    return -1;
}

/** @brief Show feasible cards for a given frequency
 * @param frequency The frequency to check
 */
static void show_feasible_cards_for_frequency(double frequency)
{
    // Use the global unified system if available
    extern unified_channel_system_t *global_unified_system;
    
    if (!global_unified_system) {
        log_message(log_module, MSG_INFO, "No unified system available to check feasible cards");
        return;
    }
    
    log_message(log_module, MSG_INFO, "Checking feasible cards for frequency %.0f Hz:", frequency);
    
    int feasible_count = 0;
    int available_count = 0;
    
    // First check if this frequency is in the global frequency list
    int frequency_in_list = 0;
    for (int j = 0; j < global_unified_system->num_frequencies; j++) {
        if (global_unified_system->frequencies[j] == frequency) {
            frequency_in_list = 1;
            break;
        }
    }
    
    if (!frequency_in_list) {
        log_message(log_module, MSG_WARN, "Frequency %.0f Hz is not in the global frequency list", frequency);
        return;
    }
    
    for (int i = 0; i < global_unified_system->num_cards; i++) {
        unified_card_t *card = &global_unified_system->cards[i];
        
        // All cards can handle any frequency in the global list (they're all DVB cards)
        // The real question is whether they're available and not locked
        int can_handle_freq = 1; // Assume all cards can handle any frequency
        
        if (can_handle_freq) {
            feasible_count++;
            
            // Check if card is available and get detailed status
            int is_available = 1;
            char status_str[512] = "available";
            char reason[256] = "";
            
            if (card->in_use) {
                is_available = 0;
                if (card->current_freq > 0) {
                    snprintf(reason, sizeof(reason), "in_use (tuned to %.0f Hz)", card->current_freq);
                } else {
                    snprintf(reason, sizeof(reason), "in_use (no frequency set)");
                }
            } else {
                // Check if card is locked by checking file descriptor availability
                char frontend_path[256];
                snprintf(frontend_path, sizeof(frontend_path), "/dev/dvb/adapter%d/frontend0", card->card_id);
                
                int test_fd = open(frontend_path, O_RDWR | O_NONBLOCK);
                if (test_fd < 0) {
                    is_available = 0;
                    snprintf(reason, sizeof(reason), "locked (frontend error: %s)", strerror(errno));
                } else {
                    close(test_fd);
                    snprintf(reason, sizeof(reason), "available");
                }
            }
            
            snprintf(status_str, sizeof(status_str), "%s", reason);
            
            if (is_available) {
                available_count++;
            }
            
            log_message(log_module, MSG_INFO, "  Card %d: %s", card->card_id, status_str);
        } else {
            // Card can't handle this frequency, but show it anyway for completeness
            log_message(log_module, MSG_DEBUG, "  Card %d: cannot handle frequency %.0f Hz (supports %d frequencies)", 
                       card->card_id, frequency, card->num_frequencies);
        }
    }
    
    if (feasible_count == 0) {
        log_message(log_module, MSG_WARN, "No cards can handle frequency %.0f Hz", frequency);
    } else if (available_count == 0) {
        log_message(log_module, MSG_WARN, "Found %d feasible cards for frequency %.0f Hz, but none are available (all in use or locked)", 
                    feasible_count, frequency);
    } else {
        log_message(log_module, MSG_INFO, "Found %d feasible cards (%d available) for frequency %.0f Hz", 
                    feasible_count, available_count, frequency);
    }
}

/** @brief Start data streaming thread for a card/frequency combination using existing infrastructure
 * @param card_id The card ID to start streaming for
 * @param frequency The frequency being streamed
 * @return 0 on success, -1 on error
 */
int start_card_data_streaming(int card_id, double frequency)
{
    log_message(log_module, MSG_INFO, "Starting data streaming for card %d on frequency %.0f Hz using existing infrastructure", 
                card_id, frequency);
    
    // Update card utilization tracking to mark as streaming
    pthread_mutex_lock(&utilization_global_mutex);
    
    // Find or create card utilization entry
    int card_idx = -1;
    for (int i = 0; i < 16; i++) {
        if (card_utilization[i].card_id == card_id) {
            card_idx = i;
            break;
        } else if (card_utilization[i].card_id == -1 && card_idx == -1) {
            card_idx = i; // Use first empty slot
        }
    }
    
    if (card_idx >= 0) {
        if (card_utilization[card_idx].card_id == -1) {
            // New entry
            card_utilization[card_idx].card_id = card_id;
            card_utilization[card_idx].current_frequency = frequency;
            card_utilization[card_idx].total_clients = 1; // One client requesting this
            card_utilization[card_idx].is_tuning = 0;
            card_utilization[card_idx].is_streaming = 1;
            strcpy(card_utilization[card_idx].usage_type, "unicast_client");
            card_utilization[card_idx].last_activity = time(NULL);
            card_utilization[card_idx].streaming_start_time = time(NULL);
            card_utilization_count++;
        } else {
            // Existing entry - update
            card_utilization[card_idx].current_frequency = frequency;
            card_utilization[card_idx].total_clients++;
            card_utilization[card_idx].is_streaming = 1;
            card_utilization[card_idx].last_activity = time(NULL);
            if (card_utilization[card_idx].streaming_start_time == 0) {
                card_utilization[card_idx].streaming_start_time = time(NULL);
            }
        }
    }
    
    pthread_mutex_unlock(&utilization_global_mutex);
    
    if (card_idx < 0) {
        log_message(log_module, MSG_ERROR, "No available card utilization slot for card %d", card_id);
        return -1;
    }
    
    // Check if streaming is already active for this card/frequency in card availability
    for (int i = 0; i < card_availability_count; i++) {
        if (card_availability[i].card_id == card_id) {
            pthread_mutex_lock(&card_availability[i].card_mutex);
            
            if (card_availability[i].is_streaming) {
                pthread_mutex_unlock(&card_availability[i].card_mutex);
                log_message(log_module, MSG_INFO, "Card %d already streaming on frequency %.0f Hz", 
                           card_id, frequency);
                return 0; // Already streaming
            }
            
            // Mark as streaming
            card_availability[i].is_streaming = 1;
            card_availability[i].current_frequency = frequency;
            card_availability[i].client_count = 1;
            card_availability[i].last_activity = time(NULL);
            pthread_mutex_unlock(&card_availability[i].card_mutex);
            break;
        }
    }
    
    // Use existing card reading infrastructure
    // We need to set up the card thread parameters similar to the main system
    // Create a local card buffer for this specific card
    card_buffer_t card_buffer;
    memset(&card_buffer, 0, sizeof(card_buffer_t));
    card_buffer.dvr_buffer_size = 100; // Default buffer size
    card_buffer.max_thread_buffer_size = 100;
    card_buffer.write_buffer_size = card_buffer.max_thread_buffer_size * TS_PACKET_SIZE;
    card_buffer.buffer1 = malloc(sizeof(unsigned char) * card_buffer.write_buffer_size);
    card_buffer.buffer2 = malloc(sizeof(unsigned char) * card_buffer.write_buffer_size);
    card_buffer.actual_read_buffer = 1;
    card_buffer.reading_buffer = card_buffer.buffer1;
    card_buffer.writing_buffer = card_buffer.buffer2;
    
    // Set up file descriptors for this card
    char dvr_path[256];
    snprintf(dvr_path, sizeof(dvr_path), "/dev/dvb/adapter%d/dvr0", card_id);
    
    // Open DVR device
    int fd_dvr = open(dvr_path, O_RDONLY | O_NONBLOCK);
    if (fd_dvr < 0) {
        log_message(log_module, MSG_ERROR, "Cannot open DVR device %s for card %d (errno: %d)", 
                    dvr_path, card_id, errno);
        return -1;
    }
    
    // Set up polling file descriptors for this card
    struct pollfd pfds[1];
    pfds[0].fd = fd_dvr;
    pfds[0].events = POLLIN;
    pfds[0].revents = 0;
    
    // Create a local fds structure for this card
    fds_t card_fds;
    card_fds.fd_source = 0; // DVB source
    card_fds.fd_dvr = fd_dvr;
    card_fds.pfds = pfds;
    card_fds.pfdsnum = 1;
    
    // Set up card thread parameters
    card_thread_parameters_t card_thread_params;
    card_thread_params.thread_running = 1;
    card_thread_params.fds = &card_fds;
    card_thread_params.card_buffer = &card_buffer;
    card_thread_params.threadshutdown = 0;
    
    // Initialize mutex and condition for this card
    if (pthread_mutex_init(&card_thread_params.carddatamutex, NULL) != 0) {
        log_message(log_module, MSG_ERROR, "Failed to initialize card data mutex for card %d", card_id);
        close(fd_dvr);
        return -1;
    }
    
    if (pthread_cond_init(&card_thread_params.threadcond, NULL) != 0) {
        log_message(log_module, MSG_ERROR, "Failed to initialize card thread condition for card %d", card_id);
        pthread_mutex_destroy(&card_thread_params.carddatamutex);
        close(fd_dvr);
        return -1;
    }
    
    // Create the card reading thread using existing infrastructure
    pthread_t card_thread;
    if (pthread_create(&card_thread, NULL, read_card_thread_func, &card_thread_params) != 0) {
        log_message(log_module, MSG_ERROR, "Failed to create card reading thread for card %d", card_id);
        pthread_mutex_destroy(&card_thread_params.carddatamutex);
        pthread_cond_destroy(&card_thread_params.threadcond);
        close(fd_dvr);
        return -1;
    }
    
    // Register the thread for proper cleanup
    register_thread(card_thread, "Card-Reading", NULL, NULL);
    
    log_message(log_module, MSG_INFO, "Card reading thread started for card %d on frequency %.0f Hz using existing infrastructure", 
                card_id, frequency);
    
    return 0;
}

/** @brief Stop data streaming for a card/frequency combination
 * @param card_id The card ID to stop streaming for
 * @param frequency The frequency being streamed
 * @return 0 on success, -1 on error
 */
int stop_card_data_streaming(int card_id, double frequency)
{
    log_message(log_module, MSG_INFO, "Stopping data streaming for card %d on frequency %.0f Hz", 
                card_id, frequency);
    
    // Update card utilization tracking to decrement client count
    pthread_mutex_lock(&utilization_global_mutex);
    
    for (int i = 0; i < 16; i++) {
        if (card_utilization[i].card_id == card_id) {
            pthread_mutex_lock(&card_utilization[i].utilization_mutex);
            
            if (card_utilization[i].total_clients > 0) {
                card_utilization[i].total_clients--;
            }
            
            // If no more clients, mark as not streaming
            if (card_utilization[i].total_clients <= 0) {
                card_utilization[i].is_streaming = 0;
                card_utilization[i].current_frequency = 0.0;
                strcpy(card_utilization[i].usage_type, "idle");
            }
            
            card_utilization[i].last_activity = time(NULL);
            pthread_mutex_unlock(&card_utilization[i].utilization_mutex);
            break;
        }
    }
    
    pthread_mutex_unlock(&utilization_global_mutex);
    
    // Update card availability tracking
    for (int i = 0; i < card_availability_count; i++) {
        if (card_availability[i].card_id == card_id) {
            pthread_mutex_lock(&card_availability[i].card_mutex);
            
            if (card_availability[i].client_count > 0) {
                card_availability[i].client_count--;
            }
            
            // If no more clients, mark as available
            if (card_availability[i].client_count <= 0) {
                card_availability[i].is_streaming = 0;
                card_availability[i].is_available = 1;
                card_availability[i].current_frequency = 0.0;
            }
            
            card_availability[i].last_activity = time(NULL);
            pthread_mutex_unlock(&card_availability[i].card_mutex);
            break;
        }
    }
    
    log_message(log_module, MSG_INFO, "Data streaming stopped for card %d on frequency %.0f Hz", 
                card_id, frequency);
    
    return 0;
}

/** @brief Bootstrap a card for immediate use when a client requests data
 * @param card_id The card ID to bootstrap
 * @param frequency The frequency to tune to
 * @return 0 on success, -1 on error
 */
int bootstrap_card_for_frequency(int card_id, double frequency)
{
    log_message(log_module, MSG_INFO, "Bootstrapping card %d for frequency %.0f Hz...", card_id, frequency);
    
    // Check if card is already tuned to this frequency
    for (int i = 0; i < card_availability_count; i++) {
        if (card_availability[i].card_id == card_id) {
            pthread_mutex_lock(&card_availability[i].card_mutex);
            
            if (card_availability[i].current_frequency == frequency) {
                // Already tuned to this frequency, just increment client count
                card_availability[i].client_count++;
                card_availability[i].last_activity = time(NULL);
                pthread_mutex_unlock(&card_availability[i].card_mutex);
                
                log_message(log_module, MSG_INFO, "Card %d already tuned to frequency %.0f Hz, client count: %d", 
                            card_id, frequency, card_availability[i].client_count);
                return 0;
            }
            
            pthread_mutex_unlock(&card_availability[i].card_mutex);
            break;
        }
    }
    
    // Need to tune the card to this frequency
    char frontend_path[256];
    snprintf(frontend_path, sizeof(frontend_path), "/dev/dvb/adapter%d/frontend0", card_id);
    
    int fd_frontend = open(frontend_path, O_RDWR | O_NONBLOCK);
    if (fd_frontend < 0) {
        log_message(log_module, MSG_ERROR, "Cannot open frontend %s for card %d (errno: %d)", 
                    frontend_path, card_id, errno);
        
        // Show feasible cards for this frequency
        show_feasible_cards_for_frequency(frequency);
        return -1;
    }
    
    // Set up tuning parameters
    tune_p_t tune_params;
    init_tune_v(&tune_params);
    tune_params.freq = frequency;
    tune_params.card = card_id;
    tune_params.tuner = 0;
    
    // Tune to the frequency (includes 15-second timeout for lock)
    int tune_result = tune_it(fd_frontend, &tune_params);
    
    // tune_it() already handled the 15-second timeout and lock checking
    // Just check the final status to determine if we should continue
    if (tune_result == 0) {
        fe_status_t fe_status;
        int status_ret = ioctl(fd_frontend, FE_READ_STATUS, &fe_status);
        if (status_ret >= 0 && !(fe_status & FE_HAS_LOCK)) {
            log_message(log_module, MSG_WARN, "Card %d tuned to frequency %.0f Hz but no lock achieved within 15 seconds", 
                        card_id, frequency);
        }
    }
    
    close(fd_frontend);
    
    if (tune_result != 0) {
        log_message(log_module, MSG_ERROR, "Failed to tune card %d to frequency %.0f Hz (tune_result=%d)", 
                    card_id, frequency, tune_result);
        
        // Show feasible cards for this frequency
        show_feasible_cards_for_frequency(frequency);
        return -1;
    }
    
    // Update card availability
    for (int i = 0; i < card_availability_count; i++) {
        if (card_availability[i].card_id == card_id) {
            pthread_mutex_lock(&card_availability[i].card_mutex);
            
            card_availability[i].is_available = 0;
            card_availability[i].current_frequency = frequency;
            card_availability[i].client_count = 1;
            card_availability[i].last_activity = time(NULL);
            
            pthread_mutex_unlock(&card_availability[i].card_mutex);
            break;
        }
    }
    
    log_message(log_module, MSG_INFO, "Successfully bootstrapped card %d for frequency %.0f Hz", 
                card_id, frequency);
    
    // Start data streaming using existing card reading infrastructure
    if (start_card_data_streaming(card_id, frequency) != 0) {
        log_message(log_module, MSG_ERROR, "Failed to start data streaming for card %d", card_id);
        return -1;
    }
    
    return 0;
}

/** @brief Get channel URI for a specific frequency and channel
 * @param frequency The frequency
 * @param channel_index The channel index within that frequency
 * @param uri_buffer Buffer to store the URI
 * @param buffer_size Size of the URI buffer
 * @return 0 on success, -1 if not found
 */
int get_channel_uri_for_frequency(double frequency, int channel_index, char *uri_buffer, size_t buffer_size)
{
    if (!uri_buffer || buffer_size == 0) {
        return -1;
    }
    
    for (int i = 0; i < discovered_channel_count; i++) {
        if (discovered_channels[i].frequency == frequency && 
            channel_index < discovered_channels[i].channel_count) {
            
            strncpy(uri_buffer, discovered_channels[i].channel_uris[channel_index], buffer_size - 1);
            uri_buffer[buffer_size - 1] = '\0';
            
            return 0;
        }
    }
    
    return -1; // Not found
}

/** @brief Get all available channels for HTTP endpoint
 * @param channels_output Array to store channel information
 * @param max_channels Maximum number of channels to return
 * @return Number of channels returned
 */
int get_unified_channels_for_http(frequency_channel_info_t *channels_output, int max_channels)
{
    if (!channels_output || max_channels <= 0) {
        return 0;
    }
    
    int returned_channels = 0;
    for (int i = 0; i < discovered_channel_count && returned_channels < max_channels; i++) {
        channels_output[returned_channels] = discovered_channels[i];
        returned_channels++;
    }
    
    return returned_channels;
}

/** @brief Test if a card can receive signals on a specific frequency
 * @param card The card to test
 * @param frequency The frequency to test
 * @return 1 if signal received, 0 if not
 */
static int test_card_frequency(unified_card_t *card, double frequency)
{
    if (!card || !card->tune_params) {
        return 0;
    }
    
    // Check if this card/frequency combination is already known to be unusable
    if (is_card_frequency_unusable(card->card_id, frequency)) {
        log_message(log_module, MSG_DEBUG, "Card %d already marked as unusable for frequency %.0f Hz", 
                    card->card_id, frequency);
        return 0;
    }
    
    // Use the card_id as the adapter number (each card = one adapter)
    char frontend_path[256];
    snprintf(frontend_path, sizeof(frontend_path), "/dev/dvb/adapter%d/frontend0", 
             card->card_id);
    
    // Add a small delay to prevent simultaneous device access conflicts
    // Use poll() instead of usleep() for main thread responsiveness
    extern fds_t *global_main_fds;
    extern unicast_parameters_t *global_unicast_params;
    
    if (global_main_fds && global_unicast_params) {
        main_thread_sleep_with_poll(global_main_fds, global_unicast_params, 100); // 100ms delay
    } else {
        // Fallback to interruptible sleep if main thread context not available
        if (event_sleep_interruptible(100000) < 0) { // 100ms delay
            return -1; // Interrupted
        }
    }
    
    int fd_frontend = open(frontend_path, O_RDWR | O_NONBLOCK);
    if (fd_frontend < 0) {
        log_message(log_module, MSG_DEBUG, "Cannot open frontend %s for card %d (errno: %d)", 
                    frontend_path, card->card_id, errno);
        mark_card_frequency_unusable(card->card_id, frequency);
        return 0;
    }
    
    // Check if the frontend device is accessible and get its info
    struct dvb_frontend_info fe_info;
    int res = ioctl(fd_frontend, FE_GET_INFO, &fe_info);
    if (res < 0) {
        close(fd_frontend);
        log_message(log_module, MSG_DEBUG, "Cannot get frontend info for card %d (errno: %d)", 
                    card->card_id, errno);
        mark_card_frequency_unusable(card->card_id, frequency);
        return 0;
    }
    
    log_message(log_module, MSG_INFO, "card-%d scanning frequency %.0f Hz (frontend: %s)", 
                card->card_id, frequency, fe_info.name);
    
    // Set up tuning parameters for this frequency
    card->tune_params->freq = frequency;
    card->tune_params->card = card->card_id;
    card->tune_params->tuner = 0; // Always use tuner 0 for each card
    
    // Actually tune to the frequency and check for signal lock
    log_message(log_module, MSG_DEBUG, "Card %d attempting to tune to frequency %.0f Hz...", 
                card->card_id, frequency);
    
    // Try tuning to the frequency (includes 15-second timeout for lock)
    int tune_result = tune_it(fd_frontend, card->tune_params);
    
    if (tune_result != 0) {
        close(fd_frontend);
        log_message(log_module, MSG_DEBUG, "Card %d cannot tune to frequency %.0f Hz (tune_result=%d)", 
                    card->card_id, frequency, tune_result);
        mark_card_frequency_unusable(card->card_id, frequency);
        return 0;
    }
    
    // tune_it() already handled the 15-second timeout and lock checking
    // Just check the final status to determine if we should continue
    fe_status_t fe_status;
    res = ioctl(fd_frontend, FE_READ_STATUS, &fe_status);
    if (res < 0) {
        log_message(log_module, MSG_DEBUG, "Cannot read final frontend status for card %d (errno: %d)", 
                    card->card_id, errno);
        close(fd_frontend);
        mark_card_frequency_unusable(card->card_id, frequency);
        return 0;
    }
    
    // Check if we achieved lock within the 15-second timeout
    if (!(fe_status & FE_HAS_LOCK)) {
        close(fd_frontend);
        log_message(log_module, MSG_INFO, "Card %d timeout after 15 seconds on frequency %.0f Hz - marking as unusable", 
                    card->card_id, frequency);
        mark_card_frequency_unusable(card->card_id, frequency);
        return 0;
    }
    
    // Get signal strength and quality
    uint16_t signal_strength = 0;
    uint16_t snr = 0;
    ioctl(fd_frontend, FE_READ_SIGNAL_STRENGTH, &signal_strength);
    ioctl(fd_frontend, FE_READ_SNR, &snr);
    
    log_message(log_module, MSG_INFO, "Card %d has signal lock on frequency %.0f Hz (strength: %d, SNR: %d)", 
                card->card_id, frequency, signal_strength, snr);
    
    // Mark this card/frequency combination as usable
    mark_card_frequency_usable(card->card_id, frequency);
    
    // Now build the channel list by reading the transport stream
    int channel_count = build_channel_list_for_frequency(card, frequency, fd_frontend);
    
    close(fd_frontend);
    
    if (channel_count > 0) {
        log_message(log_module, MSG_INFO, "Card %d found %d channels on frequency %.0f Hz", 
                    card->card_id, channel_count, frequency);
        return 1; // Successfully tuned and found channels
    } else {
        log_message(log_module, MSG_DEBUG, "Card %d no channels found on frequency %.0f Hz", 
                    card->card_id, frequency);
        return 0;
    }
}

// Background thread function to scan remaining frequencies
void *background_frequency_scanner(void *arg)
{
    unified_channel_system_t *unified_system = (unified_channel_system_t *)arg;
    
    if (!unified_system || unified_system->num_cards == 0) {
        log_message(log_module, MSG_ERROR, "No unified system or cards available for background scanning");
        return NULL;
    }
    
    log_message(log_module, MSG_INFO, "Background scanner: Starting to test remaining frequencies...");
    
    // Wait for background scan timing event instead of usleep
    if (global_unified_timing_tracker) {
        timing_event_t event;
        int wait_result = wait_for_timing_event(global_unified_timing_tracker, 
                                              TIMING_EVENT_BACKGROUND_SCAN, 
                                              TIMING_BACKGROUND_SCANNER_DELAY_SEC * 1000, &event);
        if (wait_result < 0 && wait_result != -1) { // -1 is timeout, which is OK
            log_message(log_module, MSG_DEBUG, "Background scan timing event wait failed: %d", wait_result);
        }
    } else {
        // Fallback to interruptible sleep if timing tracker not available
        // Use time_t units (seconds) for cleaner code
        time_t delay_sec = TIMING_BACKGROUND_SCANNER_DELAY_SEC;
        time_t start_time = time(NULL);
        
        while (time(NULL) - start_time < delay_sec) {
            if (event_sleep_interruptible(1000000) < 0) { // 1 second sleep
                return NULL; // Interrupted
            }
        }
    }
    
    // Test each remaining frequency on each available card
    for (int freq_idx = 1; freq_idx < unified_system->num_frequencies; freq_idx++) {
        // Check for interrupt signal before each frequency
        if (get_interrupted()) {
            log_message(log_module, MSG_INFO, "Background scanner: Shutting down due to interrupt signal");
            return NULL;
        }
        
        double frequency = unified_system->frequencies[freq_idx];
        log_message(log_module, MSG_INFO, "Background scanner: Testing frequency %.1f MHz on all cards...", frequency/1000000.0);
        
        // Add delay between frequency tests to match initial scan timing (6 seconds)
        if (global_unified_timing_tracker) {
            timing_event_t event;
            int wait_result = wait_for_timing_event(global_unified_timing_tracker, 
                                                  TIMING_EVENT_FREQUENCY_TEST, 
                                                  TIMING_FREQUENCY_TEST_DELAY_MS, &event);
            if (wait_result < 0 && wait_result != -1) { // -1 is timeout, which is OK
                log_message(log_module, MSG_DEBUG, "Background scanner frequency test timing event wait failed: %d", wait_result);
            }
        } else {
            // Fallback to interruptible sleep if timing tracker not available
            if (event_sleep_interruptible(MS_TO_US(TIMING_FREQUENCY_TEST_DELAY_MS)) < 0) {
                return NULL; // Interrupted
            }
        }
        
        for (int card_idx = 0; card_idx < unified_system->num_cards; card_idx++) {
            // Check for interrupt signal before each card
            if (get_interrupted()) {
                log_message(log_module, MSG_INFO, "Background scanner: Shutting down due to interrupt signal");
                return NULL;
            }
            
            // Bounds check to prevent memory corruption
            if (card_idx >= unified_system->num_cards || card_idx < 0) {
                log_message(log_module, MSG_ERROR, "Background scanner: Invalid card index %d (max: %d)", 
                           card_idx, unified_system->num_cards - 1);
                break;
            }
            
            int card_id = unified_system->cards[card_idx].card_id;
            
            // Add delay between testing different cards on the same frequency
            if (card_idx > 0) {
                // Use event-based timing for card test delays
                if (global_unified_timing_tracker) {
                    timing_event_t event;
                    int wait_result = wait_for_timing_event(global_unified_timing_tracker, 
                                                          TIMING_EVENT_POLL_INTERVAL, 
                                                          TIMING_CARD_TEST_DELAY_MS, &event);
                    if (wait_result < 0 && wait_result != -1) { // -1 is timeout, which is OK
                        log_message(log_module, MSG_DEBUG, "Card test delay timing event wait failed: %d", wait_result);
                    }
                } else {
                    // Fallback to interruptible sleep if timing tracker not available
                    if (event_sleep_interruptible(MS_TO_US(TIMING_CARD_TEST_DELAY_MS)) < 0) { // Delay between card tests
                        return NULL; // Interrupted
                    }
                }
            }
            
            // Skip cards already known to be unusable for this frequency
            if (is_card_frequency_unusable(card_id, frequency)) {
                log_message(log_module, MSG_DEBUG, "Background scanner: Skipping card-%d (already marked unusable for frequency %.1f MHz)", 
                            card_id, frequency/1000000.0);
                continue;
            }
            
            // Check if card is currently in use by any system
            if (is_card_in_use(card_id)) {
                log_message(log_module, MSG_DEBUG, "Background scanner: Skipping card-%d (currently in use)", 
                            card_id);
                continue;
            }
            
            // Only log testing message if card is actually available
            log_message(log_module, MSG_INFO, "Background scanner: Testing card-%d for frequency %.1f MHz", 
                        card_id, frequency/1000000.0);
            
            // Test if this card can tune to this frequency (with 6-second timeout)
            if (test_card_frequency(&unified_system->cards[card_idx], frequency) > 0) {
                log_message(log_module, MSG_INFO, "Background scanner: card-%d can access frequency %.1f MHz - found channels", 
                            card_id, frequency/1000000.0);
                
                // Mark this frequency scan as completed for this card
                mark_frequency_scan_completed(card_id, frequency);
                
                // Record this frequency/card combination
                if (discovered_channel_count < 64) {
                    discovered_channels[discovered_channel_count].frequency = frequency;
                    discovered_channels[discovered_channel_count].card_id = card_id;
                    discovered_channels[discovered_channel_count].channel_count = 1; // Placeholder
                    strcpy(discovered_channels[discovered_channel_count].channel_names[0], "Background Discovered Channel");
                    discovered_channels[discovered_channel_count].service_ids[0] = 1;
                    discovered_channel_count++;
                }
                
                // For now, just find the first working card per frequency
                break;
            } else {
                log_message(log_module, MSG_DEBUG, "Background scanner: card-%d cannot access frequency %.1f MHz (marked as unusable)", 
                            card_id, frequency/1000000.0);
            }
        }
    }
    
    log_message(log_module, MSG_INFO, "Background scanner: Completed testing - found %d working frequency/card combinations", 
                discovered_channel_count);
    
    // Check for cards that can be released (completed scanning with no clients)
    check_and_release_idle_cards();
    
    return NULL;
}

int scan_remaining_frequencies(unified_channel_system_t *unified_system, fds_t *fds, 
                               tune_p_t *tune_p, int first_frequency_index)
{
    (void)fds; // Suppress unused parameter warning
    (void)tune_p; // Suppress unused parameter warning
    
    if (!unified_system || unified_system->num_cards == 0) {
        log_message(log_module, MSG_ERROR, "No unified system or cards available");
        return -1;
    }
    
    log_message(log_module, MSG_INFO, "Scanning remaining frequencies on available cards...");
    
    // Initialize discovered channels array
    discovered_channel_count = 0;
    
    // Initialize card utilization tracking
    init_card_utilization_tracking();
    
    // For each remaining frequency (skip the first one we already tuned)
    for (int freq_idx = first_frequency_index + 1; freq_idx < unified_system->num_frequencies; freq_idx++) {
        double frequency = unified_system->frequencies[freq_idx];
        log_message(log_module, MSG_INFO, "Scanning frequency %.0f Hz on available cards...", frequency);
        
        // Add delay between frequency tests to match initial scan timing (6 seconds)
        if (global_unified_timing_tracker) {
            timing_event_t event;
            int wait_result = wait_for_timing_event(global_unified_timing_tracker, 
                                                  TIMING_EVENT_FREQUENCY_TEST, 
                                                  TIMING_FREQUENCY_TEST_DELAY_MS, &event);
            if (wait_result < 0 && wait_result != -1) { // -1 is timeout, which is OK
                log_message(log_module, MSG_DEBUG, "Traditional scanner frequency test timing event wait failed: %d", wait_result);
            }
        } else {
            // Fallback to interruptible sleep if timing tracker not available
            if (event_sleep_interruptible(MS_TO_US(TIMING_FREQUENCY_TEST_DELAY_MS)) < 0) {
                return -1; // Interrupted
            }
        }
        
        // Try each available card for this frequency
        for (int card_idx = 0; card_idx < unified_system->num_cards; card_idx++) {
            int card_id = unified_system->cards[card_idx].card_id;
            
            // Add delay between testing different cards on the same frequency
            if (card_idx > 0) {
                // Use event-based timing for card test delays
                if (global_unified_timing_tracker) {
                    timing_event_t event;
                    int wait_result = wait_for_timing_event(global_unified_timing_tracker, 
                                                          TIMING_EVENT_POLL_INTERVAL, 
                                                          TIMING_CARD_TEST_DELAY_MS, &event);
                    if (wait_result < 0 && wait_result != -1) { // -1 is timeout, which is OK
                        log_message(log_module, MSG_DEBUG, "Card test delay timing event wait failed: %d", wait_result);
                    }
                } else {
                    // Fallback to interruptible sleep if timing tracker not available
                    if (event_sleep_interruptible(MS_TO_US(TIMING_CARD_TEST_DELAY_MS)) < 0) { // Delay between card tests
                        return -1; // Interrupted
                    }
                }
            }
            
            log_message(log_module, MSG_INFO, "Testing card-%d for frequency %.1f MHz", 
                        card_id, frequency/1000000.0);
            
            // Test if this card can tune to this frequency
            if (test_card_frequency(&unified_system->cards[card_idx], frequency) > 0) {
                log_message(log_module, MSG_INFO, "card-%d can access frequency %.1f MHz - found channels", 
                            card_id, frequency/1000000.0);
                
                // Record this frequency/card combination
                if (discovered_channel_count < 64) {
                    discovered_channels[discovered_channel_count].frequency = frequency;
                    discovered_channels[discovered_channel_count].card_id = card_id;
                    discovered_channels[discovered_channel_count].channel_count = 1; // Placeholder
                    strcpy(discovered_channels[discovered_channel_count].channel_names[0], "Discovered Channel");
                    discovered_channels[discovered_channel_count].service_ids[0] = 1;
                    discovered_channel_count++;
                }
                
                // For now, just find the first working card per frequency
                // In a full implementation, we'd test all cards and pick the best one
                break;
            } else {
                log_message(log_module, MSG_DEBUG, "card-%d cannot access frequency %.1f MHz", 
                            card_id, frequency/1000000.0);
            }
        }
    }
    
    log_message(log_module, MSG_INFO, "Frequency scanning complete - found %d working frequency/card combinations", 
                discovered_channel_count);
    
    return 0;
}

int start_unified_channel_serving(unified_channel_system_t *unified_system, fds_t *fds, 
                                  tune_p_t *tune_p, mumu_chan_p_t *chan_p, auto_p_t *auto_p,
                                  unicast_parameters_t *unic_p, multi_p_t *multi_p, 
                                  sap_p_t *sap_p, stats_infos_t *stats_infos, int server_id)
{
    (void)fds; // Suppress unused parameter warning
    (void)tune_p; // Suppress unused parameter warning
    (void)chan_p; // Suppress unused parameter warning
    (void)auto_p; // Suppress unused parameter warning
    (void)unic_p; // Suppress unused parameter warning
    (void)multi_p; // Suppress unused parameter warning
    (void)sap_p; // Suppress unused parameter warning
    (void)stats_infos; // Suppress unused parameter warning
    (void)server_id; // Suppress unused parameter warning
    
    if (!unified_system || unified_system->num_cards == 0) {
        log_message(log_module, MSG_ERROR, "No unified system or cards available");
        return -1;
    }
    
    log_message(log_module, MSG_INFO, "Starting unified channel serving...");
    
    // Run the card-frequency flow process
    if (unified_card_frequency_flow(unified_system) != 0) {
        log_message(log_module, MSG_ERROR, "Failed to run unified card-frequency flow");
        return -1;
    }
    
    log_message(log_module, MSG_INFO, "Unified channel serving started with %d discovered frequency/card combinations", 
                discovered_channel_count);
    log_message(log_module, MSG_INFO, "Found %d cards with %d total frequencies", 
                unified_system->num_cards, unified_system->num_frequencies);
    
    // The system is now ready to serve channels via HTTP endpoints
    // The discovered channels are stored and can be accessed through:
    // - get_unified_channels_for_http() for channel listing
    // - find_best_card_for_frequency() for card selection
    // - bootstrap_card_for_frequency() for immediate tuning
    
    return 0;
}