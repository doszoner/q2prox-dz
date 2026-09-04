#pragma once

#include "multiroom_state.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace q2dz {

struct AliasQueryResult {
    TargetId target_id = 0;
    std::uint64_t resolution_token = 0;
    std::string alias;
    std::vector<std::pair<std::string, RemotePeerId>> matches;
};

struct AliasQueryBatch {
    std::deque<AliasQueryResult> results;
    bool overflowed = false;
};

class AliasQueryMailbox {
public:
    static constexpr std::size_t max_results = MultiroomState::max_targets;

    [[nodiscard]] bool push(AliasQueryResult result) {
        std::lock_guard lock(mutex_);
        if (results_.size() >= max_results) {
            overflowed_ = true;
            return false;
        }
        results_.push_back(std::move(result));
        return true;
    }

    [[nodiscard]] AliasQueryBatch drain() {
        std::lock_guard lock(mutex_);
        AliasQueryBatch batch;
        batch.results.swap(results_);
        batch.overflowed = overflowed_;
        overflowed_ = false;
        return batch;
    }

    [[nodiscard]] std::size_t size() const {
        std::lock_guard lock(mutex_);
        return results_.size();
    }

private:
    mutable std::mutex mutex_;
    std::deque<AliasQueryResult> results_;
    bool overflowed_ = false;
};

}  // namespace q2dz
