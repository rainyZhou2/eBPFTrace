#pragma once

#include <memory>

#include "agent/plugin.h"
#include "symbol/debuginfod.h"
#include "symbol/resolver.h"

namespace etrace::agent::symbolize {

struct Options {
	int process_cache = 4096;
	Duration process_ttl = 1min;
	int elf_cache = 512;
	// debuginfod is disabled unless urls are set: it sends build IDs of the
	// node's binaries to the listed servers.
	symbol::DebuginfodConfig debuginfod;

	void validate() const { debuginfod.validate(); }
};

void decode_fields(config::Fields& f, Options& o);

// Processor resolves frame addresses to function and module names and
// drops the frames of the tracing machinery that captured the stack.
class Processor : public agent::Processor {
public:
	explicit Processor(std::unique_ptr<symbol::Resolver> r, std::unique_ptr<symbol::Debuginfod> d = nullptr);
	// from_options builds the resolver and, when debuginfod servers are
	// configured, its download workers; destroying the processor stops them.
	static std::unique_ptr<Processor> from_options(const Options& o, const Deps& deps);

	std::string name() const override { return "symbolize"; }
	void process(model::Batch& b) override;

private:
	void resolve(uint32_t pid, model::Stack& st);
	static void label_gpu_interrupt(model::Hop& h);

	std::unique_ptr<symbol::Debuginfod> debuginfod_;
	std::unique_ptr<symbol::Resolver> r_;
};

}  // namespace etrace::agent::symbolize
