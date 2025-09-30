/**
 * @file unified_channel_storage_v2.c
 * @brief Implementation of the new hierarchical unified channel storage system
 */

#include "unified_channel_storage_v2.h"
#include "mumudvb.h"
#include "log.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

static char *log_module = "StorageV2: ";

// Hash function for frequency lookup
static unsigned int frequency_hash(double frequency, int table_size) {
    // Convert frequency to integer (Hz) and hash
    unsigned long long freq_int = (unsigned long long)frequency;
    return (freq_int * 2654435761U) % table_size;
}

// Find frequency data in hash table
static frequency_data_t *find_frequency_data(unified_channel_storage_v2_t *storage, double frequency) {
    unsigned int hash = frequency_hash(frequency, storage->frequency_hash_size);
    frequency_data_t *current = storage->frequency_hash_table[hash];
    
    while (current) {
        if (current->frequency == frequency) {
            return current;
        }
        current = current->next;
    }
    return NULL;
}

// Add frequency data to hash table
static int add_frequency_data(unified_channel_storage_v2_t *storage, frequency_data_t *freq_data) {
    unsigned int hash = frequency_hash(freq_data->frequency, storage->frequency_hash_size);
    
    // Add to head of chain
    freq_data->next = storage->frequency_hash_table[hash];
    storage->frequency_hash_table[hash] = freq_data;
    
    return 0;
}

// Find card mapping
static card_frequency_mapping_t *find_card_mapping(unified_channel_storage_v2_t *storage, int card_id) {
    for (int i = 0; i < storage->num_cards; i++) {
        if (storage->card_mappings[i].card_id == card_id) {
            return &storage->card_mappings[i];
        }
    }
    return NULL;
}

int init_unified_channel_storage_v2(unified_channel_storage_v2_t *storage, 
                                   int max_cards, int max_frequencies, int max_channels_per_freq) {
    if (!storage || max_cards <= 0 || max_frequencies <= 0 || max_channels_per_freq <= 0) {
        log_message(log_module, MSG_ERROR, "Invalid parameters for storage initialization");
        return -1;
    }
    
    memset(storage, 0, sizeof(unified_channel_storage_v2_t));
    
    // Initialize global mutex
    if (pthread_mutex_init(&storage->global_lock, NULL) != 0) {
        log_message(log_module, MSG_ERROR, "Failed to initialize global mutex");
        return -1;
    }
    
    // Allocate frequency hash table (use prime number for better distribution)
    storage->frequency_hash_size = 101; // Prime number
    storage->frequency_hash_table = calloc(storage->frequency_hash_size, sizeof(frequency_data_t*));
    if (!storage->frequency_hash_table) {
        log_message(log_module, MSG_ERROR, "Failed to allocate frequency hash table");
        pthread_mutex_destroy(&storage->global_lock);
        return -1;
    }
    
    // Allocate card mappings
    storage->max_cards = max_cards;
    storage->card_mappings = calloc(max_cards, sizeof(card_frequency_mapping_t));
    if (!storage->card_mappings) {
        log_message(log_module, MSG_ERROR, "Failed to allocate card mappings");
        free(storage->frequency_hash_table);
        pthread_mutex_destroy(&storage->global_lock);
        return -1;
    }
    
    // Initialize card mappings
    for (int i = 0; i < max_cards; i++) {
        storage->card_mappings[i].card_id = -1; // Mark as unused
        if (pthread_mutex_init(&storage->card_mappings[i].lock, NULL) != 0) {
            log_message(log_module, MSG_ERROR, "Failed to initialize card mutex %d", i);
            // Cleanup previous mutexes
            for (int j = 0; j < i; j++) {
                pthread_mutex_destroy(&storage->card_mappings[j].lock);
            }
            free(storage->card_mappings);
            free(storage->frequency_hash_table);
            pthread_mutex_destroy(&storage->global_lock);
            return -1;
        }
    }
    
    log_message(log_module, MSG_INFO, "Unified channel storage v2 initialized: max_cards=%d, max_frequencies=%d, max_channels_per_freq=%d",
                max_cards, max_frequencies, max_channels_per_freq);
    
    return 0;
}

void cleanup_unified_channel_storage_v2(unified_channel_storage_v2_t *storage) {
    if (!storage) return;
    
    pthread_mutex_lock(&storage->global_lock);
    
    // Clean up all frequency data
    for (int i = 0; i < storage->frequency_hash_size; i++) {
        frequency_data_t *current = storage->frequency_hash_table[i];
        while (current) {
            frequency_data_t *next = current->next;
            // Free base_channel pointers
            if (current->channels) {
                for (int j = 0; j < current->num_channels; j++) {
                    if (current->channels[j].base_channel) {
                        free(current->channels[j].base_channel);
                    }
                }
                free(current->channels);
            }
            pthread_mutex_destroy(&current->lock);
            free(current);
            current = next;
        }
    }
    free(storage->frequency_hash_table);
    
    // Clean up card mappings
    for (int i = 0; i < storage->max_cards; i++) {
        if (storage->card_mappings[i].card_id >= 0) {
            free(storage->card_mappings[i].frequencies);
        }
        pthread_mutex_destroy(&storage->card_mappings[i].lock);
    }
    free(storage->card_mappings);
    
    pthread_mutex_unlock(&storage->global_lock);
    pthread_mutex_destroy(&storage->global_lock);
    
    log_message(log_module, MSG_INFO, "Unified channel storage v2 cleaned up");
}

int add_card_to_storage_v2(unified_channel_storage_v2_t *storage, int card_id) {
    if (!storage || card_id < 0) {
        log_message(log_module, MSG_ERROR, "Invalid card ID");
        return -1;
    }
    
    pthread_mutex_lock(&storage->global_lock);
    
    // Check if card already exists
    if (find_card_mapping(storage, card_id)) {
        log_message(log_module, MSG_DEBUG, "Card %d already exists", card_id);
        pthread_mutex_unlock(&storage->global_lock);
        return 0;
    }
    
    // Find empty slot
    for (int i = 0; i < storage->max_cards; i++) {
        if (storage->card_mappings[i].card_id < 0) {
            storage->card_mappings[i].card_id = card_id;
            storage->card_mappings[i].num_frequencies = 0;
            storage->card_mappings[i].max_frequencies = 16; // Start with 16 frequencies
            storage->card_mappings[i].frequencies = calloc(16, sizeof(frequency_data_t*));
            if (!storage->card_mappings[i].frequencies) {
                log_message(log_module, MSG_ERROR, "Failed to allocate frequencies for card %d", card_id);
                storage->card_mappings[i].card_id = -1;
                pthread_mutex_unlock(&storage->global_lock);
                return -1;
            }
            storage->num_cards++;
            storage->total_cards++;
            log_message(log_module, MSG_INFO, "Added card %d to storage", card_id);
            pthread_mutex_unlock(&storage->global_lock);
            return 0;
        }
    }
    
    log_message(log_module, MSG_ERROR, "No space for card %d (max_cards=%d)", card_id, storage->max_cards);
    pthread_mutex_unlock(&storage->global_lock);
    return -1;
}

int add_frequency_to_card_v2(unified_channel_storage_v2_t *storage, int card_id, double frequency) {
    if (!storage || card_id < 0 || frequency <= 0) {
        log_message(log_module, MSG_ERROR, "Invalid parameters");
        return -1;
    }
    
    pthread_mutex_lock(&storage->global_lock);
    
    // Find card mapping
    card_frequency_mapping_t *card_mapping = find_card_mapping(storage, card_id);
    if (!card_mapping) {
        log_message(log_module, MSG_ERROR, "Card %d not found", card_id);
        pthread_mutex_unlock(&storage->global_lock);
        return -1;
    }
    
    pthread_mutex_lock(&card_mapping->lock);
    
    // Check if frequency already exists for this card
    for (int i = 0; i < card_mapping->num_frequencies; i++) {
        if (card_mapping->frequencies[i] && card_mapping->frequencies[i]->frequency == frequency) {
            log_message(log_module, MSG_DEBUG, "Frequency %.0f Hz already exists for card %d", frequency, card_id);
            pthread_mutex_unlock(&card_mapping->lock);
            pthread_mutex_unlock(&storage->global_lock);
            return 0;
        }
    }
    
    // Find or create frequency data
    frequency_data_t *freq_data = find_frequency_data(storage, frequency);
    if (!freq_data) {
        // Create new frequency data
        freq_data = malloc(sizeof(frequency_data_t));
        if (!freq_data) {
            log_message(log_module, MSG_ERROR, "Failed to allocate frequency data");
            pthread_mutex_unlock(&card_mapping->lock);
            pthread_mutex_unlock(&storage->global_lock);
            return -1;
        }
        
        freq_data->frequency = frequency;
        freq_data->channels = calloc(128, sizeof(frequency_channel_t));
        if (!freq_data->channels) {
            log_message(log_module, MSG_ERROR, "Failed to allocate channels array");
            free(freq_data);
            pthread_mutex_unlock(&card_mapping->lock);
            pthread_mutex_unlock(&storage->global_lock);
            return -1;
        }
        freq_data->num_channels = 0;
        freq_data->max_channels = 128; // Default max channels per frequency
        freq_data->ref_count = 0;
        freq_data->last_accessed = time(NULL);
        freq_data->next = NULL;
        
        if (pthread_mutex_init(&freq_data->lock, NULL) != 0) {
            log_message(log_module, MSG_ERROR, "Failed to initialize frequency mutex");
            free(freq_data);
            pthread_mutex_unlock(&card_mapping->lock);
            pthread_mutex_unlock(&storage->global_lock);
            return -1;
        }
        
        // Add to hash table
        add_frequency_data(storage, freq_data);
        storage->num_frequencies++;
        storage->total_frequencies++;
    }
    
    // Add frequency to card
    if (card_mapping->num_frequencies >= card_mapping->max_frequencies) {
        // Resize frequency array
        int new_size = card_mapping->max_frequencies * 2;
        frequency_data_t **new_frequencies = realloc(card_mapping->frequencies, 
                                                    new_size * sizeof(frequency_data_t*));
        if (!new_frequencies) {
            log_message(log_module, MSG_ERROR, "Failed to resize frequency array for card %d", card_id);
            pthread_mutex_unlock(&card_mapping->lock);
            pthread_mutex_unlock(&storage->global_lock);
            return -1;
        }
        card_mapping->frequencies = new_frequencies;
        card_mapping->max_frequencies = new_size;
    }
    
    card_mapping->frequencies[card_mapping->num_frequencies] = freq_data;
    card_mapping->num_frequencies++;
    freq_data->ref_count++;
    
    log_message(log_module, MSG_INFO, "Added frequency %.0f Hz to card %d (ref_count=%d)", 
                frequency, card_id, freq_data->ref_count);
    
    pthread_mutex_unlock(&card_mapping->lock);
    pthread_mutex_unlock(&storage->global_lock);
    return 0;
}

int add_channel_to_frequency_v2(unified_channel_storage_v2_t *storage, 
                                double frequency, const mumudvb_channel_t *base_channel,
                                int discovered_via_parallel) {
    if (!storage || !base_channel || frequency <= 0) {
        log_message(log_module, MSG_ERROR, "Invalid parameters");
        return -1;
    }
    
    pthread_mutex_lock(&storage->global_lock);
    
    // Find frequency data
    frequency_data_t *freq_data = find_frequency_data(storage, frequency);
    if (!freq_data) {
        log_message(log_module, MSG_ERROR, "Frequency %.0f Hz not found", frequency);
        pthread_mutex_unlock(&storage->global_lock);
        return -1;
    }
    
    pthread_mutex_lock(&freq_data->lock);
    
    // Check if channel already exists
    for (int i = 0; i < freq_data->num_channels; i++) {
        if (freq_data->channels[i].base_channel->service_id == base_channel->service_id) {
            log_message(log_module, MSG_DEBUG, "Channel %d already exists on frequency %.0f Hz", 
                       base_channel->service_id, frequency);
            freq_data->channels[i].ref_count++;
            pthread_mutex_unlock(&freq_data->lock);
            pthread_mutex_unlock(&storage->global_lock);
            return 0;
        }
    }
    
    // Check if we have space
    if (freq_data->num_channels >= freq_data->max_channels) {
        log_message(log_module, MSG_ERROR, "Frequency %.0f Hz is full (%d channels)", 
                   frequency, freq_data->max_channels);
        pthread_mutex_unlock(&freq_data->lock);
        pthread_mutex_unlock(&storage->global_lock);
        return -1;
    }
    
    // Add channel
    frequency_channel_t *new_channel = &freq_data->channels[freq_data->num_channels];
    new_channel->base_channel = malloc(sizeof(mumudvb_channel_t));
    if (!new_channel->base_channel) {
        log_message(log_module, MSG_ERROR, "Failed to allocate memory for base channel");
        pthread_mutex_unlock(&freq_data->lock);
        pthread_mutex_unlock(&storage->global_lock);
        return -1;
    }
    memcpy(new_channel->base_channel, base_channel, sizeof(mumudvb_channel_t));
    new_channel->is_active = 0;
    new_channel->client_count = 0;
    new_channel->last_accessed = time(NULL);
    new_channel->discovered_via_parallel = discovered_via_parallel;
    new_channel->ref_count = 1;
    
    // Generate URI
    snprintf(new_channel->channel_uri, sizeof(new_channel->channel_uri),
             "/unified/frequency/%.0f/channel/%d", frequency, new_channel->base_channel->service_id);
    
    freq_data->num_channels++;
    storage->total_channels++;
    
    log_message(log_module, MSG_INFO, "Added channel %s (SID %d) to frequency %.0f Hz", 
                new_channel->base_channel->name, new_channel->base_channel->service_id, frequency);
    
    pthread_mutex_unlock(&freq_data->lock);
    pthread_mutex_unlock(&storage->global_lock);
    return 0;
}

int get_channels_for_frequency_v2(unified_channel_storage_v2_t *storage, double frequency,
                                 frequency_channel_t **channels, int *num_channels) {
    if (!storage || !channels || !num_channels || frequency <= 0) {
        return -1;
    }
    
    pthread_mutex_lock(&storage->global_lock);
    
    frequency_data_t *freq_data = find_frequency_data(storage, frequency);
    if (!freq_data) {
        *channels = NULL;
        *num_channels = 0;
        pthread_mutex_unlock(&storage->global_lock);
        return 0;
    }
    
    pthread_mutex_lock(&freq_data->lock);
    *channels = freq_data->channels;
    *num_channels = freq_data->num_channels;
    freq_data->last_accessed = time(NULL);
    pthread_mutex_unlock(&freq_data->lock);
    
    pthread_mutex_unlock(&storage->global_lock);
    return 0;
}

void get_storage_stats_v2(unified_channel_storage_v2_t *storage, 
                         int *total_channels, int *total_frequencies, int *total_cards) {
    if (!storage) return;
    
    pthread_mutex_lock(&storage->global_lock);
    
    if (total_channels) *total_channels = storage->total_channels;
    if (total_frequencies) *total_frequencies = storage->total_frequencies;
    if (total_cards) *total_cards = storage->total_cards;
    
    pthread_mutex_unlock(&storage->global_lock);
}

int update_channel_status_v2(unified_channel_storage_v2_t *storage, double frequency, int service_id,
                            int is_active, int client_count) {
    if (!storage || frequency <= 0) {
        return -1;
    }
    
    pthread_mutex_lock(&storage->global_lock);
    
    frequency_data_t *freq_data = find_frequency_data(storage, frequency);
    if (!freq_data) {
        pthread_mutex_unlock(&storage->global_lock);
        return -1;
    }
    
    pthread_mutex_lock(&freq_data->lock);
    
    for (int i = 0; i < freq_data->num_channels; i++) {
        if (freq_data->channels[i].base_channel->service_id == service_id) {
            freq_data->channels[i].is_active = is_active;
            freq_data->channels[i].client_count = client_count;
            freq_data->channels[i].last_accessed = time(NULL);
            pthread_mutex_unlock(&freq_data->lock);
            pthread_mutex_unlock(&storage->global_lock);
            return 0;
        }
    }
    
    pthread_mutex_unlock(&freq_data->lock);
    pthread_mutex_unlock(&storage->global_lock);
    return -1;
}

void print_storage_structure_v2(unified_channel_storage_v2_t *storage) {
    if (!storage) return;
    
    pthread_mutex_lock(&storage->global_lock);
    
    log_message(log_module, MSG_INFO, "=== Storage Structure ===");
    log_message(log_module, MSG_INFO, "Total Cards: %d", storage->total_cards);
    log_message(log_module, MSG_INFO, "Total Frequencies: %d", storage->total_frequencies);
    log_message(log_module, MSG_INFO, "Total Channels: %d", storage->total_channels);
    
    // Print frequency information
    for (int i = 0; i < storage->frequency_hash_size; i++) {
        frequency_data_t *current = storage->frequency_hash_table[i];
        while (current) {
            log_message(log_module, MSG_INFO, "Frequency %.0f Hz: %d channels, ref_count=%d", 
                       current->frequency, current->num_channels, current->ref_count);
            current = current->next;
        }
    }
    
    // Print card information
    for (int i = 0; i < storage->max_cards; i++) {
        if (storage->card_mappings[i].card_id >= 0) {
            log_message(log_module, MSG_INFO, "Card %d: %d frequencies", 
                       storage->card_mappings[i].card_id, storage->card_mappings[i].num_frequencies);
        }
    }
    
    pthread_mutex_unlock(&storage->global_lock);
}

int get_all_channels_for_card_v2(unified_channel_storage_v2_t *storage, int card_id,
                                frequency_channel_t **channels, int *num_channels) {
    if (!storage || !channels || !num_channels || card_id < 0) {
        return -1;
    }
    
    pthread_mutex_lock(&storage->global_lock);
    
    *num_channels = 0;
    *channels = NULL;
    
    // Find card mapping
    card_frequency_mapping_t *card_mapping = find_card_mapping(storage, card_id);
    if (!card_mapping) {
        pthread_mutex_unlock(&storage->global_lock);
        return 0; // Card not found, but not an error
    }
    
    // Count total channels for this card
    int total_channels = 0;
    for (int i = 0; i < card_mapping->num_frequencies; i++) {
        frequency_data_t *freq_data = card_mapping->frequencies[i];
        if (freq_data) {
            pthread_mutex_lock(&freq_data->lock);
            total_channels += freq_data->num_channels;
            pthread_mutex_unlock(&freq_data->lock);
        }
    }
    
    if (total_channels == 0) {
        pthread_mutex_unlock(&storage->global_lock);
        return 0;
    }
    
    // Allocate array for all channels
    frequency_channel_t *all_channels = malloc(sizeof(frequency_channel_t) * total_channels);
    if (!all_channels) {
        pthread_mutex_unlock(&storage->global_lock);
        return -1;
    }
    
    int channel_index = 0;
    
    // Collect channels from all frequencies for this card
    for (int i = 0; i < card_mapping->num_frequencies; i++) {
        frequency_data_t *freq_data = card_mapping->frequencies[i];
        if (freq_data) {
            pthread_mutex_lock(&freq_data->lock);
            
            for (int j = 0; j < freq_data->num_channels; j++) {
                if (channel_index < total_channels) {
                    all_channels[channel_index] = freq_data->channels[j];
                    channel_index++;
                }
            }
            
            pthread_mutex_unlock(&freq_data->lock);
        }
    }
    
    *channels = all_channels;
    *num_channels = channel_index;
    
    pthread_mutex_unlock(&storage->global_lock);
    return 0;
}
