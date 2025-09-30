/**
 * @file unified_storage_adapter.c
 * @brief Adapter functions to maintain backward compatibility with new storage system
 */

#include "unified_channel_storage_v2.h"
#include "mumudvb.h"
#include "log.h"
#include <string.h>
#include <stdlib.h>

static char *log_module = "StorageAdapter: ";

// Global reference to the new storage system
static unified_channel_storage_v2_t *global_storage_v2 = NULL;

/** Initialize the adapter with the new storage system */
int init_storage_adapter(unified_channel_storage_v2_t *storage_v2) {
    if (!storage_v2) {
        log_message(log_module, MSG_ERROR, "Invalid storage v2 parameter");
        return -1;
    }
    
    global_storage_v2 = storage_v2;
    log_message(log_module, MSG_INFO, "Storage adapter initialized with v2 storage");
    return 0;
}

/** Adapter function: Add channel to storage (compatible with old API) */
int add_channel_to_storage_adapter(const mumudvb_channel_t *base_channel, 
                                  double frequency, int card_id, int discovered_via_parallel) {
    if (!global_storage_v2 || !base_channel) {
        log_message(log_module, MSG_ERROR, "Storage v2 not initialized or invalid channel");
        return -1;
    }
    
    // Ensure card exists in storage
    if (add_card_to_storage_v2(global_storage_v2, card_id) < 0) {
        log_message(log_module, MSG_ERROR, "Failed to add card %d to storage", card_id);
        return -1;
    }
    
    // Add frequency to card (with deduplication)
    if (add_frequency_to_card_v2(global_storage_v2, card_id, frequency) < 0) {
        log_message(log_module, MSG_ERROR, "Failed to add frequency %.0f Hz to card %d", frequency, card_id);
        return -1;
    }
    
    // Add channel to frequency (with deduplication)
    if (add_channel_to_frequency_v2(global_storage_v2, frequency, base_channel, discovered_via_parallel) < 0) {
        log_message(log_module, MSG_ERROR, "Failed to add channel %s to frequency %.0f Hz", 
                   base_channel->name, frequency);
        return -1;
    }
    
    return 0;
}

/** Adapter function: Get channels for frequency (compatible with old API) */
int get_channels_for_frequency_adapter(double frequency, enhanced_channel_t **channels, int *num_channels) {
    if (!global_storage_v2 || !channels || !num_channels) {
        return -1;
    }
    
    frequency_channel_t *freq_channels = NULL;
    int freq_num_channels = 0;
    
    if (get_channels_for_frequency_v2(global_storage_v2, frequency, &freq_channels, &freq_num_channels) < 0) {
        *channels = NULL;
        *num_channels = 0;
        return 0;
    }
    
    // Convert frequency_channel_t to enhanced_channel_t
    enhanced_channel_t *enhanced_channels = malloc(sizeof(enhanced_channel_t) * freq_num_channels);
    if (!enhanced_channels) {
        log_message(log_module, MSG_ERROR, "Failed to allocate memory for enhanced channels");
        return -1;
    }
    
    for (int i = 0; i < freq_num_channels; i++) {
        memcpy(&enhanced_channels[i].base_channel, freq_channels[i].base_channel, sizeof(mumudvb_channel_t));
        enhanced_channels[i].frequency = frequency;
        enhanced_channels[i].card_id = -1; // Not available in new structure
        enhanced_channels[i].is_active = freq_channels[i].is_active;
        enhanced_channels[i].client_count = freq_channels[i].client_count;
        enhanced_channels[i].last_accessed = freq_channels[i].last_accessed;
        strncpy(enhanced_channels[i].channel_uri, freq_channels[i].channel_uri, sizeof(enhanced_channels[i].channel_uri));
        enhanced_channels[i].discovered_via_parallel = freq_channels[i].discovered_via_parallel;
    }
    
    *channels = enhanced_channels;
    *num_channels = freq_num_channels;
    
    return 0;
}

/** Adapter function: Get channels for card (compatible with old API) */
int get_channels_for_card_adapter(int card_id, enhanced_channel_t **channels, int *num_channels) {
    if (!global_storage_v2 || !channels || !num_channels) {
        return -1;
    }
    
    frequency_channel_t *card_channels = NULL;
    int card_num_channels = 0;
    
    if (get_all_channels_for_card_v2(global_storage_v2, card_id, &card_channels, &card_num_channels) < 0) {
        *channels = NULL;
        *num_channels = 0;
        return 0;
    }
    
    if (card_num_channels == 0) {
        *channels = NULL;
        *num_channels = 0;
        return 0;
    }
    
    // Convert frequency_channel_t to enhanced_channel_t
    enhanced_channel_t *enhanced_channels = malloc(sizeof(enhanced_channel_t) * card_num_channels);
    if (!enhanced_channels) {
        log_message(log_module, MSG_ERROR, "Failed to allocate memory for enhanced channels");
        return -1;
    }
    
    for (int i = 0; i < card_num_channels; i++) {
        memcpy(&enhanced_channels[i].base_channel, card_channels[i].base_channel, sizeof(mumudvb_channel_t));
        enhanced_channels[i].frequency = 0.0; // Not available in card-specific lookup
        enhanced_channels[i].card_id = card_id;
        enhanced_channels[i].is_active = card_channels[i].is_active;
        enhanced_channels[i].client_count = card_channels[i].client_count;
        enhanced_channels[i].last_accessed = card_channels[i].last_accessed;
        strncpy(enhanced_channels[i].channel_uri, card_channels[i].channel_uri, sizeof(enhanced_channels[i].channel_uri));
        enhanced_channels[i].discovered_via_parallel = card_channels[i].discovered_via_parallel;
    }
    
    *channels = enhanced_channels;
    *num_channels = card_num_channels;
    
    return 0;
}

/** Adapter function: Get all channels (compatible with old API) */
int get_all_channels_adapter(enhanced_channel_t **channels, int *num_channels) {
    if (!global_storage_v2 || !channels || !num_channels) {
        return -1;
    }
    
    int total_channels = 0;
    int total_frequencies = 0;
    int total_cards = 0;
    
    get_storage_stats_v2(global_storage_v2, &total_channels, &total_frequencies, &total_cards);
    
    if (total_channels == 0) {
        *channels = NULL;
        *num_channels = 0;
        return 0;
    }
    
    // Allocate array for all channels
    enhanced_channel_t *all_channels = malloc(sizeof(enhanced_channel_t) * total_channels);
    if (!all_channels) {
        log_message(log_module, MSG_ERROR, "Failed to allocate memory for all channels");
        return -1;
    }
    
    int channel_index = 0;
    
    // Iterate through all frequencies and collect channels
    for (int i = 0; i < global_storage_v2->frequency_hash_size; i++) {
        frequency_data_t *current = global_storage_v2->frequency_hash_table[i];
        while (current) {
            pthread_mutex_lock(&current->lock);
            
            for (int j = 0; j < current->num_channels; j++) {
                if (channel_index < total_channels) {
                    memcpy(&all_channels[channel_index].base_channel, 
                           current->channels[j].base_channel, sizeof(mumudvb_channel_t));
                    all_channels[channel_index].frequency = current->frequency;
                    all_channels[channel_index].card_id = -1; // Not available in new structure
                    all_channels[channel_index].is_active = current->channels[j].is_active;
                    all_channels[channel_index].client_count = current->channels[j].client_count;
                    all_channels[channel_index].last_accessed = current->channels[j].last_accessed;
                    strncpy(all_channels[channel_index].channel_uri, 
                           current->channels[j].channel_uri, 
                           sizeof(all_channels[channel_index].channel_uri));
                    all_channels[channel_index].discovered_via_parallel = current->channels[j].discovered_via_parallel;
                    channel_index++;
                }
            }
            
            pthread_mutex_unlock(&current->lock);
            current = current->next;
        }
    }
    
    *channels = all_channels;
    *num_channels = channel_index;
    
    return 0;
}


/** Adapter function: Find channel by service ID (compatible with old API) */
int find_channel_by_service_id_adapter(int service_id, enhanced_channel_t **channel) {
    if (!global_storage_v2 || !channel) {
        return -1;
    }
    
    // Search through all frequencies
    for (int i = 0; i < global_storage_v2->frequency_hash_size; i++) {
        frequency_data_t *current = global_storage_v2->frequency_hash_table[i];
        while (current) {
            pthread_mutex_lock(&current->lock);
            
            for (int j = 0; j < current->num_channels; j++) {
                if (current->channels[j].base_channel->service_id == service_id) {
                    // Found the channel
                    *channel = malloc(sizeof(enhanced_channel_t));
                    if (*channel) {
                        memcpy(&(*channel)->base_channel, current->channels[j].base_channel, sizeof(mumudvb_channel_t));
                        (*channel)->frequency = current->frequency;
                        (*channel)->card_id = -1; // Not available in new structure
                        (*channel)->is_active = current->channels[j].is_active;
                        (*channel)->client_count = current->channels[j].client_count;
                        (*channel)->last_accessed = current->channels[j].last_accessed;
                        strncpy((*channel)->channel_uri, current->channels[j].channel_uri, 
                               sizeof((*channel)->channel_uri));
                        (*channel)->discovered_via_parallel = current->channels[j].discovered_via_parallel;
                    }
                    pthread_mutex_unlock(&current->lock);
                    return 0;
                }
            }
            
            pthread_mutex_unlock(&current->lock);
            current = current->next;
        }
    }
    
    *channel = NULL;
    return -1;
}

/** Print storage statistics */
void print_storage_adapter_stats(void) {
    if (!global_storage_v2) {
        log_message(log_module, MSG_ERROR, "Storage v2 not initialized");
        return;
    }
    
    print_storage_structure_v2(global_storage_v2);
}

/** Convert enhanced channels to base channels for HTTP endpoints */
int convert_enhanced_to_base_channels(enhanced_channel_t *enhanced_channels, int num_enhanced,
                                      mumudvb_channel_t **base_channels, int *num_base) {
    if (!enhanced_channels || !base_channels || !num_base || num_enhanced <= 0) {
        log_message(log_module, MSG_ERROR, "Invalid parameters for channel conversion");
        return -1;
    }
    
    // Allocate memory for base channels
    *base_channels = malloc(num_enhanced * sizeof(mumudvb_channel_t));
    if (!*base_channels) {
        log_message(log_module, MSG_ERROR, "Memory allocation failed for base channels");
        return -1;
    }
    
    // Copy base channel data from enhanced channels
    for (int i = 0; i < num_enhanced; i++) {
        memcpy(&(*base_channels)[i], &enhanced_channels[i].base_channel, sizeof(mumudvb_channel_t));
    }
    
    *num_base = num_enhanced;
    log_message(log_module, MSG_DEBUG, "Converted %d enhanced channels to base channels", num_enhanced);
    return 0;
}
