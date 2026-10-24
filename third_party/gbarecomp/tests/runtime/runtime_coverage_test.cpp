#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include "gba_bus.h"
#include "runtime_arm.h"
#include "runtime_bus_bridge.h"
#include "self_heal.h"

struct DispatchEntry { uint32_t addr; uint8_t thumb,resume; void (*fn)(void); };
extern "C" const DispatchEntry kDispatchTable[]={{0xFFFFFFFFu,0,0,nullptr}};
extern "C" const unsigned kDispatchTableLen=0;

int main(int argc,char** argv) {
    const bool strict=argc>1;
#ifdef _WIN32
    _putenv_s("GBARECOMP_STRICT_STATIC",strict ? "1" : "0");
#else
    setenv("GBARECOMP_STRICT_STATIC",strict ? "1" : "0",1);
#endif
    auto bus=std::make_unique<gba::GbaBus>();
    gbarecomp::set_active_bus(bus.get());
    runtime_init(bus.get());
    gbarecomp::self_heal_reset();
    g_cpu={};
    g_cpu.cpsr=CPSR_T_BIT | 0x1Fu;
    g_cpu.R[15]=0x02000000u;
    bus->write16(g_cpu.R[15],0x2001u); // mov r0,#1
    if (strict && std::strcmp(argv[1],"--strict-bridge")==0) {
        runtime_bridge_interpret(g_cpu.R[15],true,g_cpu.R[15]+2u,8);
        return 7; // strict rejection must terminate before interpretation
    }
    runtime_force_interp_step();
    if (strict) return 7;
    auto valid=[&]() {
        return g_cpu.R[0]==1 && g_cpu.R[15]==0x02000002u &&
            gbarecomp::self_heal_distinct_misses()==0 &&
            gbarecomp::self_heal_interpreted_insns()==1 &&
            gbarecomp::self_heal_misses_json().find("\"coverage\":\"NOT_STATIC\"")!=std::string::npos;
    };
    bool ok=valid();
    gbarecomp::self_heal_write_report(nullptr);
    gbarecomp::self_heal_reset();
    g_cpu.R[0]=0; g_cpu.R[15]=0x02000000u;
    ok &= runtime_bridge_interpret(g_cpu.R[15],true,g_cpu.R[15]+2u,8)!=0;
    ok &= valid(); // explicit bridge calls need not create a dispatch miss
    gbarecomp::self_heal_reset();
    ok &= gbarecomp::self_heal_misses_json().find("\"coverage\":\"FULLY_STATIC\"")!=std::string::npos;
    runtime_shutdown();
    gbarecomp::set_active_bus(nullptr);
    if (!ok) std::fprintf(stderr,"Forced/explicit interpreter coverage was not counted honestly\n");
    return ok ? 0 : 1;
}
