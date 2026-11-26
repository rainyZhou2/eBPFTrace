#include "agent/source/offcpu/bpf_object.h"

#include <bpf/btf.h>
#include <sys/resource.h>

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <format>
#include <mutex>

namespace etrace::agent::offcpu {

namespace {

thread_local std::string* g_capture = nullptr;

int print_libbpf(enum libbpf_print_level level, const char* fmt, va_list args) {
	if (!g_capture || level == LIBBPF_DEBUG) {
		return 0;
	}
	char buf[1024];
	int n = std::vsnprintf(buf, sizeof buf, fmt, args);
	if (n > 0) {
		g_capture->append(buf, std::min<size_t>(static_cast<size_t>(n), sizeof buf - 1));
		constexpr size_t kKeep = 16 << 10;
		if (g_capture->size() > 2 * kKeep) {
			g_capture->erase(0, g_capture->size() - kKeep);
		}
	}
	return n;
}

void install_printer() {
	static std::once_flag once;
	std::call_once(once, [] { libbpf_set_print(print_libbpf); });
}

std::string errstr(int err) {
	return std::strerror(err < 0 ? -err : err);
}

std::string tail(const std::string& s, size_t n) {
	return s.size() <= n ? s : s.substr(s.size() - n);
}

}  // namespace

void raise_memlock() {
	rlimit rl{RLIM_INFINITY, RLIM_INFINITY};
	if (setrlimit(RLIMIT_MEMLOCK, &rl) != 0) {
		throw Error(std::format("remove memlock: {}", std::strerror(errno)));
	}
}

BpfObject::BpfObject(std::string_view elf, const std::string& name) {
	install_printer();
	std::string log;
	g_capture = &log;
	bpf_object_open_opts opts{};
	opts.sz = sizeof(opts);
	opts.object_name = name.c_str();
	obj_ = bpf_object__open_mem(elf.data(), elf.size(), &opts);
	g_capture = nullptr;
	if (!obj_) {
		throw Error(std::format("load spec {}: {}: {}", name, errstr(errno), tail(log, 2048)));
	}
}

BpfObject::~BpfObject() {
	if (obj_) {
		bpf_object__close(obj_);
	}
}

BpfObject::RuntimeStats BpfObject::runtime_stats() const {
	RuntimeStats stats;
	bpf_program* program;
	bpf_object__for_each_program(program, obj_) {
		int fd = bpf_program__fd(program);
		if (fd < 0) {
			continue;
		}
		bpf_prog_info info{};
		__u32 len = sizeof(info);
		if (bpf_obj_get_info_by_fd(fd, &info, &len) == 0) {
			stats.run_time_ns += info.run_time_ns;
			stats.run_cnt += info.run_cnt;
		}
	}
	return stats;
}

bpf_map* BpfObject::map(const std::string& name) const {
	auto* m = bpf_object__find_map_by_name(obj_, name.c_str());
	if (!m) {
		throw Error(std::format("bpf map \"{}\" not found", name));
	}
	return m;
}

bpf_program* BpfObject::program(const std::string& name) const {
	auto* p = bpf_object__find_program_by_name(obj_, name.c_str());
	if (!p) {
		throw Error(std::format("bpf program \"{}\" not found", name));
	}
	return p;
}

void BpfObject::set_max_entries(const std::string& name, uint32_t n) {
	if (int err = bpf_map__set_max_entries(map(name), n); err != 0) {
		throw Error(std::format("size map {}: {}", name, errstr(err)));
	}
}

void BpfObject::load() {
	std::string log;
	g_capture = &log;
	int err = bpf_object__load(obj_);
	g_capture = nullptr;
	if (err != 0) {
		throw Error(std::format("load collection {}: {}\n{}", bpf_object__name(obj_), errstr(err),
		                        tail(log, 4096)));
	}
}

void* BpfObject::var_ptr(const std::string& section, const std::string& var, size_t size) {
	const btf* b = bpf_object__btf(obj_);
	if (!b) {
		throw Error("bpf object has no BTF");
	}
	int id = btf__find_by_name_kind(b, section.c_str(), BTF_KIND_DATASEC);
	if (id < 0) {
		throw Error(std::format("bpf section {} not found", section));
	}
	const btf_type* sec = btf__type_by_id(b, static_cast<__u32>(id));
	const btf_var_secinfo* vs = btf_var_secinfos(sec);
	for (int i = 0; i < btf_vlen(sec); i++) {
		const btf_type* v = btf__type_by_id(b, vs[i].type);
		if (var != btf__name_by_offset(b, v->name_off)) {
			continue;
		}
		if (vs[i].size != size) {
			throw Error(std::format("bpf variable {} has size {}, want {}", var, vs[i].size, size));
		}
		size_t sz = 0;
		auto* data = static_cast<char*>(bpf_map__initial_value(map(section), &sz));
		if (!data || vs[i].offset + size > sz) {
			throw Error(std::format("bpf variable {} is not addressable", var));
		}
		return data + vs[i].offset;
	}
	throw Error(std::format("bpf variable \"{}\" not found", var));
}

std::unique_ptr<Link> BpfObject::attach(const std::string& prog) {
	auto* l = bpf_program__attach(program(prog));
	if (!l) {
		throw Error(std::format("attach {}: {}", prog, errstr(errno)));
	}
	return std::make_unique<Link>(l);
}

std::unique_ptr<Link> BpfObject::attach_uprobe(const std::string& prog, const std::string& path,
                                               const std::string& symbol, bool retprobe, uint64_t cookie) {
	bpf_uprobe_opts opts{};
	opts.sz = sizeof(opts);
	opts.func_name = symbol.c_str();
	opts.retprobe = retprobe;
	opts.bpf_cookie = cookie;
	auto* l = bpf_program__attach_uprobe_opts(program(prog), -1, path.c_str(), 0, &opts);
	if (!l) {
		throw Error(std::format("uprobe {}:{}: {}", path, symbol, errstr(errno)));
	}
	return std::make_unique<Link>(l);
}

void BpfObject::set_autoload(const std::string& prog, bool on) {
	if (int err = bpf_program__set_autoload(program(prog), on); err != 0) {
		throw Error(std::format("set autoload {}: {}", prog, errstr(-err)));
	}
}

bool attach_cookies_supported() {
	return libbpf_probe_bpf_helper(BPF_PROG_TYPE_KPROBE, BPF_FUNC_get_attach_cookie, nullptr) == 1;
}

}  // namespace etrace::agent::offcpu
