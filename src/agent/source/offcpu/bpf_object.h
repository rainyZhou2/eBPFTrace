#pragma once

#include <bpf/bpf.h>
#include <bpf/libbpf.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>

#include "platform/error.h"

namespace etrace::agent::offcpu {

// Link owns an attached BPF program.
class Link {
public:
	explicit Link(bpf_link* l) : l_(l) {}
	~Link() {
		if (l_) {
			bpf_link__destroy(l_);
		}
	}
	Link(const Link&) = delete;
	Link& operator=(const Link&) = delete;

private:
	bpf_link* l_;
};

// BpfObject owns a libbpf object opened from an in-memory ELF file. Global
// variables are located through the object's BTF so the loader does not
// depend on generated skeletons.
class BpfObject {
public:
	BpfObject(std::string_view elf, const std::string& name);
	~BpfObject();
	BpfObject(const BpfObject&) = delete;
	BpfObject& operator=(const BpfObject&) = delete;

	// set_const writes a `const volatile` variable; only valid before load.
	template <class T>
	void set_const(const std::string& var, T value) {
		std::memcpy(var_ptr(".rodata", var, sizeof(T)), &value, sizeof(T));
	}
	void set_max_entries(const std::string& map, uint32_t n);
	// set_autoload excludes a program from load when on is false; only
	// valid before load.
	void set_autoload(const std::string& prog, bool on);
	// load verifies and loads every program; the exception carries the
	// tail of the verifier log.
	void load();

	// runtime_stats sums run_time_ns and run_cnt across loaded programs.
	// Both are zero when kernel.bpf_stats_enabled is off.
	struct RuntimeStats {
		uint64_t run_time_ns = 0;
		uint64_t run_cnt = 0;
	};
	RuntimeStats runtime_stats() const;

	bpf_map* map(const std::string& name) const;
	int map_fd(const std::string& name) const { return bpf_map__fd(map(name)); }
	bpf_program* program(const std::string& name) const;
	std::unique_ptr<Link> attach(const std::string& prog);
	// attach_uprobe attaches prog to symbol in the file at path for every
	// process that maps it.
	std::unique_ptr<Link> attach_uprobe(const std::string& prog, const std::string& path, const std::string& symbol,
	                                    bool retprobe = false, uint64_t cookie = 0);

private:
	void* var_ptr(const std::string& section, const std::string& var, size_t size);

	bpf_object* obj_ = nullptr;
};

// raise_memlock lifts RLIMIT_MEMLOCK for kernels that still charge BPF maps
// against it.
void raise_memlock();

// attach_cookies_supported reports whether the kernel passes attach cookies
// to uprobe programs (Linux 5.15).
bool attach_cookies_supported();

}  // namespace etrace::agent::offcpu
