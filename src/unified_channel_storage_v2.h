/**
 * @file unified_channel_storage_v2.h
 * @brief New hierarchical unified channel storage system
 * 
 * Architecture: Card -> Frequency -> Channel List
 * - Frequencies are deduplicated (same frequency data shared across cards)
 * - Channels within a frequency are deduplicated
 * - Efficient lookups and memory usage
 */

#ifndef _UNIFIED_CHANNEL_STORAGE_V2_H
#define _UNIFIED_CHANNEL_STORAGE_V2_H

#include <pthread.h>
#include <time.h>

// Forward declarations
typedef struct mumudvb_channel_t mumudvb_channel_t;

// Forward declarations
typedef struct frequency_data_t frequency_data_t;
typedef struct card_frequency_mapping_t card_frequency_mapping_t;
typedef struct unified_channel_storage_v2_t unified_channel_storage_v2_t;

/** Channel information within a frequency */
typedef struct {
    /** Base channel information */
    struct mumudvb_channel_t *base_channel;
    /** Whether this channel is currently active (tuned and streaming) */
    int is_active;
    /** Number of clients currently using this channel */
    int client_count;
    /** When this channel was last accessed */
    time_t last_accessed;
    /** URI for accessing this channel via HTTP */
    char channel_uri[256];
    /** Whether this channel was discovered via parallel scanning */
    int discovered_via_parallel;
    /** Reference count - how many cards have this channel */
    int ref_count;
} frequency_channel_t;

/** Frequency data structure (deduplicated) */
typedef struct frequency_data_t {
    /** Frequency in Hz */
    double frequency;
    /** Array of channels on this frequency */
    frequency_channel_t *channels;
    /** Number of channels on this frequency */
    int num_channels;
    /** Maximum number of channels for this frequency */
    int max_channels;
    /** Reference count - how many cards can access this frequency */
    int ref_count;
    /** When this frequency was last accessed */
    time_t last_accessed;
    /** Mutex for thread-safe access to this frequency */
    pthread_mutex_t lock;
    /** Next frequency in the list (for hash table chaining) */
    frequency_data_t *next;
} frequency_data_t;

/** Card to frequency mapping */
typedef struct card_frequency_mapping_t {
    /** Card ID */
    int card_id;
    /** Array of frequency pointers this card can access */
    frequency_data_t **frequencies;
    /** Number of frequencies this card can access */
    int num_frequencies;
    /** Maximum number of frequencies for this card */
    int max_frequencies;
    /** Whether this card is currently in use */
    int in_use;
    /** Current frequency being tuned (NULL if idle) */
    frequency_data_t *current_frequency;
    /** Mutex for thread-safe access to this card */
    pthread_mutex_t lock;
} card_frequency_mapping_t;

/** New unified channel storage system */
typedef struct unified_channel_storage_v2_t {
    /** Hash table of frequencies (for O(1) lookup) */
    frequency_data_t **frequency_hash_table;
    /** Size of the frequency hash table */
    int frequency_hash_size;
    /** Number of frequencies currently stored */
    int num_frequencies;
    
    /** Array of card mappings */
    card_frequency_mapping_t *card_mappings;
    /** Number of cards */
    int num_cards;
    /** Maximum number of cards */
    int max_cards;
    
    /** Global mutex for thread-safe access */
    pthread_mutex_t global_lock;
    
    /** Statistics */
    int total_channels;
    int total_frequencies;
    int total_cards;
} unified_channel_storage_v2_t;

// Function declarations

/** Initialize the new unified channel storage system */
int init_unified_channel_storage_v2(unified_channel_storage_v2_t *storage, 
                                   int max_cards, int max_frequencies, int max_channels_per_freq);

/** Clean up the unified channel storage system */
void cleanup_unified_channel_storage_v2(unified_channel_storage_v2_t *storage);

/** Add a card to the system */
int add_card_to_storage_v2(unified_channel_storage_v2_t *storage, int card_id);

/** Add a frequency to a card (with deduplication) */
int add_frequency_to_card_v2(unified_channel_storage_v2_t *storage, int card_id, double frequency);

/** Add a channel to a frequency (with deduplication) */
int add_channel_to_frequency_v2(unified_channel_storage_v2_t *storage, 
                                double frequency, const mumudvb_channel_t *base_channel,
                                int discovered_via_parallel);

/** Get all channels for a specific frequency */
int get_channels_for_frequency_v2(unified_channel_storage_v2_t *storage, double frequency,
                                 frequency_channel_t **channels, int *num_channels);

/** Get all frequencies for a specific card */
int get_frequencies_for_card_v2(unified_channel_storage_v2_t *storage, int card_id,
                               frequency_data_t **frequencies, int *num_frequencies);

/** Get all channels for a specific card */
int get_all_channels_for_card_v2(unified_channel_storage_v2_t *storage, int card_id,
                                frequency_channel_t **channels, int *num_channels);

/** Find a specific channel by service ID and frequency */
int find_channel_v2(unified_channel_storage_v2_t *storage, double frequency, int service_id,
                   frequency_channel_t **channel);

/** Update channel status (active/inactive, client count) */
int update_channel_status_v2(unified_channel_storage_v2_t *storage, double frequency, int service_id,
                            int is_active, int client_count);

/** Remove a channel from a frequency */
int remove_channel_from_frequency_v2(unified_channel_storage_v2_t *storage, 
                                    double frequency, int service_id);

/** Remove a frequency from a card */
int remove_frequency_from_card_v2(unified_channel_storage_v2_t *storage, int card_id, double frequency);

/** Get storage statistics */
void get_storage_stats_v2(unified_channel_storage_v2_t *storage, 
                         int *total_channels, int *total_frequencies, int *total_cards);

/** Print storage structure (for debugging) */
void print_storage_structure_v2(unified_channel_storage_v2_t *storage);

#endif // _UNIFIED_CHANNEL_STORAGE_V2_H
