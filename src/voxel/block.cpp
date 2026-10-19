#include "voxel/block.h"

namespace vox {
namespace {

constexpr std::array<BlockDefinition, blocks::Count> kBlocks{{
    {"air", false, false, false,
     {{{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}}}},
    {"stone", true, true, false,
     {{{72, 74, 78, 255}, {92, 95, 100, 255}, {112, 115, 120, 255}, {136, 139, 144, 255}}}},
    {"dirt", true, true, false,
     {{{78, 53, 35, 255}, {95, 66, 43, 255}, {113, 79, 51, 255}, {132, 94, 61, 255}}}},
    {"grass", true, true, false,
     {{{55, 93, 38, 255}, {72, 116, 48, 255}, {91, 140, 60, 255}, {116, 163, 75, 255}}}},
    {"sand", true, true, false,
     {{{154, 137, 90, 255}, {178, 160, 108, 255}, {202, 184, 128, 255}, {224, 208, 156, 255}}}},
    {"water", false, false, true,
     {{{28, 74, 120, 170}, {36, 94, 148, 170}, {48, 116, 176, 170}, {68, 140, 200, 170}}}},
    {"snow", true, true, false,
     {{{190, 205, 218, 255}, {210, 224, 235, 255}, {230, 240, 248, 255}, {248, 252, 255, 255}}}},
    {"bedrock", true, true, false,
     {{{34, 34, 38, 255}, {44, 44, 49, 255}, {55, 55, 61, 255}, {68, 68, 74, 255}}}},
    {"gravel", true, true, false,
     {{{82, 79, 76, 255}, {99, 96, 92, 255}, {117, 113, 108, 255}, {136, 132, 126, 255}}}},
    {"clay", true, true, false,
     {{{111, 119, 134, 255}, {128, 137, 153, 255}, {146, 156, 172, 255}, {166, 176, 192, 255}}}},
    {"wood", true, true, false,
     {{{74, 53, 31, 255}, {92, 66, 39, 255}, {112, 81, 48, 255}, {134, 98, 59, 255}}}},
    {"leaves", true, true, false,
     {{{34, 72, 31, 255}, {45, 92, 40, 255}, {58, 116, 50, 255}, {76, 140, 64, 255}}}},
}};

} // namespace

const BlockDefinition& block_definition(BlockId id) noexcept {
    return kBlocks[id < blocks::Count ? id : blocks::Air];
}

bool block_is_solid(BlockId id) noexcept { return block_definition(id).solid; }
bool block_is_opaque(BlockId id) noexcept { return block_definition(id).opaque; }
bool block_is_liquid(BlockId id) noexcept { return block_definition(id).liquid; }
std::string_view block_name(BlockId id) noexcept { return block_definition(id).name; }

} // namespace vox
