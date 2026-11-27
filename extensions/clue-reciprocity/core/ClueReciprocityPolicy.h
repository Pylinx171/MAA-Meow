#pragma once

#include <algorithm>
#include <chrono>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

namespace asst::clue_reciprocity
{
struct Gift
{
    std::string sender;
    std::chrono::sys_seconds expires;
};

struct Recipient
{
    std::string name;
    bool needs_clue = false;
    bool online_today = false;
};

class ClueReciprocityPolicy
{
public:
    static constexpr int MinimumOwnStockForFallback = 9;

    static bool sent_a_clue(
        const Recipient& recipient,
        const std::vector<Gift>& gifts)
    {
        return !recipient.name.empty() && std::ranges::any_of(gifts, [&](const auto& gift) {
            return same_player(gift.sender, recipient.name);
        });
    }

    static std::optional<size_t> recipient(
        const std::vector<Recipient>& candidates,
        const std::vector<Gift>& gifts,
        int own_stock,
        bool checked_all_pages)
    {
        std::optional<size_t> fallback;
        for (size_t index = 0; index < candidates.size(); ++index) {
            const auto& candidate = candidates[index];
            if (!candidate.needs_clue) {
                continue;
            }
            if (sent_a_clue(candidate, gifts)) {
                return index;
            }
            if (!fallback && candidate.online_today && !candidate.name.empty()) {
                fallback = index;
            }
        }
        return checked_all_pages && own_stock >= MinimumOwnStockForFallback ? fallback : std::nullopt;
    }

    static bool same_player(std::string_view sender, std::string_view recipient)
    {
        if (sender.empty() || recipient.empty()) {
            return false;
        }
        // 带 # 编号时必须完全匹配；不含编号的记录也必须由调用端验证唯一。
        return sender == recipient;
    }
};
}
