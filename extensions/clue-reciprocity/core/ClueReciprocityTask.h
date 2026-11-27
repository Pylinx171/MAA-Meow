#pragma once

#include "ClueReciprocityPolicy.h"
#include "InstHelper.h"
#include "Vision/OCRer.h"

#include <filesystem>
#include <functional>

namespace asst
{
class AbstractTask;
}

namespace asst::clue_reciprocity
{
class ClueReciprocityTask final : private InstHelper
{
public:
    explicit ClueReciprocityTask(AbstractTask& task);

    bool run(const std::string& key, const std::function<bool()>& receive_own_clue);

private:
    struct Ledger
    {
        bool initialized = false;
        bool wall_cleared = false;
        int initial_own_stock = 0;
        std::vector<Gift> gifts;
    };

    struct Card
    {
        Gift gift;
        Rect rect;
        bool pinned = false;
    };

    struct SendCandidate
    {
        Recipient recipient;
        Rect button;
        int page;
    };

    std::optional<Ledger> load_ledger(const std::filesystem::path& path);
    bool save_ledger(const std::filesystem::path& path, const Ledger& ledger);
    static void register_gifts(Ledger& ledger, const std::vector<Card>& cards);
    std::vector<TextRect> read(const cv::Mat& image, const Rect& roi, bool without_det = false);
    std::optional<Rect> find_word(const cv::Mat& image, const Rect& roi, std::string_view text);
    std::optional<Rect> match(const cv::Mat& image, const std::string& task_name);
    std::optional<Rect> find_page_arrow(const cv::Mat& image, bool forward);
    bool select_received_tab();
    bool received_tab_active(const cv::Mat& image, const Rect& label);
    bool click(const Rect& rect);
    bool clear_wall();
    bool close_drawer();
    bool open_received_library();
    std::optional<std::vector<Card>> scan_cards(bool pending);
    std::optional<std::vector<Card>> read_cards(const cv::Mat& image, bool pending);
    bool receive_new_gifts(const std::filesystem::path& path, Ledger& ledger);
    bool pin_received_gifts();
    bool run_send_step(const std::string& name, std::vector<std::string> next);
    bool send_own_gifts(const Ledger& ledger);
    std::optional<bool> send_one(const Ledger& ledger, bool allow_fallback, int stock);
    std::optional<std::vector<SendCandidate>> send_candidates(const cv::Mat& image, int page);
    bool turn_page(bool forward);
    bool rewind_friend_pages();
    std::optional<int> own_stock(const cv::Mat& image);
    std::optional<int> wait_for_own_stock();
    bool keep_needs_filter();

    const AbstractTask& m_task;
};
}
