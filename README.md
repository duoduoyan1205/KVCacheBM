# DDR Buffer Manager Simulation

This repository contains a C implementation of a simple DDR-side buffer manager for KV Cache access simulation.

## Build and run

```sh
make
./ddr_buffer_manager trace.txt
```

## Trace format

Each non-comment line in the trace file is one access:

```text
R <key>
W <key> <payload>
```

- `R` reads a buffer unit by key.
- `W` writes a payload to a buffer unit by key.
- Keys support decimal or `0x` hexadecimal syntax.
- Payload data is truncated to `BUFFER_UNIT_SIZE - 1` bytes.

## Design

- `BUFFER_UNIT_SIZE`, `BUFFER_COUNT`, `L1_HASH_SIZE`, and `L2_HASH_SIZE` are compile-time macros in `ddr_buffer_manager.c`.
- Lookup uses a two-level hash table: level 1 chooses a coarse bucket and level 2 chooses a sub-bucket. Collisions are chained inside the selected sub-bucket.
- Eviction is score based. Reads, writes, misses, dirty state, and last-access ticks influence which buffer is retained or evicted.
- The program keeps a simulated backing store and an independent expected store, then verifies every read against the expected store to validate buffer-manager correctness.
