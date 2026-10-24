#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include "function_finder.h"

namespace gbarecomp {

// Statically discovered instruction spans, compiled before the game runs.
// Fixed instruction fields are guarded; supported immediate operands are live
// data. Matching neither decodes instructions nor creates executable code.
struct RamCodeTemplate {
    Function function;
    std::vector<uint8_t> bytes;
    Function cart_function{};
    // Optional per-byte guards. Only an AL, non-S MOV/ADD immediate operand
    // may be data; operation, condition, registers and flags stay exact.
    std::vector<uint8_t> masks;
};

std::vector<RamCodeTemplate> collect_ram_code_templates(
    const std::vector<Function>& functions, const uint8_t* image,
    std::size_t image_size, uint32_t image_base, const std::string& prefix,
    const std::vector<DataRange>* data_ranges = nullptr,
    const std::vector<CodeCopyRange>* code_copies = nullptr,
    const std::vector<ConstantCodePatch>* code_patches = nullptr);

}  // namespace gbarecomp
