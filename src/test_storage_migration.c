/**
 * @file test_storage_migration.c
 * @brief Test program to verify the new storage system migration
 */

#include "unified_channel_storage_v2.h"
#include "unified_storage_adapter.h"
#include "mumudvb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *log_module = "StorageTest: ";

int main() {
    printf("Testing new hierarchical storage system migration...\n");
    
    // Initialize logging (skip for test)
    // init_log();
    
    // Create test storage system
    unified_channel_storage_v2_t storage;
    if (init_unified_channel_storage_v2(&storage, 4, 16, 32) != 0) {
        printf("ERROR: Failed to initialize storage v2\n");
        return -1;
    }
    
    // Initialize adapter
    if (init_storage_adapter(&storage) != 0) {
        printf("ERROR: Failed to initialize storage adapter\n");
        cleanup_unified_channel_storage_v2(&storage);
        return -1;
    }
    
    // Test 1: Add cards
    printf("Test 1: Adding cards...\n");
    if (add_card_to_storage_v2(&storage, 0) != 0) {
        printf("ERROR: Failed to add card 0\n");
        return -1;
    }
    if (add_card_to_storage_v2(&storage, 1) != 0) {
        printf("ERROR: Failed to add card 1\n");
        return -1;
    }
    printf("✓ Cards added successfully\n");
    
    // Test 2: Add frequencies to cards (with deduplication)
    printf("Test 2: Adding frequencies with deduplication...\n");
    double freq1 = 593000000.0; // 593 MHz
    double freq2 = 597000000.0; // 597 MHz
    
    if (add_frequency_to_card_v2(&storage, 0, freq1) != 0) {
        printf("ERROR: Failed to add frequency %.0f to card 0\n", freq1);
        return -1;
    }
    if (add_frequency_to_card_v2(&storage, 1, freq1) != 0) {
        printf("ERROR: Failed to add frequency %.0f to card 1\n", freq1);
        return -1;
    }
    if (add_frequency_to_card_v2(&storage, 0, freq2) != 0) {
        printf("ERROR: Failed to add frequency %.0f to card 0\n", freq2);
        return -1;
    }
    printf("✓ Frequencies added with deduplication\n");
    
    // Test 3: Add channels using adapter (backward compatibility)
    printf("Test 3: Adding channels using adapter...\n");
    
    // Create test channels
    mumudvb_channel_t channel1 = {0};
    strncpy(channel1.name, "Test Channel 1", sizeof(channel1.name));
    channel1.service_id = 1;
    
    mumudvb_channel_t channel2 = {0};
    strncpy(channel2.name, "Test Channel 2", sizeof(channel2.name));
    channel2.service_id = 2;
    
    // Add channels using adapter
    if (add_channel_to_storage_adapter(&channel1, freq1, 0, 0) != 0) {
        printf("ERROR: Failed to add channel 1 via adapter\n");
        return -1;
    }
    if (add_channel_to_storage_adapter(&channel2, freq1, 0, 0) != 0) {
        printf("ERROR: Failed to add channel 2 via adapter\n");
        return -1;
    }
    if (add_channel_to_storage_adapter(&channel1, freq1, 1, 0) != 0) {
        printf("ERROR: Failed to add channel 1 to card 1 (should deduplicate)\n");
        return -1;
    }
    printf("✓ Channels added with deduplication\n");
    
    // Test 4: Retrieve channels
    printf("Test 4: Retrieving channels...\n");
    enhanced_channel_t *channels = NULL;
    int num_channels = 0;
    
    if (get_channels_for_frequency_adapter(freq1, &channels, &num_channels) != 0) {
        printf("ERROR: Failed to get channels for frequency %.0f\n", freq1);
        return -1;
    }
    
    printf("✓ Found %d channels on frequency %.0f Hz\n", num_channels, freq1);
    for (int i = 0; i < num_channels; i++) {
        printf("  - %s (SID: %d)\n", channels[i].base_channel.name, channels[i].base_channel.service_id);
    }
    
    if (channels) {
        free(channels);
    }
    
    // Test 5: Print storage statistics
    printf("Test 5: Storage statistics...\n");
    print_storage_adapter_stats();
    
    // Test 6: Test deduplication
    printf("Test 6: Testing deduplication...\n");
    int total_channels, total_frequencies, total_cards;
    get_storage_stats_v2(&storage, &total_channels, &total_frequencies, &total_cards);
    
    printf("Total channels: %d (should be 2, not 3 due to deduplication)\n", total_channels);
    printf("Total frequencies: %d (should be 2)\n", total_frequencies);
    printf("Total cards: %d (should be 2)\n", total_cards);
    
    if (total_channels == 2 && total_frequencies == 2 && total_cards == 2) {
        printf("✓ Deduplication working correctly\n");
    } else {
        printf("ERROR: Deduplication not working correctly\n");
        return -1;
    }
    
    // Cleanup
    cleanup_unified_channel_storage_v2(&storage);
    
    printf("\n🎉 All tests passed! Migration successful!\n");
    printf("The new hierarchical storage system is working correctly with:\n");
    printf("- Card -> Frequency -> Channel hierarchy\n");
    printf("- Frequency deduplication across cards\n");
    printf("- Channel deduplication within frequencies\n");
    printf("- Backward compatibility via adapter functions\n");
    
    return 0;
}
