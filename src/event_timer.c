/**
 * @file event_timer.c
 * @brief Event-based timer system implementation
 */

#include "event_timer.h"
#include "mumudvb_common.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

static event_timer_manager_t *global_timer_manager = NULL;

/**
 * @brief Background timer thread function
 */
static void *timer_thread_function(void *arg)
{
    event_timer_manager_t *manager = (event_timer_manager_t *)arg;
    struct timespec current_time;
    struct timespec timeout;
    
    log_message(log_module, MSG_DEBUG, "Event timer thread started");
    
    while (manager->active) {
        clock_gettime(CLOCK_MONOTONIC, &current_time);
        
        pthread_mutex_lock(&manager->mutex);
        
        int events_due = 0;
        for (int i = 0; i < manager->num_events; i++) {
            if (manager->events[i].active) {
                if (current_time.tv_sec > manager->events[i].next_expected.tv_sec ||
                    (current_time.tv_sec == manager->events[i].next_expected.tv_sec &&
                     current_time.tv_nsec >= manager->events[i].next_expected.tv_nsec)) {
                    events_due++;
                }
            }
        }
        
        if (events_due > 0) {
            // Signal waiting threads
            pthread_cond_broadcast(&manager->condition);
        }
        
        // Calculate next timeout
        timeout.tv_sec = current_time.tv_sec + 1; // Check every second
        timeout.tv_nsec = 0;
        
        pthread_cond_timedwait(&manager->condition, &manager->mutex, &timeout);
        pthread_mutex_unlock(&manager->mutex);
    }
    
    log_message(log_module, MSG_DEBUG, "Event timer thread stopped");
    return NULL;
}

int init_event_timer_manager(int max_events)
{
    if (global_timer_manager) {
        return 0; // Already initialized
    }
    
    global_timer_manager = malloc(sizeof(event_timer_manager_t));
    if (!global_timer_manager) {
        return -1;
    }
    
    // Set default values
    if (max_events == -1) {
        max_events = 0; // Default to unlimited
    }
    
    // Start with a reasonable initial allocation
    int initial_allocation = (max_events > 0) ? max_events : 100;
    
    global_timer_manager->events = calloc(initial_allocation, sizeof(timer_event_t));
    if (!global_timer_manager->events) {
        free(global_timer_manager);
        global_timer_manager = NULL;
        return -1;
    }
    
    global_timer_manager->max_events = max_events;
    global_timer_manager->num_events = 0;
    global_timer_manager->allocated_events = initial_allocation;
    global_timer_manager->active = 1;
    
    if (pthread_mutex_init(&global_timer_manager->mutex, NULL) != 0) {
        free(global_timer_manager->events);
        free(global_timer_manager);
        global_timer_manager = NULL;
        return -1;
    }
    
    if (pthread_cond_init(&global_timer_manager->condition, NULL) != 0) {
        pthread_mutex_destroy(&global_timer_manager->mutex);
        free(global_timer_manager->events);
        free(global_timer_manager);
        global_timer_manager = NULL;
        return -1;
    }
    
    // Start background timer thread
    if (pthread_create(&global_timer_manager->timer_thread, NULL, 
                      timer_thread_function, global_timer_manager) != 0) {
        pthread_cond_destroy(&global_timer_manager->condition);
        pthread_mutex_destroy(&global_timer_manager->mutex);
        free(global_timer_manager->events);
        free(global_timer_manager);
        global_timer_manager = NULL;
        return -1;
    }
    
    const char *limit_str = (max_events == 0) ? "unlimited" : "limited";
    log_message(log_module, MSG_DEBUG, "Event timer manager initialized with %s event limit (initial allocation: %d)", 
                limit_str, initial_allocation);
    return 0;
}

void cleanup_event_timer_manager(void)
{
    if (!global_timer_manager) {
        return;
    }
    
    log_message(log_module, MSG_DEBUG, "Cleaning up event timer manager with %d events", 
                global_timer_manager->num_events);
    
    // Stop timer thread
    global_timer_manager->active = 0;
    pthread_cond_signal(&global_timer_manager->condition);
    pthread_join(global_timer_manager->timer_thread, NULL);
    
    // Cleanup resources
    if (global_timer_manager->events) {
        free(global_timer_manager->events);
    }
    
    pthread_cond_destroy(&global_timer_manager->condition);
    pthread_mutex_destroy(&global_timer_manager->mutex);
    free(global_timer_manager);
    global_timer_manager = NULL;
    
    log_message(log_module, MSG_DEBUG, "Event timer manager cleanup completed");
}

static int expand_event_array_if_needed(void)
{
    if (!global_timer_manager) {
        return -1;
    }
    
    // Check if we need to expand
    if (global_timer_manager->num_events >= global_timer_manager->allocated_events) {
        // Check if we're at the limit
        if (global_timer_manager->max_events > 0 && 
            global_timer_manager->num_events >= global_timer_manager->max_events) {
            return -1; // At hard limit
        }
        
        // Expand by doubling the size, with a minimum increase of 10
        int new_size = global_timer_manager->allocated_events * 2;
        if (new_size < global_timer_manager->allocated_events + 10) {
            new_size = global_timer_manager->allocated_events + 10;
        }
        
        // Respect max_events if set
        if (global_timer_manager->max_events > 0 && new_size > global_timer_manager->max_events) {
            new_size = global_timer_manager->max_events;
        }
        
        timer_event_t *new_events = realloc(global_timer_manager->events, 
                                           new_size * sizeof(timer_event_t));
        if (!new_events) {
            log_message(log_module, MSG_ERROR, "Failed to expand event array from %d to %d", 
                       global_timer_manager->allocated_events, new_size);
            return -1;
        }
        
        // Zero out the new space
        memset(new_events + global_timer_manager->allocated_events, 0, 
               (new_size - global_timer_manager->allocated_events) * sizeof(timer_event_t));
        
        global_timer_manager->events = new_events;
        global_timer_manager->allocated_events = new_size;
        
        log_message(log_module, MSG_DEBUG, "Expanded event array from %d to %d slots", 
                   global_timer_manager->allocated_events / 2, new_size);
    }
    
    return 0;
}

int create_timer_event(timer_event_type_t event_type, int card_id, int channel_id, 
                      int interval_ms, void *user_data)
{
    if (!global_timer_manager) {
        return -1;
    }
    
    pthread_mutex_lock(&global_timer_manager->mutex);
    
    // Check if we're at the hard limit
    if (global_timer_manager->max_events > 0 && 
        global_timer_manager->num_events >= global_timer_manager->max_events) {
        pthread_mutex_unlock(&global_timer_manager->mutex);
        log_message(log_module, MSG_ERROR, "Event timer manager: maximum events reached (%d)", 
                   global_timer_manager->max_events);
        return -1;
    }
    
    // Expand array if needed
    if (expand_event_array_if_needed() != 0) {
        pthread_mutex_unlock(&global_timer_manager->mutex);
        log_message(log_module, MSG_ERROR, "Event timer manager: failed to expand event array");
        return -1;
    }
    
    int idx = global_timer_manager->num_events++;
    timer_event_t *event = &global_timer_manager->events[idx];
    
    event->event_type = event_type;
    event->card_id = card_id;
    event->channel_id = channel_id;
    event->interval_ms = interval_ms;
    event->user_data = user_data;
    event->active = 1;
    
    // Set initial timestamp
    clock_gettime(CLOCK_MONOTONIC, &event->timestamp);
    
    // Calculate next expected time
    event->next_expected.tv_sec = event->timestamp.tv_sec + (interval_ms / 1000);
    event->next_expected.tv_nsec = event->timestamp.tv_nsec + ((interval_ms % 1000) * 1000000);
    if (event->next_expected.tv_nsec >= 1000000000) {
        event->next_expected.tv_sec++;
        event->next_expected.tv_nsec -= 1000000000;
    }
    
    pthread_mutex_unlock(&global_timer_manager->mutex);
    
    log_message(log_module, MSG_DEBUG, "Created timer event: type=%d, card=%d, channel=%d, interval=%dms", 
               event_type, card_id, channel_id, interval_ms);
    return idx;
}

int wait_for_timer_event(timer_event_type_t event_type, int card_id, int channel_id, 
                        int timeout_ms)
{
    if (!global_timer_manager) {
        return -2;
    }
    
    struct timespec timeout;
    if (timeout_ms > 0) {
        clock_gettime(CLOCK_MONOTONIC, &timeout);
        timeout.tv_sec += timeout_ms / 1000;
        timeout.tv_nsec += (timeout_ms % 1000) * 1000000;
        if (timeout.tv_nsec >= 1000000000) {
            timeout.tv_sec++;
            timeout.tv_nsec -= 1000000000;
        }
    }
    
    pthread_mutex_lock(&global_timer_manager->mutex);
    
    while (global_timer_manager->active) {
        // Check if any matching event is due
        struct timespec current_time;
        clock_gettime(CLOCK_MONOTONIC, &current_time);
        
        for (int i = 0; i < global_timer_manager->num_events; i++) {
            timer_event_t *event = &global_timer_manager->events[i];
            if (event->active && event->event_type == event_type &&
                (card_id == -1 || event->card_id == card_id) &&
                (channel_id == -1 || event->channel_id == channel_id)) {
                
                if (current_time.tv_sec > event->next_expected.tv_sec ||
                    (current_time.tv_sec == event->next_expected.tv_sec &&
                     current_time.tv_nsec >= event->next_expected.tv_nsec)) {
                    
                    // Event is due, update next expected time
                    event->next_expected.tv_sec += event->interval_ms / 1000;
                    event->next_expected.tv_nsec += (event->interval_ms % 1000) * 1000000;
                    if (event->next_expected.tv_nsec >= 1000000000) {
                        event->next_expected.tv_sec++;
                        event->next_expected.tv_nsec -= 1000000000;
                    }
                    
                    pthread_mutex_unlock(&global_timer_manager->mutex);
                    return 0; // Event found
                }
            }
        }
        
        // Wait for next event or timeout
        if (timeout_ms == 0) {
            // No timeout, return immediately
            pthread_mutex_unlock(&global_timer_manager->mutex);
            return -1; // No event due
        } else if (timeout_ms > 0) {
            // Timed wait
            int ret = pthread_cond_timedwait(&global_timer_manager->condition, 
                                           &global_timer_manager->mutex, &timeout);
            if (ret == ETIMEDOUT) {
                pthread_mutex_unlock(&global_timer_manager->mutex);
                return -1; // Timeout
            }
        } else {
            // Infinite wait
            pthread_cond_wait(&global_timer_manager->condition, &global_timer_manager->mutex);
        }
    }
    
    pthread_mutex_unlock(&global_timer_manager->mutex);
    return -2; // Manager stopped
}

int wait_for_any_timer_event(int timeout_ms, timer_event_t *event)
{
    if (!global_timer_manager || !event) {
        return -2;
    }
    
    struct timespec timeout;
    if (timeout_ms > 0) {
        clock_gettime(CLOCK_MONOTONIC, &timeout);
        timeout.tv_sec += timeout_ms / 1000;
        timeout.tv_nsec += (timeout_ms % 1000) * 1000000;
        if (timeout.tv_nsec >= 1000000000) {
            timeout.tv_sec++;
            timeout.tv_nsec -= 1000000000;
        }
    }
    
    pthread_mutex_lock(&global_timer_manager->mutex);
    
    while (global_timer_manager->active) {
        // Check if any event is due
        struct timespec current_time;
        clock_gettime(CLOCK_MONOTONIC, &current_time);
        
        for (int i = 0; i < global_timer_manager->num_events; i++) {
            timer_event_t *evt = &global_timer_manager->events[i];
            if (evt->active) {
                if (current_time.tv_sec > evt->next_expected.tv_sec ||
                    (current_time.tv_sec == evt->next_expected.tv_sec &&
                     current_time.tv_nsec >= evt->next_expected.tv_nsec)) {
                    
                    // Copy event data
                    *event = *evt;
                    
                    // Update next expected time
                    evt->next_expected.tv_sec += evt->interval_ms / 1000;
                    evt->next_expected.tv_nsec += (evt->interval_ms % 1000) * 1000000;
                    if (evt->next_expected.tv_nsec >= 1000000000) {
                        evt->next_expected.tv_sec++;
                        evt->next_expected.tv_nsec -= 1000000000;
                    }
                    
                    pthread_mutex_unlock(&global_timer_manager->mutex);
                    return 0; // Event found
                }
            }
        }
        
        // Wait for next event or timeout
        if (timeout_ms == 0) {
            // No timeout, return immediately
            pthread_mutex_unlock(&global_timer_manager->mutex);
            return -1; // No event due
        } else if (timeout_ms > 0) {
            // Timed wait
            int ret = pthread_cond_timedwait(&global_timer_manager->condition, 
                                           &global_timer_manager->mutex, &timeout);
            if (ret == ETIMEDOUT) {
                pthread_mutex_unlock(&global_timer_manager->mutex);
                return -1; // Timeout
            }
        } else {
            // Infinite wait
            pthread_cond_wait(&global_timer_manager->condition, &global_timer_manager->mutex);
        }
    }
    
    pthread_mutex_unlock(&global_timer_manager->mutex);
    return -2; // Manager stopped
}

int is_timer_event_due(timer_event_type_t event_type, int card_id, int channel_id)
{
    if (!global_timer_manager) {
        return -1;
    }
    
    pthread_mutex_lock(&global_timer_manager->mutex);
    
    struct timespec current_time;
    clock_gettime(CLOCK_MONOTONIC, &current_time);
    
    for (int i = 0; i < global_timer_manager->num_events; i++) {
        timer_event_t *event = &global_timer_manager->events[i];
        if (event->active && event->event_type == event_type &&
            (card_id == -1 || event->card_id == card_id) &&
            (channel_id == -1 || event->channel_id == channel_id)) {
            
            if (current_time.tv_sec > event->next_expected.tv_sec ||
                (current_time.tv_sec == event->next_expected.tv_sec &&
                 current_time.tv_nsec >= event->next_expected.tv_nsec)) {
                pthread_mutex_unlock(&global_timer_manager->mutex);
                return 1; // Event is due
            }
        }
    }
    
    pthread_mutex_unlock(&global_timer_manager->mutex);
    return 0; // No event due
}

int get_time_until_next_timer_event(timer_event_type_t event_type, int card_id, int channel_id)
{
    if (!global_timer_manager) {
        return -1;
    }
    
    pthread_mutex_lock(&global_timer_manager->mutex);
    
    struct timespec current_time;
    clock_gettime(CLOCK_MONOTONIC, &current_time);
    
    int min_ms = -1;
    
    for (int i = 0; i < global_timer_manager->num_events; i++) {
        timer_event_t *event = &global_timer_manager->events[i];
        if (event->active && event->event_type == event_type &&
            (card_id == -1 || event->card_id == card_id) &&
            (channel_id == -1 || event->channel_id == channel_id)) {
            
            long diff_sec = event->next_expected.tv_sec - current_time.tv_sec;
            long diff_nsec = event->next_expected.tv_nsec - current_time.tv_nsec;
            
            if (diff_nsec < 0) {
                diff_sec--;
                diff_nsec += 1000000000;
            }
            
            int diff_ms = diff_sec * 1000 + diff_nsec / 1000000;
            if (diff_ms < 0) diff_ms = 0; // Event is due
            
            if (min_ms == -1 || diff_ms < min_ms) {
                min_ms = diff_ms;
            }
        }
    }
    
    pthread_mutex_unlock(&global_timer_manager->mutex);
    return min_ms;
}

int cancel_timer_event(timer_event_type_t event_type, int card_id, int channel_id)
{
    if (!global_timer_manager) {
        return -1;
    }
    
    pthread_mutex_lock(&global_timer_manager->mutex);
    
    for (int i = 0; i < global_timer_manager->num_events; i++) {
        timer_event_t *event = &global_timer_manager->events[i];
        if (event->active && event->event_type == event_type &&
            (card_id == -1 || event->card_id == card_id) &&
            (channel_id == -1 || event->channel_id == channel_id)) {
            
            event->active = 0;
            pthread_mutex_unlock(&global_timer_manager->mutex);
            return 0; // Event cancelled
        }
    }
    
    pthread_mutex_unlock(&global_timer_manager->mutex);
    return -1; // Event not found
}

int cancel_all_timer_events(timer_event_type_t event_type)
{
    if (!global_timer_manager) {
        return 0;
    }
    
    pthread_mutex_lock(&global_timer_manager->mutex);
    
    int cancelled = 0;
    for (int i = 0; i < global_timer_manager->num_events; i++) {
        timer_event_t *event = &global_timer_manager->events[i];
        if (event->active && event->event_type == event_type) {
            event->active = 0;
            cancelled++;
        }
    }
    
    pthread_mutex_unlock(&global_timer_manager->mutex);
    return cancelled;
}

int get_timer_event_count(void)
{
    if (!global_timer_manager) {
        return 0;
    }
    
    pthread_mutex_lock(&global_timer_manager->mutex);
    int count = global_timer_manager->num_events;
    pthread_mutex_unlock(&global_timer_manager->mutex);
    
    return count;
}

int get_timer_event_stats(int *total_events, int *active_events, int *due_events)
{
    if (!global_timer_manager || !total_events || !active_events || !due_events) {
        return -1;
    }
    
    pthread_mutex_lock(&global_timer_manager->mutex);
    
    *total_events = global_timer_manager->num_events;
    *active_events = 0;
    *due_events = 0;
    
    struct timespec current_time;
    clock_gettime(CLOCK_MONOTONIC, &current_time);
    
    for (int i = 0; i < global_timer_manager->num_events; i++) {
        timer_event_t *event = &global_timer_manager->events[i];
        if (event->active) {
            (*active_events)++;
            
            if (current_time.tv_sec > event->next_expected.tv_sec ||
                (current_time.tv_sec == event->next_expected.tv_sec &&
                 current_time.tv_nsec >= event->next_expected.tv_nsec)) {
                (*due_events)++;
            }
        }
    }
    
    pthread_mutex_unlock(&global_timer_manager->mutex);
    return 0;
}
