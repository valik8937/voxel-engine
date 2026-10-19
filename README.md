# Voxel Engine

A C++23 voxel engine built around the techniques from the "optimizing my voxel
engine" talk: runtime RLE chunks, automatic height bounds, 4x noise upsampling,
noise caching, compressed chunk transfer, and occlusion-based color ramping.

The networking path exists from the first commit. Single-player is an embedded
authoritative server connected over ENet loopback, not a separate code path.

## Current status

Working:

- Clang C++23, CMake presets, Ninja.
- RLE chunks sized `32 x 256 x 32`, column offsets, random access, edits,
  serialization and validation.
- Deterministic FastNoiseLite terrain: continents, hills, mountains, biomes,
  water and caves.
- 4x upsampled height/cave grids, per-column height bounds and noise caching.
- Zstandard-compressed chunks over the network and on disk.
- ENet server/client: handshake, chunk streaming, snapshots, player input and
  block edits.
- Authoritative server, embedded single-player server and a dedicated server
  executable.
- SDL_GPU/Vulkan renderer with chunk meshing, per-vertex ambient occlusion,
  color ramps, sky gradient, fog and frustum-independent chunk draws.
- First-person player with gravity, jumping, collision and client-side
  prediction/reconciliation.
- Block breaking/placing with server-side reach validation.
- Jolt physics wrapper (static triangle meshes, dynamic boxes) and unit test.
- miniaudio client audio and a click sound.
- Headless integration tests and a chunk generation benchmark.

Next:

- greedy meshing and chunk LOD;
- Jolt collision streaming for chunks around players;
- remote player rendering;
- trees and surface features;
- region-file persistence and client chunk cache;
- greedy meshing, shadows, water pass and better UI.

## Build

Requirements: Clang with C++23, CMake 3.28+, Ninja, SDL3, zstd, Vulkan loader,
`glslc`. CMake fetches pinned Jolt 5.6.0 and ENet 1.3.18; miniaudio 0.11.25 and
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

Single-player, rendered:

```sh
./build/release/voxel_client --singleplayer --radius 6
```

Dedicated server:

```sh
./build/release/voxel_server --port 7777 --seed 1337 --world worlds/default
./build/release/voxel_client --connect 127.0.0.1:7777 --radius 6
```

Headless smoke test (no window, useful for CI):

```sh
./build/release/voxel_client --singleplayer --headless --radius 3 --timeout 10
```

Benchmark:

```sh
./build/release/voxel_bench 512
```

On the development machine (Ryzen 7 4700U, release build): 512 chunks generated
in 0.128 s, about 1.05 Gvoxels/s, 40 KiB RLE per chunk, 0.93 KiB/chunk after
Zstandard. The rendered client holds 60 FPS at 1600x900 and radius 6.

## Controls

- Click: capture the mouse.
- `Esc`: release the mouse; press again to quit.
- `F1`: toggle mouse capture.
- `WASD`: move.
- `Space`: jump.
- `Shift`: sprint.
- Mouse: look.
- `LMB`: break block.
- `RMB`: place stone.

## Layout

```text
src/core       logging, math, job system, binary protocol helpers, zstd
src/game       shared player movement/collision
src/voxel      block registry, RLE chunks, world storage, raycast
src/worldgen   FastNoiseLite sampler, terrain/biome/cave generator
src/net        protocol, ENet client/server
src/server     authoritative server, persistence
src/physics    Jolt physics wrapper
src/audio      miniaudio engine
src/render     SDL_GPU renderer and chunk mesher
src/apps       server/client executables
tests          unit + loopback integration tests
benchmarks     chunk generation / memory benchmarks
```

## Networking model

- Server is authoritative. Clients receive compressed RLE chunks instead of
  generating terrain locally.
- Chunks are serialized, CRC32-checked, compressed with Zstandard and sent over
  reliable ENet channels.
- Player input is unreliable and sequenced; snapshots are unreliable and
  include the last processed input sequence.
- Block edits are validated for reach and block ID, applied on the server, then
  broadcast as `BlockUpdate`.
- Only modified chunks are persisted; unmodified terrain is deterministic from
  the seed and world spec.
