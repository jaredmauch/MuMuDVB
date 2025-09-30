# MuMuDVB Tests and Examples

This directory contains test files, example code, and experimental modules that are not part of the main MuMuDVB build.

## Files

### Example Files
- `fast_signal_example.c` - Example implementation of fast signal handling
- `dvb_events_example.c` - Example of DVB event handling

### Test Files
- `test_event_timing.c` - Test program for event timing functionality

### Experimental Modules
- `timing_compatibility.c` / `timing_compatibility.h` - Timing compatibility layer (moved from src/)
- `signal_optimization.c` / `signal_optimization.h` - Signal optimization module (moved from src/)

## Building

These files are not included in the main MuMuDVB build. To compile them individually, you would need to:

1. Include the necessary headers from the main source directory
2. Link against the main MuMuDVB libraries
3. Handle any missing dependencies

## Status

These files were moved from the main source directory because:
- They are example/test code not needed for production builds
- They were not included in the main Makefile.am
- They contain experimental or incomplete functionality

If you need to use any of this functionality, you may need to integrate it back into the main source tree and update the build system accordingly.
