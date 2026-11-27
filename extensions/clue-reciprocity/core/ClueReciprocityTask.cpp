#include "ClueReciprocityTask.h"

#include <array>
#include <fstream>
#include <regex>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include "Config/GeneralConfig.h"
#include "Config/TaskData.h"
#include "Config/TemplResource.h"
#include "Controller/Controller.h"
#include "Task/ProcessTask.h"
#include "Utils/Logger.hpp"
#include "Utils/WorkingDir.hpp"
#include "Vision/Infrast/InfrastClueVacancyImageAnalyzer.h"
#include "Vision/Matcher.h"

namespace asst::clue_reciprocity
{
namespace
{
constexpr std::string_view LogTag = "PylinxClueReciprocity";
constexpr int PageLimit = 35;
const Rect RightDrawer { 850, 0, 430, 720 };
const Rect DrawerTabs { 850, 0, 430, 63 };
const Rect LibraryLabel { 1110, 475, 170, 135 };
const Rect PageArrows { 1030, 650, 250, 70 };
const cv::Rect FriendRows { 467, 72, 710, 573 };
const Rect FilterLabel { 434, 0, 259, 52 };
const Rect StockLabel { 280, 1, 118, 53 };
const Rect FirstClueTab { 78, 70, 19, 33 };
constexpr int ClueTabSpacing = 51;
constexpr std::array WallSlots { "No1", "No2", "No3", "No4", "No5", "No6", "No7" };

std::string compact(std::string value)
{
    std::erase_if(value, [](auto ch) { return ch == ' ' || ch == '\n' || ch == '\r' || ch == '\t'; });
    for (size_t pos = value.find("＃"); pos != std::string::npos; pos = value.find("＃", pos + 1)) {
        value.replace(pos, std::string_view("＃").size(), "#");
    }
    return value;
}

std::optional<std::chrono::seconds> time_left(const std::string& text)
{
    static const std::regex pattern(R"(^(?:剩余)?([0-9]{1,2})(天|小时|时|分钟|分)$)");
    std::smatch match;
    if (!std::regex_match(text, match, pattern)) {
        return std::nullopt;
    }
    const auto value = std::stoi(match[1].str());
    const auto unit = match[2].str();
    if (unit == "天") {
        return value <= 10 ? std::optional<std::chrono::seconds>(std::chrono::days(value)) : std::nullopt;
    }
    if (unit == "时" || unit == "小时") {
        return value <= 24 ? std::optional<std::chrono::seconds>(std::chrono::hours(value)) : std::nullopt;
    }
    return value <= 60 ? std::optional<std::chrono::seconds>(std::chrono::minutes(value)) : std::nullopt;
}

bool is_player_id(const std::string& value)
{
    const auto hash = value.rfind('#');
    return hash != std::string::npos && hash > 0 && value.size() > hash + 2 &&
           std::ranges::all_of(std::string_view(value).substr(hash + 1), [](char ch) {
               return ch >= '0' && ch <= '9';
           });
}

bool has_text(const std::vector<TextRect>& texts, std::string_view value)
{
    return std::ranges::any_of(texts, [&](const auto& word) { return word.text.find(value) != std::string::npos; });
}

std::optional<Rect> filter_knob(const cv::Mat& image, const Rect& label)
{
    const int left = label.x + label.width;
    const int top = std::max(0, label.y - 8);
    const int width = std::min(88, image.cols - left);
    const int height = std::min(label.height + 16, image.rows - top);
    if (width < 12 || height < 12 || image.type() != CV_8UC3) {
        return std::nullopt;
    }
    cv::Mat white, components, stats, centroids;
    cv::inRange(image(cv::Rect { left, top, width, height }), cv::Scalar(223, 223, 223),
                cv::Scalar(255, 255, 255), white);
    const int count = cv::connectedComponentsWithStats(white, components, stats, centroids);
    std::optional<Rect> result;
    for (int i = 1; i < count; ++i) {
        const int w = stats.at<int>(i, cv::CC_STAT_WIDTH);
        const int h = stats.at<int>(i, cv::CC_STAT_HEIGHT);
        const int area = stats.at<int>(i, cv::CC_STAT_AREA);
        if (w >= 11 && w <= 24 && h >= 11 && h <= 24 && std::abs(w - h) <= 5 &&
            area * 100 >= w * h * 55) {
            if (result) {
                return std::nullopt;
            }
            result = Rect { left + stats.at<int>(i, cv::CC_STAT_LEFT), top + stats.at<int>(i, cv::CC_STAT_TOP), w, h };
        }
    }
    return result;
}

bool filter_is_on(const cv::Mat& image, const Rect& knob)
{
    const auto roi = cv::Rect(knob.x - 22, knob.y, knob.width + 44, knob.height) &
                     cv::Rect(0, 0, image.cols, image.rows);
    cv::Mat orange;
    cv::inRange(image(roi), cv::Scalar(0, 65, 165), cv::Scalar(100, 205, 255), orange);
    return cv::countNonZero(orange) >= knob.width * knob.height / 2;
}
}

ClueReciprocityTask::ClueReciprocityTask(AbstractTask& task) : InstHelper(task.inst()), m_task(task) {}

bool ClueReciprocityTask::run(
    const std::string& key,
    const std::function<bool()>& receive_own_clue)
{
    Log.info(LogTag, "begin");
    if (key.empty() || key.size() > 240 || key.find_first_not_of("0123456789abcdef") != std::string::npos) {
        return false;
    }
    const auto path = UserDir.get() / "clue-reciprocity" / (key + ".json");
    auto ledger = load_ledger(path);
    if (!ledger) {
        Log.warn(LogTag, "cannot read clue ledger");
        return false;
    }
    if (!ledger->initialized) {
        if (!ledger->wall_cleared) {
            if (!clear_wall()) {
                Log.warn(LogTag, "wall verification incomplete; leaving other clue steps for next run");
                return false;
            }
            ledger->wall_cleared = true;
            if (!save_ledger(path, *ledger)) {
                return false;
            }
        }
        if (!open_received_library()) {
            Log.warn(LogTag, "failed to open received library");
            return false;
        }
        const auto own = [this]() -> std::optional<int> {
            static const std::regex count(R"(自有库([0-9]{1,2})/10)");
            for (const auto& word : read(ctrler()->get_image(), { 991, 0, 159, 55 })) {
                std::smatch match;
                if (std::regex_match(word.text, match, count) && std::stoi(match[1].str()) <= 10) {
                    return std::stoi(match[1].str());
                }
            }
            return std::nullopt;
        }();
        const auto cards = scan_cards(false);
        const bool drawer_closed = close_drawer();
        if (!own || !cards || !drawer_closed) {
            Log.warn(LogTag, "initial inventory scan incomplete");
            return false;
        }
        ledger->initial_own_stock = *own;
        register_gifts(*ledger, *cards);
        ledger->initialized = true;
        if (!save_ledger(path, *ledger)) {
            return false;
        }
        Log.info(LogTag, "initial inventory registered", "own", *own, "received entries", ledger->gifts.size());
    }

    if (!receive_new_gifts(path, *ledger) || !pin_received_gifts() || need_exit()) {
        return false;
    }
    // 只有出现新产出提示才进入领取页，绕开会在空库存时误开的满库探测。
    if (match(ctrler()->get_image(), "InfrastClueSelfNew") && !receive_own_clue()) {
        Log.warn(LogTag, "could not return from self-clue panel");
        return false;
    }
    return !need_exit() && send_own_gifts(*ledger);
}

std::optional<ClueReciprocityTask::Ledger> ClueReciprocityTask::load_ledger(const std::filesystem::path& path)
{
    std::error_code error;
    if (!std::filesystem::exists(path, error)) {
        return error ? std::nullopt : std::optional<Ledger>(Ledger {});
    }
    try {
        const auto document = json::open(path);
        if (!document || !document->contains("initialized") || !document->contains("gifts")) {
            return std::nullopt;
        }
        Ledger ledger {
            document->at("initialized").as_boolean(),
            document->get("wall_cleared", document->at("initialized").as_boolean()),
            document->get("initial_own_stock", 0),
            {},
        };
        for (const auto& entry : document->at("gifts").as_array()) {
            ledger.gifts.push_back({
                entry.at("sender").as_string(),
                std::chrono::sys_seconds(std::chrono::seconds(entry.at("expires").as_long_long())),
            });
        }
        return ledger;
    }
    catch (const std::exception&) {
        return std::nullopt;
    }
}

bool ClueReciprocityTask::save_ledger(const std::filesystem::path& path, const Ledger& ledger)
{
    auto gifts = json::array();
    for (const auto& gift : ledger.gifts) {
        gifts.emplace_back(json::object {
            { "sender", gift.sender },
            { "expires", static_cast<long long>(gift.expires.time_since_epoch().count()) },
        });
    }
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) {
        return false;
    }
    const auto temporary = path.string() + ".tmp";
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    file << json::value(json::object {
        { "initialized", ledger.initialized },
        { "wall_cleared", ledger.wall_cleared },
        { "initial_own_stock", ledger.initial_own_stock },
        { "gifts", std::move(gifts) },
    }).dumps();
    file.close();
    if (!file) {
        return false;
    }
    std::filesystem::rename(temporary, path, error);
    return !error;
}

void ClueReciprocityTask::register_gifts(Ledger& ledger, const std::vector<Card>& cards)
{
    for (const auto& card : cards) {
        auto existing = std::ranges::find_if(ledger.gifts, [&](const auto& gift) {
            return gift.sender == card.gift.sender &&
                   std::chrono::floor<std::chrono::days>(gift.expires) ==
                       std::chrono::floor<std::chrono::days>(card.gift.expires);
        });
        if (existing == ledger.gifts.end()) {
            ledger.gifts.push_back(card.gift);
        }
        else {
            existing->expires = std::max(existing->expires, card.gift.expires);
        }
    }
}

std::vector<TextRect> ClueReciprocityTask::read(const cv::Mat& image, const Rect& roi, bool without_det)
{
    if (image.empty() || image.cols < WindowWidthDefault || image.rows < WindowHeightDefault) {
        return {};
    }
    OCRer reader(image, roi, inst());
    reader.set_without_det(without_det);
    reader.set_log_tracing(false);
    std::vector<TextRect> words;
    if (const auto result = reader.analyze()) {
        for (auto word : *result) {
            word.text = compact(std::move(word.text));
            words.push_back(std::move(word));
        }
    }
    return words;
}

std::optional<Rect> ClueReciprocityTask::find_word(const cv::Mat& image, const Rect& roi, std::string_view text)
{
    const auto words = read(image, roi);
    std::optional<Rect> result;
    for (const auto& word : words) {
        if (word.text == text && word.score >= 0.55) {
            if (result) {
                return std::nullopt;
            }
            result = word.rect;
        }
    }
    return result;
}

std::optional<Rect> ClueReciprocityTask::match(const cv::Mat& image, const std::string& task_name)
{
    if (image.empty()) {
        return std::nullopt;
    }
    Matcher matcher(image);
    matcher.set_log_tracing(false);
    matcher.set_task_info(task_name);
    if (!matcher.analyze()) {
        return std::nullopt;
    }
    return matcher.get_result().rect;
}

std::optional<Rect> ClueReciprocityTask::find_page_arrow(const cv::Mat& image, bool forward)
{
    if (image.empty() || !match(image, "CloseSendClue")) {
        return std::nullopt;
    }
    const auto& next = TemplResource::get_instance().get_templ("ClueGiveToNextPage.png");
    if (next.empty()) {
        return std::nullopt;
    }
    cv::Mat arrow;
    if (forward) {
        arrow = next;
    }
    else {
        cv::flip(next, arrow, 1);
    }
    Matcher matcher(image, PageArrows);
    matcher.set_log_tracing(false);
    matcher.set_templ(arrow);
    matcher.set_method(MatchMethod::Ccoeff);
    matcher.set_threshold(0.82);
    const auto result = matcher.analyze();
    if (!result) {
        return std::nullopt;
    }
    const auto& box = result->rect;
    if (box.width < 16 || box.height < 16) {
        return std::nullopt;
    }
    const auto top_of_circle = cv::Rect { box.x + box.width * 3 / 8, box.y + box.height / 8, box.width / 4,
                                         box.height / 8 };
    const auto light = cv::mean(image(top_of_circle));
    return light[0] >= 172 && light[1] >= 172 && light[2] >= 172
               ? std::optional<Rect>(box)
               : std::nullopt;
}

bool ClueReciprocityTask::received_tab_active(const cv::Mat& image, const Rect& label)
{
    const auto area = cv::Rect(label.x, std::max(0, label.y - 7), label.width, 4) &
                      cv::Rect(0, 0, image.cols, image.rows);
    if (area.empty()) {
        return false;
    }
    const auto background = cv::mean(image(area));
    return background[0] > 170 && background[1] > 170 && background[2] > 170;
}

bool ClueReciprocityTask::select_received_tab()
{
    const auto before = ctrler()->get_image();
    const auto label = find_word(before, DrawerTabs, "接收库");
    if (!label) {
        return false;
    }
    if (received_tab_active(before, *label)) {
        return true;
    }
    if (!click(*label)) {
        return false;
    }
    const auto after = ctrler()->get_image();
    const auto updated = find_word(after, DrawerTabs, "接收库");
    return updated && received_tab_active(after, *updated);
}

bool ClueReciprocityTask::click(const Rect& rect)
{
    return !need_exit() && ctrler()->click(rect) && sleep(650);
}

bool ClueReciprocityTask::clear_wall()
{
    for (int pass = 0; pass < 3 && !need_exit(); ++pass) {
        bool remaining = false;
        for (int number = 1; number <= 7 && !need_exit(); ++number) {
            const auto slot = std::to_string(number);
            const auto board = ctrler()->get_image();
            if (!match(board, "ReceptionFlag")) {
                return false;
            }
            InfrastClueVacancyImageAnalyzer occupied(board);
            occupied.set_to_be_analyzed({ slot });
            if (!occupied.analyze()) {
                continue;
            }
            if (!click(occupied.get_vacancy().at(slot))) {
                return false;
            }
            auto image = ctrler()->get_image();
            auto pin = match(image, "InfrastClueVacancyPin");
            for (int scroll = 0; !pin && scroll < 3 &&
                 has_text(read(image, { 850, 0, 430, 63 }), "接收库"); ++scroll) {
                if (!ctrler()->swipe(Point { 1080, 602 }, Point { 1080, 240 }, 480) || !sleep(600)) {
                    return false;
                }
                image = ctrler()->get_image();
                pin = match(image, "InfrastClueVacancyPin");
            }
            if (pin && !click(*pin)) {
                return false;
            }
            if (!close_drawer()) {
                return false;
            }
            InfrastClueVacancyImageAnalyzer empty(ctrler()->get_image());
            empty.set_to_be_analyzed({ "No" + slot });
            const bool cleared = empty.analyze();
            remaining |= !cleared;
            Log.info(LogTag, "clear slot", number, "verified", cleared);
        }
        if (!remaining && !need_exit()) {
            InfrastClueVacancyImageAnalyzer empty(ctrler()->get_image());
            empty.set_to_be_analyzed({ "No1", "No2", "No3", "No4", "No5", "No6", "No7" });
            empty.analyze();
            return empty.get_vacancy().size() == WallSlots.size();
        }
    }
    return false;
}

bool ClueReciprocityTask::close_drawer()
{
    const auto image_before = ctrler()->get_image();
    const auto icon = match(image_before, "InfrastReceptionIcon");
    if (!icon || !click(*icon)) {
        return false;
    }
    const auto image = ctrler()->get_image();
    const auto labels = read(image, { 850, 0, 430, 63 });
    return !has_text(labels, "接收库") && !has_text(labels, "接收线索") && match(image, "ReceptionFlag").has_value();
}

bool ClueReciprocityTask::open_received_library()
{
    const auto image = ctrler()->get_image();
    if (!match(image, "ReceptionFlag")) {
        return false;
    }
    const auto button = find_word(image, LibraryLabel, "线索");
    return button && click(*button) && select_received_tab();
}

std::optional<std::vector<ClueReciprocityTask::Card>> ClueReciprocityTask::read_cards(
    const cv::Mat& image,
    bool pending)
{
    const auto words = read(image, RightDrawer);
    const auto label = std::ranges::find_if(words, [&](const auto& word) {
        return word.rect.y < DrawerTabs.height && word.text == (pending ? "接收线索" : "接收库");
    });
    if (label == words.end() || (!pending && !received_tab_active(image, label->rect))) {
        return std::nullopt;
    }
    if (has_text(words, "暂无") || has_text(words, "没有线索")) {
        return std::vector<Card> {};
    }
    auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    std::vector<Card> cards;
    for (const auto& expiry : words) {
        const auto remaining = time_left(expiry.text);
        if (!remaining || expiry.rect.y < 140) {
            continue;
        }
        const int top = std::max(90, expiry.rect.y - 150);
        auto sender = std::ranges::find_if(words, [&](const auto& word) {
            const auto tag = word.text.find("来自");
            const auto id = tag == std::string::npos ? word.text : word.text.substr(tag + std::string_view("来自").size());
            return is_player_id(id) && word.rect.y >= top && word.rect.y < expiry.rect.y + 12 && word.score >= 0.6;
        });
        if (sender == words.end()) {
            if (expiry.rect.y < 220) {
                continue; // 滚动后顶部可能只有半张卡片
            }
            return std::nullopt;
        }
        auto id = sender->text;
        if (const auto tag = id.find("来自"); tag != std::string::npos) {
            id.erase(0, tag + std::string_view("来自").size());
        }
        const bool pinned = std::ranges::any_of(words, [&](const auto& word) {
            return word.rect.y >= top && word.rect.y <= expiry.rect.y &&
                   (word.text.find("已置入") != std::string::npos ||
                    word.text.find("取下该线索") != std::string::npos);
        });
        const Rect card { expiry.rect.x, std::max(top + 12, expiry.rect.y - 79), expiry.rect.width, 24 };
        cards.push_back({ Gift { id, now + *remaining }, card, pinned });
    }
    if (cards.empty() && has_text(words, "来自")) {
        return std::nullopt;
    }
    return cards;
}

std::optional<std::vector<ClueReciprocityTask::Card>> ClueReciprocityTask::scan_cards(bool pending)
{
    std::vector<Card> all;
    cv::Mat previous;
    const cv::Rect stable_region { 863, 100, 399, 509 };
    for (int page = 0; page < PageLimit && !need_exit(); ++page) {
        const auto image = ctrler()->get_image();
        if (image.cols < WindowWidthDefault || image.rows < WindowHeightDefault) {
            return std::nullopt;
        }
        const auto region = image(stable_region);
        if (!previous.empty() && cv::norm(region, previous, cv::NORM_L1) / (region.total() * region.channels()) < 1.5) {
            return all;
        }
        auto cards = read_cards(image, pending);
        if (!cards) {
            return std::nullopt;
        }
        all.insert(all.end(), cards->begin(), cards->end());
        if (cards->empty() && page == 0) {
            return all;
        }
        previous = region.clone();
        if (!ctrler()->swipe(Point { 1080, 604 }, Point { 1080, 239 }, 520) || !sleep(700)) {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

bool ClueReciprocityTask::receive_new_gifts(const std::filesystem::path& path, Ledger& ledger)
{
    auto notification = match(ctrler()->get_image(), "InfrastClueFriendNew");
    if (!notification) {
        return true;
    }
    const auto button = notification->move(Task.get("InfrastClueFriendNew")->rect_move);
    if (!click(button)) {
        return false;
    }
    const auto cards = scan_cards(true);
    const auto receive = match(ctrler()->get_image(), "GetFriendClue");
    if (!cards || !receive || cards->empty()) {
        Log.warn(LogTag, "cannot verify pending clue senders");
        return close_drawer();
    }
    register_gifts(ledger, *cards);
    return save_ledger(path, ledger) && click(*receive) && sleep(1200) &&
           match(ctrler()->get_image(), "ReceptionFlag").has_value();
}

bool ClueReciprocityTask::pin_received_gifts()
{
    for (auto slot : WallSlots) {
        const auto image = ctrler()->get_image();
        InfrastClueVacancyImageAnalyzer vacancy(image);
        vacancy.set_to_be_analyzed({ slot });
        if (!vacancy.analyze()) {
            continue;
        }
        if (!click(vacancy.get_vacancy().cbegin()->second)) {
            return false;
        }
        if (!select_received_tab()) {
            return false;
        }
        const auto cards = read_cards(ctrler()->get_image(), false);
        if (!cards) {
            return false;
        }
        const auto card = std::ranges::find(cards.value(), false, &Card::pinned);
        if (card != cards->end() && !click(card->rect)) {
            return false;
        }
        if (!close_drawer()) {
            return false;
        }
    }
    return !need_exit();
}

std::optional<int> ClueReciprocityTask::own_stock(const cv::Mat& image)
{
    static const std::regex pattern(R"((?:自有库)?([0-9]{1,2})/10)");
    for (const auto& word : read(image, StockLabel, true)) {
        std::smatch match;
        if (word.score >= 0.7 && std::regex_match(word.text, match, pattern)) {
            const int stock = std::stoi(match[1].str());
            if (stock <= 10) {
                return stock;
            }
        }
    }
    return std::nullopt;
}

std::optional<int> ClueReciprocityTask::wait_for_own_stock()
{
    const auto delay = Config.get_options().task_delay;
    for (int attempt = 0; attempt <= 3 && !need_exit(); ++attempt) {
        if (attempt > 0 && !sleep(delay)) {
            break;
        }
        const auto image = ctrler()->get_image();
        if (match(image, "CloseSendClue")) {
            if (const auto stock = own_stock(image)) {
                return stock;
            }
        }
    }
    return std::nullopt;
}

bool ClueReciprocityTask::keep_needs_filter()
{
    auto image = ctrler()->get_image();
    auto label = find_word(image, FilterLabel, "仅显示缺少对应线索的好友");
    if (!label) {
        return false;
    }
    const auto knob = filter_knob(image, *label);
    if (!knob) {
        return false;
    }
    if (filter_is_on(image, *knob)) {
        return true;
    }
    if (!click(*knob)) {
        return false;
    }
    image = ctrler()->get_image();
    label = find_word(image, FilterLabel, "仅显示缺少对应线索的好友");
    const auto updated = label ? filter_knob(image, *label) : std::nullopt;
    return updated && filter_is_on(image, *updated);
}

std::optional<std::vector<ClueReciprocityTask::SendCandidate>> ClueReciprocityTask::send_candidates(
    const cv::Mat& image,
    int page)
{
    static constexpr std::array names { "1st", "2nd", "3rd", "4th" };
    std::vector<SendCandidate> candidates;
    for (int row = 0; row < 4; ++row) {
        const auto prefix = std::string("ClueGiveTo") + names[row];
        const auto confirm = match(image, prefix + "Confirm");
        if (!confirm) {
            continue;
        }
        const auto words = read(image, { 569, 64 + 150 * row, 240, 122 });
        auto name = std::ranges::find_if(words, [](const auto& word) {
            return word.score >= 0.7 && is_player_id(word.text);
        });
        if (name == words.end()) {
            return std::nullopt;
        }
        candidates.push_back({ Recipient { name->text, true, has_text(words, "今日") }, *confirm, page });
    }
    return candidates;
}

bool ClueReciprocityTask::turn_page(bool forward)
{
    const auto before = ctrler()->get_image();
    const auto arrow = find_page_arrow(before, forward);
    if (!arrow || !click(*arrow)) {
        return false;
    }
    for (int check = 0; check < 3 && !need_exit(); ++check) {
        const auto after = ctrler()->get_image();
        if (after.cols >= WindowWidthDefault && after.rows >= WindowHeightDefault &&
            cv::norm(before(FriendRows), after(FriendRows), cv::NORM_L1) /
                    (FriendRows.area() * before.channels()) > 5.0) {
            return true;
        }
        sleep(300);
    }
    Log.warn(LogTag, "friend page did not change after clicking", forward ? "next" : "previous");
    return false;
}

bool ClueReciprocityTask::rewind_friend_pages()
{
    for (int step = 0; step < PageLimit && !need_exit(); ++step) {
        if (!find_page_arrow(ctrler()->get_image(), false)) {
            return true;
        }
        if (!turn_page(false)) {
            return false;
        }
    }
    return false;
}

std::optional<bool> ClueReciprocityTask::send_one(const Ledger& ledger, bool allow_fallback, int stock)
{
    bool scan_complete = true;
    for (int clue = 1; clue <= 7 && !need_exit(); ++clue) {
        const auto image = ctrler()->get_image();
        if (!match(image, "CloseSendClue")) {
            return std::nullopt;
        }
        // 只识别圆框内的数字，避免边框被识别成带圈数字或其他字符。
        auto area = FirstClueTab;
        area.x += (clue - 1) * ClueTabSpacing;
        const auto label = read(image, area, true);
        if (label.size() != 1 || label.front().score < 0.7 || label.front().text != std::to_string(clue)) {
            scan_complete = false;
            continue;
        }
        if (!click(label.front().rect)) {
            return std::nullopt;
        }
        const auto first_clue = match(ctrler()->get_image(), "SelectClue");
        if (!first_clue) {
            continue;
        }
        if (!click(*first_clue) || !match(ctrler()->get_image(), "ClueSelected") || !keep_needs_filter()) {
            return std::nullopt;
        }

        std::vector<SendCandidate> candidates;
        int page = 0;
        bool all_pages = false;
        bool all_rows_read = true;
        for (; page < PageLimit && !need_exit(); ++page) {
            const auto image = ctrler()->get_image();
            const auto rows = send_candidates(image, page);
            if (rows) {
                candidates.insert(candidates.end(), rows->begin(), rows->end());
            }
            else {
                all_rows_read = false;
            }
            const bool has_next = find_page_arrow(image, true).has_value();
            Log.info(LogTag, "friend page", page + 1, "clue", clue, "next enabled", has_next);
            if (!has_next) {
                all_pages = true;
                break;
            }
            if (!turn_page(true)) {
                return std::nullopt;
            }
        }
        if (!all_pages) {
            return std::nullopt;
        }
        std::vector<Recipient> recipients;
        for (const auto& candidate : candidates) {
            recipients.push_back(candidate.recipient);
        }
        const auto target = ClueReciprocityPolicy::recipient(
            recipients, ledger.gifts, allow_fallback ? stock : 0, all_pages && all_rows_read);
        if (!target) {
            scan_complete &= all_rows_read;
            if (!all_rows_read && !allow_fallback) {
                Log.warn(LogTag, "friend name recognition incomplete on clue", clue);
            }
            for (int current = page; current > 0; --current) {
                if (!turn_page(false)) {
                    return std::nullopt;
                }
            }
            if (allow_fallback && !all_rows_read) {
                return std::nullopt;
            }
            continue;
        }
        const auto& selected = candidates.at(*target);
        for (int current = page; current > selected.page; --current) {
            if (!turn_page(false)) {
                return std::nullopt;
            }
        }
        const auto refreshed = send_candidates(ctrler()->get_image(), selected.page);
        if (!refreshed) {
            return std::nullopt;
        }
        const auto same = std::ranges::find_if(*refreshed, [&](const auto& candidate) {
            return candidate.recipient.name == selected.recipient.name &&
                   candidate.recipient.needs_clue &&
                   (ClueReciprocityPolicy::sent_a_clue(candidate.recipient, ledger.gifts) ||
                    candidate.recipient.online_today);
        });
        if (same == refreshed->end() || !click(same->button) || !sleep(800)) {
            return std::nullopt;
        }
        for (int attempt = 0; attempt < 3 && !need_exit(); ++attempt) {
            if (own_stock(ctrler()->get_image()) == stock - 1) {
                Log.info(LogTag, "sent one self-owned clue");
                return true;
            }
            sleep(550);
        }
        Log.warn(LogTag, "gift result uncertain; stopped sending in this run");
        return std::nullopt;
    }
    return scan_complete ? std::optional<bool>(false) : std::nullopt;
}

bool ClueReciprocityTask::run_send_step(const std::string& name, std::vector<std::string> next)
{
    ProcessTask task(m_task, { name });
    task.set_retry_times(ProcessTask::RetryTimesDefault);
    // 保留上游节点的识别、动作和延时，后续步骤由礼尚往来策略决定。
    return task.override_next(name, std::move(next)) && task.run() && !need_exit();
}

bool ClueReciprocityTask::send_own_gifts(const Ledger& ledger)
{
    if (!match(ctrler()->get_image(), "SendClues")) {
        return true;
    }
    if (!run_send_step("SendClues", {})) {
        return false;
    }

    for (int attempt = 0; attempt < 10 && !need_exit() && rewind_friend_pages(); ++attempt) {
        const auto stock = wait_for_own_stock();
        if (!stock) {
            LogWarn << LogTag << "cannot verify own clue stock";
            break;
        }
        LogInfo << LogTag << "own clue stock" << *stock;
        if (*stock == 0) {
            break;
        }
        auto result = send_one(ledger, false, *stock);
        if (result && !*result && *stock >= ClueReciprocityPolicy::MinimumOwnStockForFallback) {
            result = send_one(ledger, true, *stock);
        }
        if (!result) {
            LogWarn << LogTag << "gift scan incomplete";
            break;
        }
        if (!*result) {
            break;
        }
    }
    return !need_exit() && run_send_step("CloseSendClue", { "ReceptionFlag" });
}
}
