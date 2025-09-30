/**
 * @file unified_storage_adapter.h
 * @brief Header for unified storage adapter functions
 */

#ifndef _UNIFIED_STORAGE_ADAPTER_H
#define _UNIFIED_STORAGE_ADAPTER_H

// Forward declarations
typedef struct mumudvb_channel_t mumudvb_channel_t;
typedef struct unified_channel_storage_v2_t unified_channel_storage_v2_t;

// Adapter function declarations
int init_storage_adapter(unified_channel_storage_v2_t *storage_v2);
int add_channel_to_storage_adapter(const mumudvb_channel_t *base_channel, 
                                  double frequency, int card_id, int discovered_via_parallel);
int get_channels_for_frequency_adapter(double frequency, enhanced_channel_t **channels, int *num_channels);
int get_channels_for_card_adapter(int card_id, enhanced_channel_t **channels, int *num_channels);
int get_all_channels_adapter(enhanced_channel_t **channels, int *num_channels);
int find_channel_by_service_id_adapter(int service_id, enhanced_channel_t **channel);
void print_storage_adapter_stats(void);

#endif // _UNIFIED_STORAGE_ADAPTER_H
