#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "agent/source/offcpu/uprobes.h"
#include "model/model.h"

namespace etrace::agent::offcpu::gpu {

// The BPF programs that time GPU waits, mark NCCL collectives and track
// kernel launches.
inline constexpr const char* kEnterProg = "on_gpu_enter";
inline constexpr const char* kExitProg = "on_gpu_exit";
inline constexpr const char* kNcclProg = "on_nccl_enter";
inline constexpr const char* kLaunchProg = "on_gpu_launch";
inline constexpr const char* kEventRecordProg = "on_gpu_event_record";
inline constexpr const char* kFnEnterProg = "on_gpu_fn_enter";
inline constexpr const char* kFnExitProg = "on_gpu_fn_exit";

// Kernel identifier kinds, as in the BPF program.
inline constexpr uint8_t kKernelStub = 1;
inline constexpr uint8_t kKernelFunc = 2;
inline constexpr uint8_t kKernelGraph = 3;

// kGpuModule is the module of synthetic GPU frames.
inline constexpr const char* kGpuModule = "[gpu]";

// Wait is what the BPF program recorded about a GPU wait: the CUDA call,
// the NCCL collective it was queued behind, and the last kernel queued
// before it.
struct Wait {
	uint8_t api = 0;
	uint8_t nccl_op = 0;
	uint8_t kernel_kind = 0;
	uint64_t kernel = 0;
};

// api_name returns the CUDA call an attach cookie stands for, or "" for an
// unknown cookie. Cookies are 1-based indexes into a fixed list.
std::string_view api_name(uint8_t cookie);
std::string_view nccl_name(uint8_t cookie);

// match_api returns the cookie of a host-blocking CUDA call exported under
// sym, or 0. Versioned and per-thread-stream variants (_v2, _ptsz, _ptds)
// map to the base call. The cookie's second byte says whether argument 0 is
// the stream or event waited on.
uint64_t match_api(std::string_view sym);
uint8_t match_nccl(std::string_view sym);

// probes lists the GPU wait probes for the dynamic symbols of the ELF file
// at path; symbols that alias one address are probed once.
std::vector<uprobes::Probe> probes(const std::string& path);
// launch_probes lists the launch tracking probes: kernel and graph launches,
// event records, and driver function lookups.
std::vector<uprobes::Probe> launch_probes(const std::string& path);

// frames returns the synthetic innermost frames that name a GPU wait: the
// CUDA call, before it the kernel queued last, and before that the NCCL
// collective. kernel_name names a driver function handle.
model::Stack frames(const Wait& w, const std::string& kernel_name);

}  // namespace etrace::agent::offcpu::gpu
