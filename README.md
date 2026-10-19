# Voxel Engine

A C++23 voxel engine built around the techniques from the "optimizing my voxel
engine" talk: runtime RLE chunks, automatic height bounds, 4x noise upsampling,
noise caching, compressed chunk transfer, and (next) occlusion-based color
ramping.

The networking path exists from the first commit. Single-player is an embedded
authoritative server connected over ENet loopback, not a separate code path.

## Current status

Working foundation:

- Clang C++23, CMake presets, Ninja.
- RLE chunks sized `32 x 256 x 32`, column offsets, random access, edits,
  serialization, validation.
- Deterministic FastNoiseLite terrain: continents, hills, mountains, biomes,
  water, caves.
- 4x upsampled height/cave grids, per-column height bounds, noise caching.
- Zstandard-compressed chunks over the network and on disk.
- ENet server/client: handshake, chunk streaming, snapshots, block edits.
- Headless server executable, headless client executable, integration tests,
  chunk generation benchmark.
- Jolt and miniaudio are pinned and built/ready for the gameplay/render phase.

Not implemented yet:

- SDL_GPU terrain renderer and input camera.
- chunk meshing and AO color ramps.
- player movement/prediction.
- Jolt collision streaming and miniaudio playback.

## Build

Requirements: Clang with C++23, CMake 3.28+, Ninja, SDL3, zstd, Vulkan loader.
CMake fetches pinned Jolt 5.6.0 and ENet 1.3.18; miniaudio 0.11.25 and
FastNoiseLite are vendored under `third_party/`.

```sh
cmake --preset debug
cmake --build --preset debug -j
ctest --preset debug
```

Release:

```sh
cmake --preset release
cmake --build --preset release -j
```

## Run

Single-player (embedded server):

```sh
./build/debug/voxel_client --singleplayer --radius 4
```

Dedicated server:

```sh
./build/debug/voxel_server --port 7777 --seed 1337 --world worlds/default
./build/debug/voxel_client --connect 127.0.0.1:7777 --radius 4
```

Benchmark chunk generation:

```sh
./build/release/voxel_bench 512
```

## Layout

```text
src/core       logging, math, job system, binary protocol helpers, zstd
src/voxel      block registry, RLE chunks, world storage
src/worldgen   FastNoiseLite sampler, terrain/biome/cave generator
src/net        protocol, ENet client/server
src/server     authoritative server, persistence
src/apps       server/client executables
tests          unit + loopback integration tests
benchmarks     chunk generation / memory benchmarks
```

## Networking model

- Server is authoritative. Clients receive compressed RLE chunks instead of
  generating terrain locally.
- Chunks are serialized, CRC32-checked, compressed with Zstandard, and sent over
  reliable ENet channels.
- Snapshots use an unreliable ENet channel.
- Block edits are validated for reach and block ID, applied on the server, then
  broadcast as `BlockUpdate`.
- Only modified chunks are persisted; unmodified terrain is deterministic from
  the seed and world spec.
