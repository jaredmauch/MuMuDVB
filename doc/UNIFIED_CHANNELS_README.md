# MuMuDVB Unified Channel System

## Overview

The Unified Channel System is an advanced feature in MuMuDVB that allows you to configure multiple tuners and frequencies in a single instance, with automatic assignment of clients to available tuners. This system is particularly useful when you have multiple DVB cards and want to efficiently utilize them without manual configuration of each tuner-frequency combination.

## Key Benefits

- **Automatic Resource Management**: The system automatically detects which tuners can access which frequencies and assigns them dynamically
- **Resource Optimization**: Uses the minimum number of tuners to cover all frequencies, while supporting multiple unicast clients per frequency
- **Simplified Configuration**: Instead of manually configuring each tuner-frequency pair, you simply list all tuners and frequencies
- **Single Port Access**: All channels are served on the same HTTP port with frequency-based routing
- **Better Resource Utilization**: Tuners are used efficiently based on actual demand rather than static assignments
- **Easier Scaling**: Adding new tuners or frequencies requires minimal configuration changes

## Quick Start

### Basic Configuration

```bash
# Define your tuners (4 tuners: 2 on card 0, 2 on card 1)
unified_tuners=0,1,2,3
unified_cards=0,0,1,1

# Define all frequencies to serve (in Hz)
unified_frequencies=509000000,515000000,521000000,539000000,581000000,593000000

# Common settings
autoconfiguration=full
unicast=1
port_http=10000
```

### Client Access

Clients can access channels using the following URL format:

```
http://server:port/freq/FREQUENCY
```

Examples:
- `http://localhost:10000/freq/509000000` - Accesses 509 MHz frequency
- `http://localhost:10000/freq/515000000` - Accesses 515 MHz frequency

## Configuration Parameters

| Parameter | Description | Example | Required |
|-----------|-------------|---------|----------|
| `unified_tuners` | Comma-separated list of tuner IDs | `0,1,2,3` | Yes |
| `unified_cards` | Comma-separated list of card IDs for each tuner | `0,0,1,1` | Yes |
| `unified_frequencies` | Comma-separated list of frequencies to serve (in Hz) | `509000000,515000000` | Yes |

## How It Works

### Startup Phase

1. **Configuration Parsing**: The system reads the unified configuration parameters and validates them
2. **Tuner Initialization**: Each specified tuner is initialized with its corresponding card
3. **Capability Detection**: The system tests each tuner to determine which frequencies it can access
4. **Resource Mapping**: A mapping table is created showing which tuners can serve which frequencies

### Runtime Phase

1. **Client Request**: When a client connects requesting a specific frequency
2. **Tuner Selection**: The system selects an available tuner that can access the requested frequency
3. **Resource Optimization**: If multiple tuners can access the same frequency, only one tuner is assigned, but it can serve multiple unicast clients
4. **Dynamic Assignment**: Tuners are assigned and released dynamically based on demand

## Migration from Single Tuner

### Before (Single Tuner)
```bash
card=0
tuner=0
freq=189000000
port_http=10000+%card
```

### After (Unified System)
```bash
unified_tuners=0,1,2,3
unified_cards=0,0,1,1
unified_frequencies=189000000,509000000,515000000,521000000,539000000,581000000,593000000
port_http=10000
```

## Example Configurations

### Basic Setup
See `doc/configuration_examples/unified_channels.conf` for a complete basic configuration.

### Migration Guide
See `doc/configuration_examples/migration_example.conf` for detailed migration instructions.

### Troubleshooting
See `doc/configuration_examples/unified_troubleshooting.conf` for common issues and solutions.

## Troubleshooting

### Common Issues

**No tuner available for frequency**
- Check that the frequency is correctly specified in `unified_frequencies`
- Verify at least one tuner can physically access the frequency
- Ensure tuners are not all busy with other clients

**Tuner initialization failed**
- Check DVB cards are properly installed and recognized
- Verify tuner IDs match the physical tuners
- Ensure card IDs correctly map to installed cards

**Configuration errors**
- Verify tuner and card lists have the same length
- Check frequency format (must be in Hz)
- Ensure tuner IDs are unique

## Advanced Features

### Multi-Client Architecture
The unified channel system efficiently handles multiple unicast clients:

- **Single Tuner per Frequency**: Each frequency is assigned to exactly one tuner to minimize resource usage
- **Multiple Clients per Tuner**: Each tuner can serve multiple unicast clients simultaneously for the same frequency
- **Shared Stream Processing**: The tuner processes the DVB stream once and distributes it to all connected clients
- **Efficient Resource Usage**: Instead of dedicating a tuner per client, one tuner handles all clients for a frequency

**Example Scenario:**
- 3 clients request frequency 509000000 Hz
- 2 clients request frequency 515000000 Hz  
- 1 client requests frequency 521000000 Hz
- **Result**: Only 3 tuners needed (one per frequency), not 6 tuners

### Resource Optimization
The system uses the minimum number of tuners needed to cover all frequencies, while supporting multiple unicast clients per frequency. Each tuner can serve multiple clients simultaneously for the same frequency/sub-channel.

### Automatic Failover
If a tuner becomes unavailable, the system automatically switches to another tuner.

### Thread Safety
The system uses mutexes to ensure thread-safe operation across multiple tuners.

## Performance Considerations

- **Tuner Count**: More tuners provide better load balancing but require more system resources
- **Frequency Count**: More frequencies increase startup time due to capability detection
- **Client Load**: The system automatically scales based on client demand

## Limitations

- All tuners must be on the same system (single MuMuDVB instance)
- Frequencies must be specified in Hz
- Tuner and card lists must have matching lengths
- Tuner IDs must be unique within the system

## Support

For additional help with the unified channel system:

1. Check the troubleshooting guide in `doc/configuration_examples/unified_troubleshooting.conf`
2. Review the migration examples in `doc/configuration_examples/migration_example.conf`
3. Consult the main documentation in `doc/README_CONF.asciidoc`
4. Check the MuMuDVB project documentation at http://mumudvb.net
