# Clock Handling and NTP Correction Behavior

## Overview

The event-based timing system has been designed to match the original single channel behavior while providing robust handling of clock backward jumps due to NTP corrections and other system clock changes.

## Key Features

### 1. **Timing Compatibility Modes**

The system supports three timing modes:

- **`TIMING_MODE_MONOTONIC`**: Uses `CLOCK_MONOTONIC` (not affected by NTP)
- **`TIMING_MODE_REALTIME`**: Uses `CLOCK_REALTIME` (affected by NTP, like original single channel)
- **`TIMING_MODE_AUTO`**: Automatically detects NTP corrections and switches modes

### 2. **Single Channel Behavior Matching**

The default configuration matches the original single channel system:

```c
timing_compatibility_config_t single_channel_config = {
    .mode = TIMING_MODE_REALTIME,              // Uses gettimeofday/time() like original
    .handle_clock_jumps = 1,                   // Handle NTP corrections
    .ntp_correction_threshold_ms = 1000,       // 1 second threshold for detection
    .max_clock_jump_ms = 5000                  // 5 second maximum acceptable jump
};
```

### 3. **Clock Backward Jump Handling**

When a clock backward jump is detected:

1. **Detection**: Compares real-time and monotonic clocks to identify NTP corrections
2. **Logging**: Logs the clock jump with details about the time difference
3. **Recovery**: Resets all timing events to current time + their intervals
4. **Continuation**: Continues normal operation without timing disruption

### 4. **NTP Correction Detection**

The system detects NTP corrections by:

- Monitoring both real-time and monotonic clocks
- Detecting when real-time jumps backward significantly more than monotonic
- Using configurable thresholds to avoid false positives
- Automatically switching to appropriate timing mode

## Implementation Details

### Clock Functions Used

| Function | Clock Type | NTP Affected | Usage |
|----------|------------|--------------|-------|
| `gettimeofday()` | Real-time | Yes | Original single channel |
| `time()` | Real-time | Yes | Original single channel |
| `clock_gettime(CLOCK_REALTIME)` | Real-time | Yes | Compatibility layer |
| `clock_gettime(CLOCK_MONOTONIC)` | Monotonic | No | Event-based timing |

### Time Conversion

The system provides conversion functions between clock types:

- `monotonic_to_realtime()`: Convert monotonic time to real-time equivalent
- `realtime_to_monotonic()`: Convert real-time to monotonic equivalent

### Event Timing Behavior

1. **Normal Operation**: Uses real-time clock (like original single channel)
2. **Clock Jump Detection**: Automatically detects backward jumps
3. **Event Rescheduling**: Resets all timing events to prevent missed events
4. **Graceful Recovery**: Continues operation without timing disruption

## Comparison with Original Single Channel

| Aspect | Original Single Channel | Event-Based Timing |
|--------|------------------------|-------------------|
| Clock Source | `gettimeofday()` / `time()` | `CLOCK_REALTIME` (compatible) |
| NTP Handling | No special handling | Automatic detection and recovery |
| Clock Jumps | May cause timing issues | Graceful handling with rescheduling |
| Precision | Second-level precision | Nanosecond precision |
| Robustness | Basic | Enhanced with fallback modes |

## Configuration Options

### Timing Modes

```c
// Use monotonic time (not affected by NTP)
config.mode = TIMING_MODE_MONOTONIC;

// Use real-time (affected by NTP, like original)
config.mode = TIMING_MODE_REALTIME;

// Auto-detect NTP corrections
config.mode = TIMING_MODE_AUTO;
```

### Clock Jump Handling

```c
// Enable clock jump handling
config.handle_clock_jumps = 1;

// Set NTP correction detection threshold
config.ntp_correction_threshold_ms = 1000;  // 1 second

// Set maximum acceptable clock jump
config.max_clock_jump_ms = 5000;  // 5 seconds
```

## Testing

The system includes comprehensive tests for:

- Clock backward jump simulation
- NTP correction detection
- Time conversion accuracy
- Event rescheduling behavior
- Compatibility with original timing

## Benefits

1. **Compatibility**: Matches original single channel behavior exactly
2. **Robustness**: Handles clock changes gracefully
3. **Precision**: Higher precision than original system
4. **Flexibility**: Configurable timing modes for different use cases
5. **Reliability**: Automatic recovery from timing disruptions

## Usage

The timing system automatically initializes with single channel compatibility:

```c
// Create timing tracker (automatically uses single channel config)
event_timing_tracker_t tracker;
create_event_timing_tracker(64, NULL, &tracker);

// All timing operations now use real-time clock like original
struct timespec current_time = get_compatible_time();
long time_diff = get_compatible_time_diff_ms(&start, &end);
```

This ensures that the event-based timing system behaves identically to the original single channel system while providing enhanced robustness for clock changes.
