#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "platform/lru.h"
#include "platform/time.h"
#include "symbol/elf.h"
#include "symbol/table.h"

namespace etrace::symbol {

// ElfInfo is what symbolization keeps of one object.
struct ElfInfo {
	std::shared_ptr<const Table> table;
	std::string build_id;
	// complete is false when only exported symbols are known, i.e. the
	// object is stripped and no separate debug file has been found yet.
	bool complete = false;
	std::vector<ElfProg> segments;
	std::chrono::steady_clock::time_point retry_at{};

	// file_offset_to_vaddr maps an offset within the file to the ELF
	// virtual address.
	std::optional<uint64_t> file_offset_to_vaddr(uint64_t off) const;
};

std::shared_ptr<ElfInfo> load_elf_info(ElfFile& f);
std::shared_ptr<ElfInfo> load_elf_info(const std::string& path);
// with_debug returns a copy of e that also knows the full symbol table of a
// separate debug file. The file must carry the same build ID, which also
// guards against corrupt or mismatched downloads.
std::shared_ptr<ElfInfo> with_debug(const ElfInfo& e, const std::string& path);

// DebugFinder locates a separate debug file by GNU build ID without
// blocking.
class DebugFinder {
public:
	virtual ~DebugFinder() = default;
	virtual std::optional<std::string> find(const std::string& build_id) = 0;
};

struct Resolved {
	std::string function;
	std::string module;
};

class Resolver {
public:
	virtual ~Resolver() = default;
	virtual Resolved kernel(uint64_t addr) = 0;
	virtual Resolved user(uint32_t pid, uint64_t addr) = 0;
};

// kDebugRetry is how long a stripped object waits before its separate
// debug file is looked for again; a download may be in progress meanwhile.
inline constexpr auto kDebugRetry = std::chrono::seconds(30);

struct Config {
	std::string proc_root = "/proc";
	int process_cache = 4096;
	Duration process_ttl = 1min;
	int elf_cache = 512;
	// debug, when set, supplies debug files for stripped objects that have
	// none under /usr/lib/debug/.build-id in their own root.
	DebugFinder* debug = nullptr;
	std::function<std::chrono::steady_clock::time_point()> clock = std::chrono::steady_clock::now;
};

// ProcResolver symbolizes using /proc and on-disk ELF symbol tables. User
// addresses are resolved through the process namespace root so binaries
// inside containers are found without host path translation. Parsed objects
// are shared by GNU build ID, so one library mapped by many containers is
// loaded once.
class ProcResolver : public Resolver {
public:
	explicit ProcResolver(Config cfg);

	Resolved kernel(uint64_t addr) override;
	Resolved user(uint32_t pid, uint64_t addr) override;

private:
	using ElfPtr = std::shared_ptr<ElfInfo>;
	std::shared_ptr<const std::vector<Mapping>> load_maps(uint32_t pid);
	ElfPtr elf(uint32_t pid, const std::string& path);
	ElfPtr load(const std::string& full);
	ElfPtr attach_debug(const std::string& root, const ElfPtr& ef);

	Config cfg_;
	std::once_flag kernel_once_;
	std::shared_ptr<const Table> kernel_;
	LruCache<uint32_t, std::shared_ptr<const std::vector<Mapping>>> procs_;
	LruCache<std::string, ElfPtr> elfs_;
	LruCache<std::string, ElfPtr> by_id_;
	// last_by_path keeps the object last loaded for /proc/<pid>/root/<path>.
	LruCache<std::string, ElfPtr> last_by_path_;
};

}  // namespace etrace::symbol
