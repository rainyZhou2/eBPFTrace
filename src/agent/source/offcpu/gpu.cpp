#include "agent/source/offcpu/gpu.h"

#include <array>
#include <set>

#include "symbol/elf.h"

namespace etrace::agent::offcpu::gpu {

namespace {

// kApis lists the CUDA driver and runtime calls that block the calling
// thread until GPU work completes. Each index + 1 is a BPF attach cookie.
constexpr std::array<std::string_view, 16> kApis = {
        "cuStreamSynchronize",
        "cuEventSynchronize",
        "cuCtxSynchronize",
        "cuMemcpy",
        "cuMemcpyDtoH",
        "cuMemcpyHtoD",
        "cuMemFree",
        "cuMemFreeHost",
        "cuMemHostAlloc",
        "cudaStreamSynchronize",
        "cudaEventSynchronize",
        "cudaDeviceSynchronize",
        "cudaMemcpy",
        "cudaFree",
        "cudaFreeHost",
        "cudaHostAlloc",
};

// What argument 0 of a sync call is, as in the BPF program.
constexpr uint64_t kArgStream = 1;
constexpr uint64_t kArgEvent = 2;

constexpr std::array<std::string_view, 7> kNcclOps = {
        "ncclAllReduce", "ncclBroadcast", "ncclReduce", "ncclAllGather", "ncclReduceScatter", "ncclSend", "ncclRecv",
};

// Launch calls and their argument layout cookies, as in the BPF program.
struct Launch {
	std::string_view name;
	uint64_t layout;
};
constexpr std::array<Launch, 6> kLaunches = {{
        {"cuLaunchKernel", 1},
        {"cuLaunchKernelEx", 2},
        {"cudaLaunchKernel", 3},
        {"cudaLaunchKernelExC", 4},
        {"cuGraphLaunch", 5},
        {"cudaGraphLaunch", 6},
}};

// Runtime launch layouts: these calls can call the driver themselves, so
// they also get a uretprobe that closes the launch scope.
constexpr uint64_t kLayoutCuda = 3;
constexpr uint64_t kLayoutCudaEx = 4;
constexpr uint64_t kLayoutCudaGraph = 6;

constexpr std::array<std::string_view, 4> kEventRecords = {"cuEventRecord", "cuEventRecordWithFlags", "cudaEventRecord",
                                                           "cudaEventRecordWithFlags"};
constexpr std::array<std::string_view, 2> kFnLookups = {"cuModuleGetFunction", "cuLibraryGetKernel"};

// kVariants are the suffixes of versioned and per-thread default stream
// entry points exported next to a base call.
constexpr std::array<std::string_view, 7> kVariants = {"", "_v2", "_v3", "_ptsz", "_ptds", "_v2_ptsz", "_v2_ptds"};

bool same_call(std::string_view base, std::string_view sym, bool variants) {
	if (!sym.starts_with(base)) {
		return false;
	}
	auto rest = sym.substr(base.size());
	if (rest.empty()) {
		return true;
	}
	if (!variants) {
		return false;
	}
	for (auto v : kVariants) {
		if (rest == v) {
			return true;
		}
	}
	return false;
}

// index returns the 1-based index of the call sym is a variant of, or 0.
template <size_t N>
uint8_t index(const std::array<std::string_view, N>& names, std::string_view sym, bool variants) {
	for (size_t i = 0; i < names.size(); i++) {
		if (same_call(names[i], sym, variants)) {
			return static_cast<uint8_t>(i + 1);
		}
	}
	return 0;
}

template <size_t N>
std::string_view name_of(const std::array<std::string_view, N>& names, uint8_t cookie) {
	return cookie >= 1 && cookie <= names.size() ? names[cookie - 1] : std::string_view();
}

}  // namespace

std::string_view api_name(uint8_t cookie) {
	return name_of(kApis, cookie);
}

std::string_view nccl_name(uint8_t cookie) {
	return name_of(kNcclOps, cookie);
}

uint64_t match_api(std::string_view sym) {
	uint8_t i = index(kApis, sym, true);
	if (i == 0) {
		return 0;
	}
	auto name = kApis[i - 1];
	uint64_t arg = 0;
	if (name.ends_with("StreamSynchronize")) {
		arg = kArgStream;
	} else if (name.ends_with("EventSynchronize")) {
		arg = kArgEvent;
	}
	return i | arg << 8;
}

uint8_t match_nccl(std::string_view sym) {
	return index(kNcclOps, sym, false);
}

std::vector<uprobes::Probe> probes(const std::string& path) {
	std::vector<uprobes::Probe> out;
	std::set<uint64_t> seen;
	auto elf = symbol::ElfFile::open(path);
	for (const auto& s : elf->symbols(symbol::kShtDynsym)) {
		if (s.value == 0 || (s.info & 0xf) != symbol::kSttFunc || seen.contains(s.value)) {
			continue;
		}
		std::string name(s.name);
		if (auto api = match_api(name)) {
			out.push_back({kEnterProg, name, false, api});
			out.push_back({kExitProg, name, true, api});
		} else if (auto op = match_nccl(name)) {
			out.push_back({kNcclProg, name, false, op});
		} else {
			continue;
		}
		seen.insert(s.value);
	}
	return out;
}

std::vector<uprobes::Probe> launch_probes(const std::string& path) {
	std::vector<uprobes::Probe> out;
	std::set<uint64_t> seen;
	auto elf = symbol::ElfFile::open(path);
	for (const auto& s : elf->symbols(symbol::kShtDynsym)) {
		if (s.value == 0 || (s.info & 0xf) != symbol::kSttFunc || seen.contains(s.value)) {
			continue;
		}
		std::string name(s.name);
		size_t before = out.size();
		for (const auto& l : kLaunches) {
			if (same_call(l.name, name, true)) {
				out.push_back({kLaunchProg, name, false, l.layout});
				if (l.layout == kLayoutCuda || l.layout == kLayoutCudaEx ||
				    l.layout == kLayoutCudaGraph) {
					out.push_back({kLaunchRetProg, name, true, 0});
				}
				break;
			}
		}
		if (index(kEventRecords, name, true)) {
			out.push_back({kEventRecordProg, name, false, 0});
		} else if (index(kFnLookups, name, true)) {
			out.push_back({kFnEnterProg, name, false, 0});
			out.push_back({kFnExitProg, name, true, 0});
		}
		if (out.size() != before) {
			seen.insert(s.value);
		}
	}
	return out;
}

model::Stack frames(const Wait& w, const std::string& kernel_name) {
	model::Stack out;
	if (auto op = nccl_name(w.nccl_op); !op.empty()) {
		out.push_back(model::Frame{0, "waiting on " + std::string(op), kGpuModule, model::FrameKind::unknown});
	}
	switch (w.kernel_kind) {
		case kKernelStub:
			out.push_back(model::Frame{w.kernel, "", "", model::FrameKind::gpu_kernel});
			break;
		case kKernelFunc:
			out.push_back(model::Frame{0, kernel_name.empty() ? "[unknown]" : kernel_name,
			                           model::kGpuKernelModule, model::FrameKind::gpu_kernel});
			break;
		case kKernelGraph:
			out.push_back(
			        model::Frame{0, "CUDA graph", model::kGpuKernelModule, model::FrameKind::gpu_kernel});
			break;
		default:
			break;
	}
	auto name = api_name(w.api);
	out.push_back(
	        model::Frame{0, name.empty() ? "unknown" : std::string(name), kGpuModule, model::FrameKind::unknown});
	return out;
}

}  // namespace etrace::agent::offcpu::gpu
