#include <bpf/btf.h>
#include <gtest/gtest.h>

#include <map>

#include "agent/source/offcpu/decode.h"
#include "agent/source/offcpu/unwinder.h"

namespace etrace {
namespace {

using namespace agent::offcpu;

size_t btf_size(const btf* b, const char* name) {
	int id = btf__find_by_name_kind(b, name, BTF_KIND_STRUCT);
	if (id < 0) {
		ADD_FAILURE() << name << " not in BTF";
		return 0;
	}
	return btf__type_by_id(b, static_cast<__u32>(id))->size;
}

// The raw structs are copied byte for byte out of BPF maps, so their sizes
// must match the program's BTF.
TEST(Layout, MatchesBPF) {
	btf* b = btf__parse(ETRACE_BPF_FP_OBJECT, nullptr);
	ASSERT_NE(b, nullptr) << "bpf object not built";
	std::map<const char*, size_t> sizes{{"hop", sizeof(RawHop)},
	                                    {"chain", sizeof(RawChain)},
	                                    {"offcpu_key", sizeof(RawKey)},
	                                    {"offcpu_val", sizeof(RawVal)},
	                                    {"stack", sizeof(RawStack)},
	                                    {"start_rec", sizeof(RawStart)},
	                                    {"handle_key", sizeof(RawHandleKey)},
	                                    {"kernel_name", sizeof(RawKernelName)},
	                                    {"gpu_wait", sizeof(RawGpuWait)},
	                                    {"launch", sizeof(RawLaunch)}};
	for (const auto& [name, size] : sizes) {
		EXPECT_EQ(btf_size(b, name), size) << name;
	}
	// struct event only reaches BTF through a ring buffer reservation, so
	// it is checked as its fixed header plus the chain.
	EXPECT_EQ(sizeof(RawEvent), 112 + btf_size(b, "chain"));
	btf__free(b);

	b = btf__parse(ETRACE_BPF_DWARF_OBJECT, nullptr);
	ASSERT_NE(b, nullptr);
	EXPECT_EQ(btf_size(b, "unwind_row"), sizeof(unwinder::Row));
	EXPECT_EQ(btf_size(b, "unwind_mapping"), sizeof(unwinder::Mapping));
	EXPECT_EQ(btf_size(b, "proc_unwind"), sizeof(unwinder::Proc));
	btf__free(b);
}

}  // namespace
}  // namespace etrace
