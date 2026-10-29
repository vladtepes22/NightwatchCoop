// SPDX-License-Identifier: GPL-3.0-or-later
// 0.7.7 (sharing077, CL-806406): the notice "<item>: shared with the party" on every member's screen when a
// shared item (key / unique item, weapon, rune) is given. Pure rate limit and text (tests/test_party_items.cpp);
// the producers and the tick are in party_items.cpp, the player's switch is bbport.ini party_item_notices
// (System > Party members, launcher My Party tab) and the notice only shows while the party notices are on.
#pragma once

#include <cstddef>
#include <deque>
#include <string>

namespace coop::itemnotice {

constexpr double kGapS = 2.5;       ///< at least this long between two notices on the screen
constexpr double kSameS = 20.0;     ///< the same text is not shown again within this time
constexpr std::size_t kMaxQueued = 4;  ///< more waiting than this: the newest are dropped (the log has them)

inline std::string Text(const std::string& item) {
    return (item.empty() ? std::string("Item") : item) + ": shared with the party";
}

class Queue {
public:
    /// Offer a notice; false when dropped (duplicate within kSameS, or the queue is full).
    bool Push(const std::string& text, double now) {
        if (text == last_text_ && now - last_shown_ < kSameS) return false;
        for (const std::string& q : q_) {
            if (q == text) return false;
        }
        if (q_.size() >= kMaxQueued) return false;
        q_.push_back(text);
        return true;
    }
    /// The next notice to show now, or false (empty, or the previous one is still up).
    bool Pop(double now, std::string* out) {
        if (q_.empty() || now - last_shown_ < kGapS) return false;
        *out = q_.front();
        q_.pop_front();
        last_text_ = *out;
        last_shown_ = now;
        return true;
    }
    std::size_t size() const { return q_.size(); }

private:
    std::deque<std::string> q_;
    std::string last_text_;
    double last_shown_ = -1e9;
};

}  // namespace coop::itemnotice
