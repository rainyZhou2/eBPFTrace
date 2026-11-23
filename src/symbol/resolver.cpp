#include "symbol/resolver.h"

#include <sys/stat.h>

#include <filesystem>
#include <format>
#include <fstream>

#include "platform/error.h"

namespace fs = std::filesystem;

namespace etrace::symbol {

namespace {

std::vector<Symbol> func_symbols(ElfFile& f, uint32_t type) {
	std::vector<Symbol> out;
	for (const auto& s : f.symbols(type)) {
		if ((s.info & 0xf) != kSttFunc || s.value == 0) {
			continue;
		}
		out.push_back(Symbol{s.value, s.size, std::string(s.name)});
	}
	return out;
}

std::string elf_key(const std::string& path) {
	struct stat st{};
	if (::stat(path.c_str(), &st) != 0) {
		return path;
	}
	int64_t mtime = int64_t{st.st_mtim.tv_sec} * 1000000000 + st.st_mtim.tv_nsec;
	return std::format("{}:{}:{}", path, st.st_size, mtime);
}

std::string join(const std::string& a, const std::string& b) {
	return (fs::path(a) / fs::path(b).relative_path()).string();
}

}  // namespace

std::optional<uint64_t> ElfInfo::file_offset_to_vaddr(uint64_t off) const {
	for (const auto& p : segments) {
		if (off >= p.offset && off < p.offset + p.filesz) {
			return off - p.offset + p.vaddr;
		}
	}
	return std::nullopt;
}

std::shared_ptr<ElfInfo> load_elf_info(ElfFile& f) {
	auto out = std::make_shared<ElfInfo>();
	auto syms = func_symbols(f, kShtSymtab);
	out->build_id = build_id(f);
	out->complete = !syms.empty();
	auto dyn = func_symbols(f, kShtDynsym);
	syms.insert(syms.end(), std::make_move_iterator(dyn.begin()), std::make_move_iterator(dyn.end()));
	out->table = std::make_shared<const Table>(std::move(syms));
	for (const auto& p : f.progs()) {
		if (p.type == kPtLoad) {
			out->segments.push_back(p);
		}
	}
	return out;
}

std::shared_ptr<ElfInfo> load_elf_info(const std::string& path) {
	auto f = ElfFile::open(path);
	return load_elf_info(*f);
}

std::shared_ptr<ElfInfo> with_debug(const ElfInfo& e, const std::string& path) {
	auto f = ElfFile::open(path);
	if (auto id = build_id(*f); id != e.build_id) {
		throw Error(std::format("debug file {} has build id \"{}\", want \"{}\"", path, id, e.build_id));
	}
	auto syms = func_symbols(*f, kShtSymtab);
	if (syms.empty()) {
		throw Error(std::format("debug file {} has no function symbols", path));
	}
	auto all = e.table->symbols();
	all.insert(all.end(), std::make_move_iterator(syms.begin()), std::make_move_iterator(syms.end()));
	auto out = std::make_shared<ElfInfo>();
	out->table = std::make_shared<const Table>(std::move(all));
	out->build_id = e.build_id;
	out->complete = true;
	out->segments = e.segments;
	return out;
}

ProcResolver::ProcResolver(Config cfg)
    : cfg_(std::move(cfg)),
      procs_(static_cast<size_t>(cfg_.process_cache), cfg_.process_ttl),
      elfs_(static_cast<size_t>(cfg_.elf_cache), Duration(0)),
      by_id_(static_cast<size_t>(cfg_.elf_cache), Duration(0)),
      last_by_path_(static_cast<size_t>(cfg_.elf_cache), Duration(0)) {}

Resolved ProcResolver::kernel(uint64_t addr) {
	std::call_once(kernel_once_, [this] {
		try {
			kernel_ = load_kallsyms(join(cfg_.proc_root, "kallsyms"));
		} catch (const Error&) {
			kernel_ = std::make_shared<const Table>();
		}
	});
	if (const auto* name = kernel_->lookup(addr)) {
		return {*name, "[kernel]"};
	}
	return {"", "[kernel]"};
}

std::shared_ptr<const std::vector<Mapping>> ProcResolver::load_maps(uint32_t pid) {
	std::ifstream in(join(cfg_.proc_root, std::format("{}/maps", pid)));
	if (!in) {
		return nullptr;
	}
	return std::make_shared<const std::vector<Mapping>>(parse_maps(in));
}

Resolved ProcResolver::user(uint32_t pid, uint64_t addr) {
	auto maps = procs_.get(pid).value_or(nullptr);
	if (!maps) {
		maps = load_maps(pid);
		if (!maps) {
			return {};
		}
		procs_.add(pid, maps);
	}
	const auto* m = find_mapping(*maps, addr);
	if (!m) {
		return {};
	}
	if (m->path.empty() || m->path[0] != '/') {
		return {"", m->path};
	}
	auto module = fs::path(m->path).filename().string();
	ElfPtr ef;
	try {
		ef = elf(pid, m->path);
	} catch (const Error&) {
		return {"", module};
	}
	auto vaddr = ef->file_offset_to_vaddr(addr - m->start + m->offset);
	if (!vaddr) {
		return {"", module};
	}
	const auto* name = ef->table->lookup(*vaddr);
	return {name ? *name : "", module};
}

ProcResolver::ElfPtr ProcResolver::elf(uint32_t pid, const std::string& path) {
	auto root = join(cfg_.proc_root, std::format("{}/root", pid));
	auto full = join(root, path);
	auto key = elf_key(full);
	auto ef = elfs_.get(key).value_or(nullptr);
	if (!ef) {
		try {
			ef = load(full);
		} catch (const Error&) {
			// The process may have exited between blocking and this batch;
			// its binary is then only reachable through what was loaded
			// while it ran.
			if (auto last = last_by_path_.get(full)) {
				return *last;
			}
			throw;
		}
	}
	if (!ef->complete && !ef->build_id.empty() && cfg_.clock() >= ef->retry_at) {
		ef = attach_debug(root, ef);
	}
	elfs_.add(key, ef);
	last_by_path_.add(full, ef);
	return ef;
}

ProcResolver::ElfPtr ProcResolver::load(const std::string& full) {
	auto f = ElfFile::open(full);
	auto id = build_id(*f);
	if (!id.empty()) {
		if (auto ef = by_id_.get(id)) {
			return *ef;
		}
	}
	auto ef = load_elf_info(*f);
	if (!ef->build_id.empty()) {
		by_id_.add(ef->build_id, ef);
	}
	return ef;
}

// attach_debug looks for the separate debug file of a stripped object, first
// in the distribution debug directory of the process root, then through the
// configured finder. Without one the object is retried after kDebugRetry.
ProcResolver::ElfPtr ProcResolver::attach_debug(const std::string& root, const ElfPtr& ef) {
	const auto& id = ef->build_id;
	std::vector<std::string> candidates{
	        join(root, std::format("usr/lib/debug/.build-id/{}/{}.debug", id.substr(0, 2), id.substr(2)))};
	if (cfg_.debug) {
		if (auto p = cfg_.debug->find(id)) {
			candidates.push_back(*p);
		}
	}
	for (const auto& p : candidates) {
		std::error_code ec;
		if (!fs::exists(p, ec)) {
			continue;
		}
		try {
			auto full = with_debug(*ef, p);
			by_id_.add(id, full);
			return full;
		} catch (const Error&) {
			continue;
		}
	}
	auto retry = std::make_shared<ElfInfo>(*ef);
	retry->retry_at = cfg_.clock() + kDebugRetry;
	by_id_.add(id, retry);
	return retry;
}

}  // namespace etrace::symbol
