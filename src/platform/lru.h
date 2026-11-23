#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <list>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>

#include "platform/time.h"

namespace etrace {

// LruCache is a thread-safe, size-bounded cache with an optional TTL
// (zero disables expiry).
template <class K, class V, class Hash = std::hash<K>>
class LruCache {
public:
	using Clock = std::function<std::chrono::steady_clock::time_point()>;

	LruCache(size_t capacity, Duration ttl, Clock clock = std::chrono::steady_clock::now)
	    : cap_(capacity == 0 ? 1 : capacity), ttl_(ttl), now_(std::move(clock)) {}

	std::optional<V> get(const K& k) {
		std::lock_guard lock(mu_);
		auto it = items_.find(k);
		if (it == items_.end()) {
			return std::nullopt;
		}
		if (expired(*it->second)) {
			remove_locked(it);
			return std::nullopt;
		}
		ll_.splice(ll_.begin(), ll_, it->second);
		return it->second->value;
	}

	void add(const K& k, V v) {
		std::lock_guard lock(mu_);
		add_locked(k, std::move(v));
	}

	// add_if_absent stores the value only when the key is missing or
	// expired and reports whether it did.
	bool add_if_absent(const K& k, V v) {
		std::lock_guard lock(mu_);
		auto it = items_.find(k);
		if (it != items_.end()) {
			if (!expired(*it->second)) {
				return false;
			}
			remove_locked(it);
		}
		add_locked(k, std::move(v));
		return true;
	}

	void remove(const K& k) {
		std::lock_guard lock(mu_);
		auto it = items_.find(k);
		if (it != items_.end()) {
			remove_locked(it);
		}
	}

	size_t size() const {
		std::lock_guard lock(mu_);
		return ll_.size();
	}

private:
	struct Entry {
		K key;
		V value;
		std::chrono::steady_clock::time_point expires;
	};
	using List = std::list<Entry>;
	using Map = std::unordered_map<K, typename List::iterator, Hash>;

	bool expired(const Entry& e) const { return ttl_.count() > 0 && now_() > e.expires; }

	void add_locked(const K& k, V v) {
		auto exp = now_() + ttl_;
		if (auto it = items_.find(k); it != items_.end()) {
			it->second->value = std::move(v);
			it->second->expires = exp;
			ll_.splice(ll_.begin(), ll_, it->second);
			return;
		}
		ll_.push_front(Entry{k, std::move(v), exp});
		items_.emplace(k, ll_.begin());
		while (ll_.size() > cap_) {
			items_.erase(ll_.back().key);
			ll_.pop_back();
		}
	}

	void remove_locked(typename Map::iterator it) {
		ll_.erase(it->second);
		items_.erase(it);
	}

	mutable std::mutex mu_;
	size_t cap_;
	Duration ttl_;
	Clock now_;
	List ll_;
	Map items_;
};

}  // namespace etrace
