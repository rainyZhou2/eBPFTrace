#include "agent/exporter/grpc.h"

#include <format>
#include <vector>

#include "wire/wire.h"

namespace etrace::agent::grpcexp {

namespace {

// Room for the batch's field tag and length prefix in PushRequest and for the
// "/<n>" suffix of a part id.
constexpr size_t kPartSlack = 32;
// Field tag and length prefix of one sample or event.
constexpr size_t kItemSlack = 6;

// split packs the samples and events of a batch into requests of at most
// limit bytes. Parts get ids derived from the batch id, so a retry after a
// partial failure resends the same parts and the collector deduplicates
// them. Items that do not fit in a request on their own are counted in
// dropped.
std::vector<etrace::v1::PushRequest> split(etrace::v1::Batch& full, size_t limit, size_t& dropped) {
	etrace::v1::Batch header;
	header.set_id(full.id());
	*header.mutable_resource() = full.resource();
	header.set_window_start_unix_nano(full.window_start_unix_nano());
	header.set_window_end_unix_nano(full.window_end_unix_nano());
	size_t base = header.ByteSizeLong() + kPartSlack;

	std::vector<etrace::v1::PushRequest> parts;
	etrace::v1::PushRequest cur;
	size_t used = 0;
	auto flush = [&] {
		if (used == 0) {
			return;
		}
		cur.mutable_batch()->set_id(std::format("{}/{}", full.id(), parts.size()));
		parts.push_back(std::move(cur));
		cur = {};
		used = 0;
	};
	auto add = [&](auto& item, auto append) {
		size_t n = item.ByteSizeLong() + kItemSlack;
		if (base + n > limit) {
			dropped++;
			return;
		}
		if (base + used + n > limit) {
			flush();
		}
		if (used == 0) {
			*cur.mutable_batch() = header;
		}
		append(cur.mutable_batch())->Swap(&item);
		used += n;
	};
	for (auto& s : *full.mutable_offcpu()) {
		add(s, [](etrace::v1::Batch* p) { return p->add_offcpu(); });
	}
	for (auto& e : *full.mutable_events()) {
		add(e, [](etrace::v1::Batch* p) { return p->add_events(); });
	}
	flush();
	return parts;
}

}  // namespace

void Options::validate() const {
	grpcx::ClientConfig::validate();
	if (timeout <= Duration(0)) {
		throw Error("timeout must be positive");
	}
}

void decode_fields(config::Fields& f, Options& o) {
	grpcx::decode_fields(f, static_cast<grpcx::ClientConfig&>(o));
	f("timeout", o.timeout);
}

Exporter::Exporter(Options o) : opts_(std::move(o)) {
	try {
		channel_ = grpcx::dial(opts_);
	} catch (const Error& e) {
		throw Error(std::format("dial collector: {}", e.what()));
	}
	stub_ = etrace::v1::IngestService::NewStub(channel_);
}

void Exporter::export_batch(std::stop_token st, const model::Batch& b) {
	etrace::v1::PushRequest req;
	wire::to_proto(b, *req.mutable_batch());
	auto limit = static_cast<size_t>(opts_.max_send_msg_bytes);
	if (req.ByteSizeLong() <= limit) {
		push(st, req);
		return;
	}
	size_t dropped = 0;
	for (const auto& part : split(*req.mutable_batch(), limit, dropped)) {
		push(st, part);
	}
	if (dropped) {
		throw PermanentError(
		        std::format("{} items of batch {} exceed max_send_msg_bytes {} on their own and were dropped",
		                    dropped, b.id, limit));
	}
}

void Exporter::push(std::stop_token st, const etrace::v1::PushRequest& req) {
	etrace::v1::PushResponse resp;
	grpc::ClientContext ctx;
	ctx.set_deadline(std::chrono::system_clock::now() + opts_.timeout);
	if (!opts_.token.empty()) {
		ctx.AddMetadata("authorization", "Bearer " + opts_.token);
	}
	std::stop_callback cancel(st, [&ctx] { ctx.TryCancel(); });
	auto status = stub_->Push(&ctx, req, &resp);
	if (status.ok()) {
		return;
	}
	auto msg = std::format("rpc error: code = {} desc = {}", static_cast<int>(status.error_code()),
	                       status.error_message());
	switch (status.error_code()) {
		case grpc::StatusCode::INVALID_ARGUMENT:
		case grpc::StatusCode::UNIMPLEMENTED:
			throw PermanentError(msg);
		default:
			throw Error(msg);
	}
}

}  // namespace etrace::agent::grpcexp
