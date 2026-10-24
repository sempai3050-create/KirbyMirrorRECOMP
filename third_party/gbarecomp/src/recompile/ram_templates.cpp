#include "ram_templates.h"

#include <algorithm>
#include <cstdio>
#include <set>
#include <unordered_set>
#include "arm_decode.h"
#include "arm_codegen.h"
#include "thumb_decode.h"

namespace gbarecomp {

std::vector<RamCodeTemplate> collect_ram_code_templates(
    const std::vector<Function>& original_functions, const uint8_t* image,
    std::size_t image_size, uint32_t image_base, const std::string& prefix,
    const std::vector<DataRange>* data_ranges,
    const std::vector<CodeCopyRange>* code_copies,
    const std::vector<ConstantCodePatch>* code_patches) {
    std::vector<Function> functions=original_functions;
    const bool matrix_cart=image_size>0x02000000u && image[0xAC]=='M';
    std::vector<RamCodeTemplate> result;
    std::set<std::pair<CpuMode, std::vector<uint8_t>>> seen;
    std::set<uint32_t> references;
    std::set<uint32_t> thumb_sources;
    auto source_of=[](const Function& fn) { return fn.source_addr ? fn.source_addr : fn.addr; };
    struct PatchSpan { uint32_t begin,end,marker,replacement; };
    std::vector<PatchSpan> patches;
    auto spans_of=[](const Function& fn) {
        auto spans=fn.switch_blocks;
        spans.push_back({fn.addr,fn.end_addr});
        return spans;
    };
    auto read_word=[&](int64_t address, uint32_t width, uint32_t* value) {
        if (address < image_base || uint64_t(address-image_base)+width > image_size)
            return false;
        *value=0;
        for (uint32_t b=0;b<width;++b)
            *value |= uint32_t(image[std::size_t(address-image_base)+b]) << (8u*b);
        return true;
    };
    auto decode=[&](const Function& fn,uint32_t pc,armv4t::Instr* instruction) {
        uint32_t raw=0;
        if (!read_word(int64_t(source_of(fn))+int64_t(pc)-fn.addr,
                       fn.mode==CpuMode::Thumb ? 2u : 4u,&raw)) return false;
        *instruction=fn.mode==CpuMode::Thumb
            ? armv4t::ThumbDecoder::decode(uint16_t(raw),pc)
            : armv4t::ArmDecoder::decode(raw,pc);
        return true;
    };
    auto append_patches=[&](const std::vector<Function>& bodies,
                           const std::vector<ConstantCodePatch>& updates) {
        for (const auto& update : updates) for (const auto& root : bodies) {
            if (root.mode!=CpuMode::Arm || root.addr!=update.runtime_base ||
                source_of(root)==root.addr || root.end_addr<=root.addr) continue;
            const int64_t bias=int64_t(source_of(root))-root.addr;
            std::vector<uint32_t> pending={source_of(root)};
            std::unordered_set<uint32_t> visited;
            for (std::size_t i=0;i<pending.size();++i) {
                if (!visited.insert(pending[i]).second) continue;
                for (const auto& body : bodies) {
                    if (body.mode!=CpuMode::Arm || source_of(body)!=pending[i] ||
                        int64_t(source_of(body))-body.addr!=bias) continue;
                    // Discovery may split the converter at loop/resume labels.
                    // Follow its non-call CFG, rather than clipping a proven
                    // initializer to the first such label or guessing an end.
                    for (const auto& span : spans_of(body)) {
                        patches.push_back({source_of(body)+(span.start-body.addr),
                            source_of(body)+(span.end-body.addr),update.marker,update.replacement});
                        armv4t::Instr last{};
                        bool have_last=false;
                        for (uint32_t pc=span.start;pc<span.end;pc+=4u) {
                            armv4t::Instr ins{};
                            if (!decode(body,pc,&ins)) break;
                            if (ins.op==armv4t::IrOp::B) {
                                const int64_t target=bias+ins.branch_target;
                                if (target>=image_base && uint64_t(target-image_base)<image_size)
                                    pending.push_back(uint32_t(target));
                            }
                            last=ins; have_last=true;
                        }
                        if (have_last && !last.is_return && !last.is_indirect &&
                            !(last.op==armv4t::IrOp::B && last.cond==armv4t::Cond::AL))
                            pending.push_back(source_of(body)+(span.end-body.addr));
                    }
                }
            }
        }
    };
    if (code_patches) append_patches(original_functions,*code_patches);
    auto add_reference=[&](uint32_t value) {
        const uint32_t address=value & ~1u;
        if (address >= image_base && uint64_t(address-image_base)<image_size) {
            references.insert(address);
            thumb_sources.insert(address);
        }
    };
    // Restrict the catalog to address-taken code and known ROM-backed copies.
    // Full decomp symbol imports can contain tens of thousands of functions;
    // compiling every body a second time would add substantial build cost.
    for (const auto& fn : functions) {
        const uint32_t step=fn.mode==CpuMode::Thumb ? 2u : 4u;
        for (const auto& span : spans_of(fn)) {
            for (uint32_t pc=span.start;pc<span.end;pc+=step) {
                armv4t::Instr ins{};
                if (!decode(fn,pc,&ins)) break;
                if (ins.op==armv4t::IrOp::LDR && ins.mem.rn==15 &&
                    !ins.mem.by_register && !ins.mem.writeback) {
                    uint32_t pool=pc+(ins.thumb ? 4u : 8u);
                    if (ins.thumb) pool &= ~3u;
                    if (ins.mem.pre_indexed)
                        pool=ins.mem.add ? pool+ins.mem.imm_offset : pool-ins.mem.imm_offset;
                    uint32_t value=0;
                    if (read_word(int64_t(source_of(fn))+int64_t(pool)-fn.addr,4,&value))
                        add_reference(value);
                }
                if ((ins.op==armv4t::IrOp::ADD || ins.op==armv4t::IrOp::SUB) &&
                    ins.rn==15 && ins.rd!=15 && ins.op2.kind==armv4t::Op2::Kind::Imm) {
                    uint32_t value=pc+(ins.thumb ? 4u : 8u);
                    if (ins.thumb) value &= ~3u;
                    value=ins.op==armv4t::IrOp::ADD
                        ? value+ins.op2.imm_value : value-ins.op2.imm_value;
                    add_reference(value);
                }
            }
        }
    }
    // A copy-source literal may name code that never executes in ROM. DMA and
    // byte-copy sources may omit the Thumb tag, so qualify the PUSH-LR bytes.
    // Keep these bodies in the byte-verified RAM catalog rather than adding ROM
    // dispatch roots or enabling unrestricted speculative literal harvesting.
    // The window is an analysis budget, not an inferred runtime copy extent.
    std::unordered_set<uint64_t> known;
    for (const auto& fn : functions)
        known.insert((uint64_t(fn.addr)<<1)|unsigned(fn.mode==CpuMode::Thumb));
    for (uint32_t source : thumb_sources) {
        const auto key=(uint64_t(source)<<1)|1u;
        if (known.count(key)) continue;
        uint32_t first=0;
        if (!read_word(source,2,&first) || (first & 0xFF00u)!=0xB500u) continue;
        bool valid=true;
        for (uint32_t offset=0;offset<12;offset+=2) {
            uint32_t opcode=0;
            if (!read_word(uint64_t(source)+offset,2,&opcode) ||
                armv4t::ThumbDecoder::decode(uint16_t(opcode),source+offset).is_undefined)
                valid=false;
        }
        if (!valid) continue;
        const auto image_offset=std::size_t(source-image_base);
        const auto window=std::min<std::size_t>(4096u,image_size-image_offset);
        FunctionFinder finder(image+image_offset,window,source);
        finder.set_speculative_literal_harvest(false);
        if (data_ranges) for (const auto& range : *data_ranges)
            finder.add_data_range(range.start,range.end,range.note);
        finder.add_seed({source,CpuMode::Thumb,"",0,true});
        finder.run(2048);
        for (const auto& fn : finder.functions()) {
            const auto found=(uint64_t(fn.addr)<<1)|unsigned(fn.mode==CpuMode::Thumb);
            if (known.insert(found).second) functions.push_back(fn);
        }
    }
    // Banked cartridges can carry several independently linked versions of a
    // copied module. Its startup entry can retain the ARM mode/MSR prefix while
    // subsequent literal offsets and function bodies differ. Locate siblings
    // of a known copy by that exact eight-byte system-mode setup signature,
    // then discover each with its own source mapping. They enter only the
    // byte-verified catalog, never the fixed-address dispatch table.
    if (matrix_cart && code_copies) {
        std::set<std::pair<uint32_t,uint32_t>> modules;
        for (const auto& fn : original_functions) {
            if (fn.mode!=CpuMode::Arm || fn.source_addr==fn.addr || !fn.source_addr)
                continue;
            uint32_t first=0,second=0;
            if (!read_word(fn.source_addr,4,&first) ||
                !read_word(uint64_t(fn.source_addr)+4u,4,&second) ||
                (first & 0xFFFFFFE0u)!=0xE3A00000u || second!=0xE129F000u)
                continue;
            const uint32_t mode=first & 31u;
            if (mode!=0x12u && mode!=0x13u && mode!=0x1Fu) continue;
            for (const auto& copy : *code_copies) {
                if (fn.addr<copy.runtime_start || uint64_t(fn.addr-copy.runtime_start)>=copy.size)
                    continue;
                const uint32_t entry_offset=fn.addr-copy.runtime_start;
                const auto* signature=image+(fn.source_addr-image_base);
                const auto* cursor=image;
                while (cursor<image+image_size) {
                    const auto* found=std::search(cursor,image+image_size,signature,signature+8);
                    if (found==image+image_size) break;
                    cursor=found+1;
                    const uint64_t source=uint64_t(image_base)+(found-image);
                    if ((source & 3u) || source<entry_offset) continue;
                    const uint64_t module=source-entry_offset;
                    if (module<image_base || module-image_base+copy.size>image_size ||
                        !modules.emplace(copy.runtime_start,uint32_t(module)).second) continue;
                    FunctionFinder variant(image,image_size,image_base);
                    variant.set_speculative_literal_harvest(false);
                    uint32_t code_size=copy.size;
                    uint32_t header_base=0,header_end=0,header_entry=0;
                    const bool load_header=read_word(module+0x38u,4,&header_base) &&
                        read_word(module+0x3Cu,4,&header_end) &&
                        read_word(module+0x40u,4,&header_entry) &&
                        header_base==copy.runtime_start && header_entry==fn.addr &&
                        header_end>fn.addr && header_end-header_base<=copy.size;
                    if (load_header) code_size=header_end-header_base;
                    variant.add_code_copy(copy.runtime_start,uint32_t(module),code_size,
                                          "guarded module sibling");
                    if (load_header) for (uint32_t record=8u;record<0x38u;record+=12u) {
                        uint32_t destination=0,begin=0,end=0;
                        if (!read_word(module+record,4,&destination) ||
                            !read_word(module+record+4u,4,&begin) ||
                            !read_word(module+record+8u,4,&end)) continue;
                        const uint32_t region=destination>>24;
                        if ((region!=2u && region!=3u) || begin<header_end || end<=begin ||
                            end-begin>(region==2u ? 0x40000u : 0x8000u) ||
                            uint64_t(destination)+(end-begin)>(uint64_t(region+1u)<<24)) continue;
                        const uint64_t backing=module+uint64_t(begin-header_base);
                        if (backing<image_base || backing-image_base+(end-begin)>image_size)
                            continue;
                        variant.add_code_copy(destination,uint32_t(backing),end-begin,
                                              "module initialized RAM section");
                    }
                    if (data_ranges) for (const auto& range : *data_ranges) {
                        // Runtime annotations describe the original linked
                        // layout. A sibling can place its tables elsewhere.
                        bool relocated=false;
                        for (const auto& section : variant.code_copies())
                            relocated |= range.end>section.runtime_start &&
                                uint64_t(range.start)<uint64_t(section.runtime_start)+section.size;
                        if (!relocated) variant.add_data_range(range.start,range.end,range.note);
                    }
                    variant.add_seed({fn.addr,CpuMode::Arm,"",uint32_t(source),true});
                    variant.run(4096);
                    append_patches(variant.functions(),variant.constant_code_patches());
                    for (const auto& body : variant.functions())
                        if (body.source_addr && body.source_addr!=body.addr)
                            functions.push_back(body);
                }
            }
        }
    }
    std::vector<bool> selected(functions.size(),false);
    bool changed=true;
    while (changed) {
        changed=false;
        for (std::size_t i=0;i<functions.size();++i) {
            if (selected[i]) continue;
            const auto& fn=functions[i];
            const auto source=source_of(fn);
            bool eligible=source!=fn.addr || (matrix_cart &&
                fn.addr>=0x08000000u && fn.addr<0x0E000000u &&
                (fn.addr & 0x01FFFFFFu)<0x2000u);
            for (const auto& span : spans_of(fn)) {
                const uint32_t start=source+(span.start-fn.addr);
                const auto it=references.lower_bound(start);
                eligible |= it!=references.end() && uint64_t(*it-start)<span.end-span.start;
            }
            if (!eligible) continue;
            selected[i]=true;
            changed=true;
            const uint32_t step=fn.mode==CpuMode::Thumb ? 2u : 4u;
            for (const auto& span : spans_of(fn)) {
                armv4t::Instr last{};
                bool have_last=false;
                for (uint32_t pc=span.start;pc<span.end;pc+=step) {
                    armv4t::Instr ins{};
                    if (!decode(fn,pc,&ins)) break;
                    if (ins.op==armv4t::IrOp::B)
                        add_reference(source+(ins.branch_target-fn.addr));
                    last=ins; have_last=true;
                }
                if (have_last && !last.is_return && !last.is_indirect &&
                    !(last.op==armv4t::IrOp::B && last.cond==armv4t::Cond::AL))
                    add_reference(source+(span.end-fn.addr));
            }
        }
    }
    // Small native fragments also cover partial copies and function suffixes.
    // Boundaries are deterministic; all transfer/resume PCs remain live RAM PCs.
    constexpr uint32_t kFragmentBytes = 64u;
    constexpr uint32_t kCanonicalBase = 0x03000000u;
    for (std::size_t index=0;index<functions.size();++index) {
        if (!selected[index]) continue;
        const auto& original=functions[index];
        std::vector<FunctionBlock> spans = original.switch_blocks;
        spans.push_back({original.addr, original.end_addr});
        std::sort(spans.begin(), spans.end(), [](const auto& a, const auto& b) {
            return a.start < b.start;
        });
        const uint32_t source = original.source_addr
            ? original.source_addr : original.addr;
        const uint32_t step = original.mode == CpuMode::Thumb ? 2u : 4u;
        for (const auto& span : spans) {
            for (uint32_t pc = span.start; pc < span.end;) {
                const uint32_t length = std::min(kFragmentBytes, span.end - pc);
                const int64_t source_pc = int64_t(source) + int64_t(pc) - original.addr;
                if (length % step != 0 || source_pc < image_base ||
                    uint64_t(source_pc - image_base) + length > image_size) break;
                const auto offset = std::size_t(source_pc - image_base);
                for (unsigned initialized=0;initialized<2;++initialized) {
                std::vector<uint8_t> bytes(image + offset, image + offset + length);
                bool patched=false;
                if (initialized && original.mode==CpuMode::Arm) {
                    for (const auto& patch : patches) {
                        for (uint32_t n=0;n+4u<=length;n+=4u) {
                            const uint64_t at=uint64_t(source_pc)+n;
                            if (at<patch.begin || at+4u>patch.end) continue;
                            uint32_t raw=0;
                            for (uint32_t b=0;b<4u;++b) raw |= uint32_t(bytes[n+b])<<(8u*b);
                            if (raw!=patch.marker) continue;
                            for (uint32_t b=0;b<4u;++b) bytes[n+b]=uint8_t(patch.replacement>>(8u*b));
                            patched=true;
                        }
                    }
                }
                if (initialized && !patched) continue;
                if (seen.emplace(original.mode, bytes).second) {
                    Function fn{};
                    fn.addr = kCanonicalBase;
                    fn.source_addr = uint32_t(source_pc);
                    fn.mode = original.mode;
                    fn.end_addr = fn.walk_end_addr = fn.addr + length;
                    char name[96];
                    std::snprintf(name, sizeof(name), "%sram_aot_%zu",
                                  prefix.c_str(), result.size());
                    fn.name = name;
                    for (uint32_t n = step; n < length; n += step)
                        fn.alias_entries.push_back(fn.addr + n);
                    RamCodeTemplate t{std::move(fn),std::move(bytes),{}, {}};
                    if (original.mode==CpuMode::Arm) {
                        std::vector<uint8_t> masks(length,0xFFu);
                        bool parameters=false;
                        for (uint32_t n=0;n+4u<=length;n+=4u) {
                            uint32_t raw=0;
                            for (uint32_t b=0;b<4u;++b) raw |= uint32_t(t.bytes[n+b])<<(8u*b);
                            const auto ins=armv4t::ArmDecoder::decode(raw,t.function.addr+n);
                            if (armv4t::ArmCodegen::supports_runtime_arm_immediate(ins)) {
                                masks[n]=0; masks[n+1u]=0xF0u; parameters=true;
                            }
                        }
                        if (parameters) t.masks=std::move(masks);
                    }
                    if (matrix_cart) {
                        t.cart_function=t.function;
                        t.cart_function.addr=0x08000000u;
                        t.cart_function.end_addr=t.cart_function.walk_end_addr=
                            t.cart_function.addr+length;
                        t.cart_function.name+="_cart";
                        for (auto& alias : t.cart_function.alias_entries)
                            alias=t.cart_function.addr+(alias-t.function.addr);
                    }
                    result.push_back(std::move(t));
                }
                }
                pc += length;
            }
        }
    }
    return result;
}

}  // namespace gbarecomp
