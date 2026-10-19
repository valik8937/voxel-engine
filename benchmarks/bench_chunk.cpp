#include "core/job_system.h"
#include "core/zstd.h"
#include "voxel/chunk.h"
#include "worldgen/generator.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

int main(int argc, char** argv) {
    const int count = argc > 1 ? std::atoi(argv[1]) : 256;
    if (count <= 0 || count > 4096) {
        std::fprintf(stderr, "Usage: voxel_bench [chunk_count:1..4096]\n");
        return 2;
    }

    vox::WorldGenerator generator(vox::WorldSpec::make_default(1337));
    vox::JobSystem jobs;
    std::vector<vox::Chunk> chunks(static_cast<std::size_t>(count));

    const auto start = std::chrono::steady_clock::now();
    jobs.parallel_for(chunks.size(), 1, [&](std::size_t index) {
        const int side = 64;
        const int x = static_cast<int>(index % side) - side / 2;
        const int z = static_cast<int>(index / side) - side / 2;
        chunks[index] = generator.generate(vox::ChunkCoord{x, z});
    });
    const auto end = std::chrono::steady_clock::now();

    const double seconds = std::chrono::duration<double>(end - start).count();
    std::size_t runs = 0;
    std::size_t bytes = 0;
    std::size_t serialized = 0;
    std::size_t compressed = 0;
    std::size_t valid = 0;

    vox::ZstdCompressor compressor(3);
    for (std::size_t i = 0; i < chunks.size(); ++i) {
        const vox::Chunk& chunk = chunks[i];
        runs += chunk.run_count();
        bytes += chunk.memory_bytes();
        if (chunk.validate()) {
            ++valid;
        }
        if (i < 32) {
            vox::BinaryWriter writer;
            chunk.serialize(writer);
            serialized += writer.size();
            if (auto z = compressor.compress(writer.data())) {
                compressed += z->size();
            }
        }
    }

    const double voxels = static_cast<double>(count) * vox::kChunkVoxelCount;
    std::printf("chunks: %d\n", count);
    std::printf("time: %.3f s\n", seconds);
    std::printf("throughput: %.1f Mvoxels/s\n", voxels / seconds / 1.0e6);
    std::printf("chunks/s: %.1f\n", count / seconds);
    std::printf("valid: %zu/%d\n", valid, count);
    std::printf("runs: %zu (avg %.1f/chunk)\n", runs, static_cast<double>(runs) / count);
    std::printf("RLE memory: %.2f MiB (avg %.1f KiB/chunk)\n",
                static_cast<double>(bytes) / (1024.0 * 1024.0),
                static_cast<double>(bytes) / count / 1024.0);
    if (serialized != 0) {
        std::printf("sample serialized: %.2f KiB/chunk\n",
                    static_cast<double>(serialized) / 32.0 / 1024.0);
        std::printf("sample zstd: %.2f KiB/chunk (%.1f%%)\n",
                    static_cast<double>(compressed) / 32.0 / 1024.0,
                    100.0 * static_cast<double>(compressed) / serialized);
    }
    return valid == static_cast<std::size_t>(count) ? 0 : 1;
}
