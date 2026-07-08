#include <Geode/Geode.hpp>
#include <Geode/binding/GameStatsManager.hpp>
#include <Geode/binding/ProfilePage.hpp>
#include <Geode/binding/GJUserScore.hpp>
#include <Geode/modify/ProfilePage.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/ui/BasedButtonSprite.hpp>
#include <Geode/ui/TextInput.hpp>
#include <Geode/cocos/draw_nodes/CCDrawNode.h>

#include <algorithm>
#include <array>
#include <ctime>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using namespace geode::prelude;

namespace {
    constexpr int64_t DAY_SECONDS = 24LL * 60LL * 60LL;
    constexpr int64_t AUTO_SNAPSHOT_COOLDOWN = 12LL * 60LL * 60LL;
    constexpr int64_t KEEP_DAYS = 370;

    struct StatDef {
        std::string id;
        std::string name;
        int key;
    };

    // Only the stats shown in the summary table are selectable in Graph View.
    // This keeps the graph picker useful instead of making you scroll through
    // cursed filler stats like map packs, likes/dislikes, shards, etc.
    const std::vector<StatDef> STATS = {
        {"stars", "Stars", 6},
        {"moons", "Moons", 28},
        {"demons", "Demons", 5},
        {"user_coins", "User Coins", 12},
        {"orbs", "Mana Orbs", 14},
        {"total_orbs", "Total Orbs", 22},
        {"diamonds", "Diamonds", 13},
        {"demon_keys", "Demon Keys", 21},
    };

    struct Snapshot {
        int64_t unixTime = 0;
        std::map<std::string, int> values;
    };

    struct TimelineMode {
        std::string name;
        std::string shortName;
        int bucketDays;
        bool averagePerDay;
        bool rollingAverage;
    };

    constexpr size_t MODE_COUNT = 3;

    const std::vector<TimelineMode> MODES = {
        {"Weekly Change", "Weeks", 7, false, false},
        {"Daily Change", "Days", 1, false, false},
        {"7-Day Avg/d", "Avg/d", 1, true, true},
    };

    struct ColorPreset {
        std::string name;
        ccColor4F color;
        ccColor3B labelColor;
    };

    const std::vector<ColorPreset> COLOR_PRESETS = {
        {"Yellow", {1.00f, 0.92f, 0.25f, 1.00f}, ccc3(255, 238, 70)},
        {"Cyan",   {0.20f, 0.78f, 1.00f, 1.00f}, ccc3(90, 230, 255)},
        {"Green",  {0.25f, 1.00f, 0.35f, 1.00f}, ccc3(90, 255, 90)},
        {"Pink",   {1.00f, 0.35f, 0.85f, 1.00f}, ccc3(255, 110, 230)},
        {"Orange", {1.00f, 0.55f, 0.15f, 1.00f}, ccc3(255, 170, 70)},
        {"White",  {0.92f, 0.92f, 0.92f, 1.00f}, ccc3(235, 235, 235)},
        {"Red",    {1.00f, 0.25f, 0.25f, 1.00f}, ccc3(255, 90, 90)},
        {"Purple", {0.62f, 0.40f, 1.00f, 1.00f}, ccc3(180, 130, 255)},
    };

    struct GraphSettings {
        int displayDays = 14;
        int avgWindowDays = 7;
        // Day buckets reset at 12:00 in this UTC offset. Default UTC+8 = Taiwan time.
        int timezoneOffsetHours = 8;
        std::array<int, MODE_COUNT> lineColor = {1, 1, 3};
        std::array<int, MODE_COUNT> nodeColor = {0, 0, 0};
    };

    struct GraphPoint {
        std::string label;
        double value = 0.0;
        int64_t startTime = 0;
        int64_t endTime = 0;
    };

    std::filesystem::path snapshotPath() {
        return Mod::get()->getSaveDir() / "snapshots.tsv";
    }

    std::filesystem::path graphSettingsPath() {
        return Mod::get()->getSaveDir() / "graph_settings.tsv";
    }

    int clampDisplayDays(int days) {
        return std::max(3, std::min(40, days));
    }

    int clampAvgWindowDays(int days) {
        return std::max(2, std::min(40, days));
    }

    int clampTimezoneOffsetHours(int hours) {
        // Practical UTC offsets. This keeps the setting simple as whole hours.
        return std::max(-12, std::min(14, hours));
    }

    int64_t timezoneOffsetSeconds(GraphSettings const& settings) {
        return static_cast<int64_t>(clampTimezoneOffsetHours(settings.timezoneOffsetHours)) * 3600LL;
    }

    int64_t dayIndexFor(int64_t unixTime, GraphSettings const& settings) {
        // A "new day" starts at 12:00 in the selected UTC offset.
        // Example: UTC+8 means boundaries occur at 04:00 UTC.
        auto shifted = unixTime + timezoneOffsetSeconds(settings) - 12LL * 3600LL;
        if (shifted >= 0) return shifted / DAY_SECONDS;
        return (shifted - DAY_SECONDS + 1) / DAY_SECONDS;
    }

    int64_t dayStartForIndex(int64_t dayIndex, GraphSettings const& settings) {
        return dayIndex * DAY_SECONDS - timezoneOffsetSeconds(settings) + 12LL * 3600LL;
    }

    std::string timezoneOffsetLabel(GraphSettings const& settings) {
        int hours = clampTimezoneOffsetHours(settings.timezoneOffsetHours);
        if (hours == 0) return "UTC";
        return fmt::format("UTC{:+d}", hours);
    }

    int clampColorIndex(int index) {
        if (COLOR_PRESETS.empty()) return 0;
        if (index < 0) return 0;
        if (index >= static_cast<int>(COLOR_PRESETS.size())) return static_cast<int>(COLOR_PRESETS.size()) - 1;
        return index;
    }

    GraphSettings loadGraphSettings() {
        GraphSettings settings;
        std::ifstream file(graphSettingsPath());
        if (!file.good()) return settings;

        std::string line;
        while (std::getline(file, line)) {
            auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            auto key = line.substr(0, eq);
            auto value = line.substr(eq + 1);
            int parsed = 0;
            try {
                parsed = std::stoi(value);
            } catch (...) {
                continue;
            }

            if (key == "display_days") {
                settings.displayDays = clampDisplayDays(parsed);
                continue;
            }
            if (key == "avg_window_days") {
                settings.avgWindowDays = clampAvgWindowDays(parsed);
                continue;
            }
            if (key == "timezone_offset_hours") {
                settings.timezoneOffsetHours = clampTimezoneOffsetHours(parsed);
                continue;
            }

            for (size_t i = 0; i < MODE_COUNT; i++) {
                if (key == fmt::format("line_{}", i)) settings.lineColor[i] = clampColorIndex(parsed);
                if (key == fmt::format("node_{}", i)) settings.nodeColor[i] = clampColorIndex(parsed);
            }
        }
        return settings;
    }

    void saveGraphSettings(GraphSettings const& settings) {
        std::filesystem::create_directories(Mod::get()->getSaveDir());
        std::ofstream file(graphSettingsPath(), std::ios::trunc);
        file << "display_days=" << clampDisplayDays(settings.displayDays) << '\n';
        file << "avg_window_days=" << clampAvgWindowDays(settings.avgWindowDays) << '\n';
        file << "timezone_offset_hours=" << clampTimezoneOffsetHours(settings.timezoneOffsetHours) << '\n';
        for (size_t i = 0; i < MODE_COUNT; i++) {
            file << "line_" << i << '=' << clampColorIndex(settings.lineColor[i]) << '\n';
            file << "node_" << i << '=' << clampColorIndex(settings.nodeColor[i]) << '\n';
        }
    }

    ColorPreset const& getColorPreset(int index) {
        return COLOR_PRESETS[clampColorIndex(index)];
    }

    std::string modeDisplayName(TimelineMode const& mode, GraphSettings const& settings) {
        if (mode.rollingAverage) {
            return fmt::format("{}-Day Avg/d", clampAvgWindowDays(settings.avgWindowDays));
        }
        return mode.name;
    }

    std::string fmtInt(int value) {
        bool negative = value < 0;
        std::string s = std::to_string(std::abs(value));
        std::string out;
        int count = 0;
        for (auto it = s.rbegin(); it != s.rend(); ++it) {
            if (count == 3) {
                out.push_back(',');
                count = 0;
            }
            out.push_back(*it);
            count++;
        }
        if (negative) out.push_back('-');
        std::reverse(out.begin(), out.end());
        return out;
    }

    std::string fmtDelta(int delta) {
        if (delta > 0) return "+" + fmtInt(delta);
        return fmtInt(delta);
    }

    std::string fmtGraphValue(double value, bool average) {
        if (average) {
            if (std::abs(value) < 0.05) return "0.0";
            return fmt::format("{:+.1f}", value);
        }
        return fmtDelta(static_cast<int>(std::llround(value)));
    }

    int readStat(int key) {
        auto gsm = GameStatsManager::sharedState();
        if (!gsm) return 0;
        auto keyStr = std::to_string(key);
        return gsm->getStat(keyStr.c_str());
    }

    Snapshot makeSnapshot() {
        Snapshot snap;
        snap.unixTime = static_cast<int64_t>(std::time(nullptr));
        for (auto const& stat : STATS) {
            snap.values[stat.id] = readStat(stat.key);
        }
        return snap;
    }

    std::vector<std::string> split(std::string const& input, char delim) {
        std::vector<std::string> parts;
        std::stringstream ss(input);
        std::string item;
        while (std::getline(ss, item, delim)) parts.push_back(item);
        return parts;
    }

    std::vector<Snapshot> loadSnapshots() {
        std::vector<Snapshot> snaps;
        std::ifstream file(snapshotPath());
        if (!file.good()) return snaps;

        std::string line;
        while (std::getline(file, line)) {
            if (line.empty()) continue;
            auto parts = split(line, '\t');
            if (parts.empty()) continue;

            Snapshot snap;
            try {
                snap.unixTime = std::stoll(parts[0]);
            } catch (...) {
                continue;
            }

            for (size_t i = 1; i < parts.size(); i++) {
                auto eq = parts[i].find('=');
                if (eq == std::string::npos) continue;
                auto key = parts[i].substr(0, eq);
                auto val = parts[i].substr(eq + 1);
                try {
                    snap.values[key] = std::stoi(val);
                } catch (...) {}
            }
            snaps.push_back(std::move(snap));
        }

        std::sort(snaps.begin(), snaps.end(), [](auto const& a, auto const& b) {
            return a.unixTime < b.unixTime;
        });
        return snaps;
    }

    void saveSnapshots(std::vector<Snapshot> const& snaps) {
        std::filesystem::create_directories(Mod::get()->getSaveDir());
        std::ofstream file(snapshotPath(), std::ios::trunc);
        for (auto const& snap : snaps) {
            file << snap.unixTime;
            for (auto const& stat : STATS) {
                auto it = snap.values.find(stat.id);
                if (it != snap.values.end()) {
                    file << '\t' << stat.id << '=' << it->second;
                }
            }
            file << '\n';
        }
    }

    void pruneOldSnapshots(std::vector<Snapshot>& snaps) {
        auto now = static_cast<int64_t>(std::time(nullptr));
        auto cutoff = now - KEEP_DAYS * DAY_SECONDS;
        snaps.erase(std::remove_if(snaps.begin(), snaps.end(), [cutoff](Snapshot const& snap) {
            return snap.unixTime < cutoff;
        }), snaps.end());
    }

    bool addSnapshot(bool force) {
        auto snaps = loadSnapshots();
        auto now = static_cast<int64_t>(std::time(nullptr));

        if (!force && !snaps.empty() && now - snaps.back().unixTime < AUTO_SNAPSHOT_COOLDOWN) {
            return false;
        }

        snaps.push_back(makeSnapshot());
        pruneOldSnapshots(snaps);
        saveSnapshots(snaps);
        return true;
    }


    Snapshot pickBaseline(std::vector<Snapshot> const& snaps, int days) {
        if (snaps.empty()) return makeSnapshot();

        auto target = static_cast<int64_t>(std::time(nullptr)) - static_cast<int64_t>(days) * DAY_SECONDS;
        Snapshot best = snaps.front();
        for (auto const& snap : snaps) {
            if (snap.unixTime <= target) best = snap;
            else break;
        }
        return best;
    }

    bool hasFullWindow(std::vector<Snapshot> const& snaps, int days) {
        if (snaps.empty()) return false;
        auto target = static_cast<int64_t>(std::time(nullptr)) - static_cast<int64_t>(days) * DAY_SECONDS;
        return snaps.front().unixTime <= target;
    }

    int getValue(Snapshot const& snap, std::string const& statId) {
        auto it = snap.values.find(statId);
        if (it == snap.values.end()) return 0;
        return it->second;
    }

    int valueAtOrBefore(std::vector<Snapshot> const& snaps, std::string const& statId, int64_t unixTime) {
        if (snaps.empty()) return 0;
        int value = getValue(snaps.front(), statId);
        for (auto const& snap : snaps) {
            if (snap.unixTime <= unixTime) value = getValue(snap, statId);
            else break;
        }
        return value;
    }

    std::vector<Snapshot> loadSnapshotsWithCurrent() {
        auto snaps = loadSnapshots();
        auto current = makeSnapshot();

        if (snaps.empty() || current.unixTime > snaps.back().unixTime) {
            snaps.push_back(current);
        } else if (!snaps.empty()) {
            snaps.back() = current;
        }

        std::sort(snaps.begin(), snaps.end(), [](auto const& a, auto const& b) {
            return a.unixTime < b.unixTime;
        });
        return snaps;
    }

    std::vector<GraphPoint> buildTimelinePoints(
        std::vector<Snapshot> const& snaps,
        std::string const& statId,
        TimelineMode const& mode,
        GraphSettings const& settings
    ) {
        std::vector<GraphPoint> points;
        if (snaps.size() < 2) return points;

        auto firstTime = snaps.front().unixTime;
        auto end = snaps.back().unixTime;
        if (end <= firstTime) return points;

        int displayDays = clampDisplayDays(settings.displayDays);
        int64_t firstDay = dayIndexFor(firstTime, settings);
        int64_t endDay = dayIndexFor(end, settings);
        int64_t firstShownDay = std::max(firstDay, endDay - static_cast<int64_t>(displayDays) + 1);

        auto dayLabel = [&](int64_t day) {
            return fmt::format("D{}", static_cast<long long>(day - firstDay + 1));
        };

        if (mode.rollingAverage) {
            int windowDays = clampAvgWindowDays(settings.avgWindowDays);
            // Rolling average per day using configured day boundaries:
            // If windowDays=7: D7=(D1+...+D7)/7, D8=(D2+...+D8)/7, etc.
            // A day starts at 12:00 in the selected UTC offset.
            for (int64_t dayEnd = firstDay + windowDays - 1; dayEnd <= endDay; dayEnd++) {
                if (dayEnd < firstShownDay) continue;

                auto windowStart = dayStartForIndex(dayEnd - windowDays + 1, settings);
                auto windowEnd = std::min(dayStartForIndex(dayEnd + 1, settings), end);
                if (windowEnd <= windowStart) continue;

                int startValue = valueAtOrBefore(snaps, statId, windowStart);
                int endValue = valueAtOrBefore(snaps, statId, windowEnd);
                double value = static_cast<double>(endValue - startValue) / static_cast<double>(windowDays);

                points.push_back({dayLabel(dayEnd), value, windowStart, windowEnd});
            }
            return points;
        }

        if (mode.bucketDays <= 1) {
            // Daily buckets. The current in-progress day is included up to now.
            for (int64_t day = firstDay; day <= endDay; day++) {
                if (day < firstShownDay) continue;

                auto bucketStart = std::max(dayStartForIndex(day, settings), firstTime);
                auto bucketEnd = std::min(dayStartForIndex(day + 1, settings), end);
                if (bucketEnd <= bucketStart) continue;

                int startValue = valueAtOrBefore(snaps, statId, bucketStart);
                int endValue = valueAtOrBefore(snaps, statId, bucketEnd);
                double value = static_cast<double>(endValue - startValue);
                points.push_back({dayLabel(day), value, bucketStart, bucketEnd});
            }
            return points;
        }

        // Weekly buckets are 7 configured day-buckets, starting from the first
        // saved snapshot's day boundary. The final week may be partial up to now.
        int64_t bucketDays = static_cast<int64_t>(mode.bucketDays);
        int64_t bucketIndex = 0;
        for (int64_t bucketStartDay = firstDay; bucketStartDay <= endDay; bucketStartDay += bucketDays) {
            int64_t bucketEndDay = std::min(bucketStartDay + bucketDays - 1, endDay);
            bucketIndex++;
            if (bucketEndDay < firstShownDay) continue;

            auto bucketStart = std::max(dayStartForIndex(bucketStartDay, settings), firstTime);
            auto bucketEnd = std::min(dayStartForIndex(bucketEndDay + 1, settings), end);
            if (bucketEnd <= bucketStart) continue;

            int startValue = valueAtOrBefore(snaps, statId, bucketStart);
            int endValue = valueAtOrBefore(snaps, statId, bucketEnd);
            double value = static_cast<double>(endValue - startValue);

            points.push_back({fmt::format("W{}", static_cast<long long>(bucketIndex)), value, bucketStart, bucketEnd});
        }
        return points;
    }

    std::string timeAgoText(int64_t unixTime) {
        auto now = static_cast<int64_t>(std::time(nullptr));
        auto diff = std::max<int64_t>(0, now - unixTime);
        auto days = diff / DAY_SECONDS;
        if (days >= 1) return fmt::format("{} day{} ago", days, days == 1 ? "" : "s");
        auto hours = diff / 3600;
        if (hours >= 1) return fmt::format("{} hour{} ago", hours, hours == 1 ? "" : "s");
        auto minutes = diff / 60;
        return fmt::format("{} min ago", minutes);
    }

    int defaultStatIndex() {
        for (size_t i = 0; i < STATS.size(); i++) {
            if (STATS[i].id == "stars") return static_cast<int>(i);
        }
        return 0;
    }

    CCLabelBMFont* makeLabel(
        CCNode* parent,
        std::string const& text,
        char const* font,
        float scale,
        CCPoint pos,
        CCPoint anchor = ccp(0.5f, 0.5f)
    ) {
        auto label = CCLabelBMFont::create(text.c_str(), font);
        label->setScale(scale);
        label->setPosition(pos);
        label->setAnchorPoint(anchor);
        parent->addChild(label);
        return label;
    }

    bool getMouseGLPosition(CCPoint& out) {
#ifdef _WIN32
        auto view = CCEGLView::sharedOpenGLView();
        if (!view) return false;

        // Geode's CCEGLView wrapper does not expose getHWnd() in some SDK versions.
        // Since hover only matters while Geometry Dash is focused, use the foreground
        // Win32 window and convert the OS cursor position into GL coordinates.
        HWND hwnd = GetForegroundWindow();
        if (!hwnd) return false;

        POINT p;
        if (!GetCursorPos(&p)) return false;
        if (!ScreenToClient(hwnd, &p)) return false;

        auto winSize = CCDirector::sharedDirector()->getWinSize();
        auto viewport = view->getViewPortRect();
        float viewportW = viewport.size.width;
        float viewportH = viewport.size.height;

        if (viewportW <= 0.f || viewportH <= 0.f) {
            auto frameSize = view->getFrameSize();
            viewportW = frameSize.width;
            viewportH = frameSize.height;
            viewport.origin = ccp(0.f, 0.f);
        }

        if (viewportW <= 0.f || viewportH <= 0.f) return false;

        float x = (static_cast<float>(p.x) - viewport.origin.x) * winSize.width / viewportW;
        float y = winSize.height - (static_cast<float>(p.y) - viewport.origin.y) * winSize.height / viewportH;
        out = ccp(x, y);
        return true;
#else
        (void)out;
        return false;
#endif
    }

    CCNode* createStatButtonIcon() {
        // Try GD's native 3D star sprites first. Different GD/texture-pack builds
        // can expose slightly different names, so this checks a few common ones.
        const char* starNames[] = {
            "GJ_bigStar_001.png",
            "GJ_starsIcon_001.png",
            "GJ_starIcon_001.png",
            "GJ_bigStar2_001.png"
        };

        for (auto name : starNames) {
            if (auto spr = CCSprite::createWithSpriteFrameName(name)) {
                spr->setScale(0.62f);
                return spr;
            }
        }

        // Last-resort fallback: still keep it inside the blue circle.
        auto fallback = CCLabelBMFont::create("*", "bigFont.fnt");
        fallback->setScale(0.55f);
        fallback->setColor({255, 230, 50});
        return fallback;
    }

    CCNode* createBackArrowIcon() {
        const char* arrowNames[] = {
            "GJ_arrow_03_001.png",
            "GJ_arrow_01_001.png",
            "edit_leftBtn_001.png"
        };

        for (auto name : arrowNames) {
            if (auto spr = CCSprite::createWithSpriteFrameName(name)) {
                spr->setScale(0.70f);
                return spr;
            }
        }

        auto fallback = CCLabelBMFont::create("<", "bigFont.fnt");
        fallback->setScale(0.55f);
        fallback->setColor(ccc3(255, 230, 40));
        return fallback;
    }

    CCNode* createBackButtonSprite() {
        // Use only GD's purple arrow sprite, centered at the original popup close
        // button position. No green CircleButtonSprite base underneath.
        auto arrow = createBackArrowIcon();
        arrow->setAnchorPoint(ccp(0.5f, 0.5f));
        return arrow;
    }
}

class StatDeltaPopup : public Popup {
protected:
    enum class ViewMode {
        Summary,
        Graph,
        Settings
    };

    struct HoverHit {
        CCRect rect;
        CCPoint labelPos;
        std::string text;
    };

    ViewMode m_view = ViewMode::Summary;
    int m_statIndex = 0;
    int m_modeIndex = 0;
    int m_settingModeIndex = 0;
    GraphSettings m_settings;
    CCNode* m_contentRoot = nullptr;
    CCMenu* m_menu = nullptr;
    std::vector<HoverHit> m_hoverHits;
    CCLabelBMFont* m_hoverLabel = nullptr;
    CCLabelBMFont* m_hoverShadow = nullptr;

    bool init() {
        // Real popup size is 0.8x the old 460x330 window, but the content now
        // uses this smaller coordinate system directly. No double-scaling.
        if (!Popup::init(368.f, 264.f)) return false;

        this->setTitle("Net Stat Changes");
        addSnapshot(false);

        m_statIndex = defaultStatIndex();
        m_modeIndex = 0;
        m_settingModeIndex = 0;
        m_settings = loadGraphSettings();

        m_contentRoot = CCNode::create();
        m_contentRoot->setPosition(ccp(0.f, 0.f));
        m_mainLayer->addChild(m_contentRoot);

        m_menu = CCMenu::create();
        m_menu->setPosition(ccp(0.f, 0.f));
        m_mainLayer->addChild(m_menu);

        this->scheduleUpdate();
        this->rebuild();
        return true;
    }

    void refreshTopLeftButton() {
        // Summary/Graph keep the normal X close button. Settings uses a back arrow
        // in the same top-left area, so users return to Graph View instead of
        // closing the whole popup by accident.
        if (m_closeBtn) {
            m_closeBtn->setVisible(m_view != ViewMode::Settings);
        }
    }

    void addSettingsBackArrow() {
        auto backBtn = CCMenuItemSpriteExtra::create(
            createBackButtonSprite(),
            this,
            menu_selector(StatDeltaPopup::onBackFromSettings)
        );
        // Match the normal popup close button location as closely as possible.
        backBtn->setPosition(m_closeBtn ? m_closeBtn->getPosition() : ccp(15.f, 248.f));
        m_menu->addChild(backBtn, 50);
    }

    CCMenuItemSpriteExtra* addGameButton(
        std::string const& text,
        float visualScale,
        CCPoint position,
        SEL_MenuHandler callback
    ) {
        auto sprite = ButtonSprite::create(text.c_str());
        sprite->setScale(visualScale);

        // Use GD/Geode's normal button item so the press animation eases in/out
        // from the center, instead of swapping to a pre-scaled selected sprite.
        // This also prevents the "released outside while huge" selected-sprite bug.
        auto item = CCMenuItemSpriteExtra::create(sprite, this, callback);
        item->setPosition(position);
        m_menu->addChild(item);
        return item;
    }

    TextInput* addNumberInput(
        int currentValue,
        int minValue,
        int maxValue,
        CCPoint position,
        std::function<void(int)> onValue
    ) {
        auto input = TextInput::create(74.f, fmt::format("{}-{}", minValue, maxValue).c_str(), "bigFont.fnt");
        input->setString(std::to_string(currentValue));
        input->setFilter("0123456789");
        input->setMaxCharCount(2);
        input->setScale(0.84f);
        input->setPosition(position);
        input->setCallback([input, minValue, maxValue, onValue](std::string const& raw) {
            if (raw.empty()) return;

            int value = minValue;
            try {
                value = std::stoi(raw);
            } catch (...) {
                value = minValue;
            }

            value = std::max(minValue, std::min(maxValue, value));
            if (raw != std::to_string(value)) {
                input->setString(std::to_string(value));
            }
            onValue(value);
        });
        m_contentRoot->addChild(input, 10);
        return input;
    }

    TextInput* addSignedNumberInput(
        int currentValue,
        int minValue,
        int maxValue,
        CCPoint position,
        std::function<void(int)> onValue
    ) {
        auto input = TextInput::create(74.f, fmt::format("{}..{}", minValue, maxValue).c_str(), "bigFont.fnt");
        input->setString(std::to_string(currentValue));
        input->setFilter("-0123456789");
        input->setMaxCharCount(3);
        input->setScale(0.84f);
        input->setPosition(position);
        input->setCallback([input, minValue, maxValue, onValue](std::string const& raw) {
            if (raw.empty() || raw == "-") return;

            int value = minValue;
            try {
                value = std::stoi(raw);
            } catch (...) {
                value = minValue;
            }

            value = std::max(minValue, std::min(maxValue, value));
            if (raw != std::to_string(value)) {
                input->setString(std::to_string(value));
            }
            onValue(value);
        });
        m_contentRoot->addChild(input, 10);
        return input;
    }

    void rebuild() {
        if (!m_contentRoot || !m_menu) return;
        m_contentRoot->removeAllChildrenWithCleanup(true);
        m_menu->removeAllChildrenWithCleanup(true);
        m_hoverHits.clear();
        m_hoverLabel = nullptr;
        m_hoverShadow = nullptr;
        this->refreshTopLeftButton();

        if (m_view == ViewMode::Summary) {
            this->buildSummaryView();
        } else if (m_view == ViewMode::Graph) {
            this->buildGraphView();
        } else {
            this->buildSettingsView();
        }
    }

    void buildSummaryView() {
        constexpr int WINDOW_DAYS = 7;
        constexpr int DAILY_DAYS = 1;

        auto snaps = loadSnapshots();
        auto current = makeSnapshot();
        auto weekBase = pickBaseline(snaps, WINDOW_DAYS);
        auto dayBase = pickBaseline(snaps, DAILY_DAYS);
        bool weekFull = hasFullWindow(snaps, WINDOW_DAYS);
        bool dayFull = hasFullWindow(snaps, DAILY_DAYS);

        auto now = static_cast<int64_t>(std::time(nullptr));
        double elapsedDays = std::max(1.0 / 24.0, static_cast<double>(std::max<int64_t>(1, now - weekBase.unixTime)) / static_cast<double>(DAY_SECONDS));

        auto fmtAvg = [](double value) -> std::string {
            if (std::abs(value) < 0.05) return "0.0";
            return fmt::format("{:+.1f}", value);
        };

        std::string headerText;
        if (weekFull && dayFull) {
            headerText = "Changes from ~7 days and ~24 hours ago";
        } else if (dayFull) {
            headerText = fmt::format("7d history not ready; weekly uses {}", timeAgoText(weekBase.unixTime));
        } else {
            headerText = fmt::format("History still young; earliest snapshot is {}", timeAgoText(weekBase.unixTime));
        }

        makeLabel(m_contentRoot, headerText, "chatFont.fnt", 0.42f, ccp(184.f, 220.f));

        auto makeHead = [&](char const* text, float x) {
            auto label = makeLabel(m_contentRoot, text, "goldFont.fnt", 0.42f, ccp(x, 202.f), ccp(1.f, 0.5f));
            label->setColor(ccc3(255, 220, 70));
        };
        makeHead("7d", 208.f);
        makeHead("24h", 280.f);
        makeHead("Avg/d", 352.f);

        auto addDeltaLabel = [&](std::string const& text, float x, float y, bool positive, float scale = 0.32f) {
            auto deltaText = makeLabel(m_contentRoot, text, "bigFont.fnt", scale, ccp(x, y), ccp(1.f, 0.5f));
            deltaText->setColor(positive ? ccc3(90, 255, 90) : ccc3(255, 90, 90));
        };

        float y = 184.f;
        for (auto const& row : STATS) {
            int nowValue = getValue(current, row.id);
            int weekDelta = nowValue - getValue(weekBase, row.id);
            int dayDelta = nowValue - getValue(dayBase, row.id);
            double avgPerDay = static_cast<double>(weekDelta) / elapsedDays;

            auto label = makeLabel(m_contentRoot, row.name, "chatFont.fnt", 0.50f, ccp(32.f, y), ccp(0.f, 0.5f));
            label->setColor(ccc3(230, 230, 230));

            addDeltaLabel(fmtDelta(weekDelta), 208.f, y, weekDelta >= 0);
            addDeltaLabel(fmtDelta(dayDelta), 280.f, y, dayDelta >= 0);
            addDeltaLabel(fmtAvg(avgPerDay), 352.f, y, avgPerDay >= 0.0, 0.30f);

            y -= 17.5f;
        }

        makeLabel(
            m_contentRoot,
            "Graph View: select one stat and one timeline at a time.",
            "chatFont.fnt",
            0.52f,
            ccp(184.f, 50.f)
        );

        this->addGameButton("Graph View", 0.62f, ccp(112.f, 16.f), menu_selector(StatDeltaPopup::onGraphView));
        this->addGameButton("Snapshot Now", 0.62f, ccp(256.f, 16.f), menu_selector(StatDeltaPopup::onSnapshotNow));
    }

    void buildGraphView() {
        m_settings = loadGraphSettings();
        auto stat = STATS[m_statIndex];
        auto mode = MODES[m_modeIndex];
        auto snaps = loadSnapshotsWithCurrent();
        auto points = buildTimelinePoints(snaps, stat.id, mode, m_settings);

        makeLabel(m_contentRoot, fmt::format("{} - {}", stat.name, modeDisplayName(mode, m_settings)), "goldFont.fnt", 0.40f, ccp(184.f, 226.f));

        int currentValue = snaps.empty() ? 0 : getValue(snaps.back(), stat.id);
        makeLabel(
            m_contentRoot,
            fmt::format("Current total: {} | History: {} | Showing {}d | Day starts 12:00 {}", fmtInt(currentValue), snaps.empty() ? "none" : timeAgoText(snaps.front().unixTime), clampDisplayDays(m_settings.displayDays), timezoneOffsetLabel(m_settings)),
            "chatFont.fnt",
            0.56f,
            ccp(184.f, 176.f)
        );

        this->addGameButton("< Stat", 0.62f, ccp(98.f, 197.f), menu_selector(StatDeltaPopup::onPrevStat));
        this->addGameButton("Mode", 0.62f, ccp(184.f, 197.f), menu_selector(StatDeltaPopup::onNextMode));
        this->addGameButton("Stat >", 0.62f, ccp(270.f, 197.f), menu_selector(StatDeltaPopup::onNextStat));

        this->addGameButton("Summary", 0.50f, ccp(68.f, 33.f), menu_selector(StatDeltaPopup::onSummaryView));
        this->addGameButton("Settings", 0.50f, ccp(184.f, 33.f), menu_selector(StatDeltaPopup::onSettingsView));
        this->addGameButton("Snapshot Now", 0.50f, ccp(300.f, 33.f), menu_selector(StatDeltaPopup::onSnapshotNow));

        auto footerLabel = makeLabel(m_contentRoot, "", "chatFont.fnt", 0.40f, ccp(184.f, 59.f));

        if (points.empty()) {
            if (mode.rollingAverage) {
                int windowDays = clampAvgWindowDays(m_settings.avgWindowDays);
                makeLabel(m_contentRoot, fmt::format("Need at least {} day buckets for rolling Avg/d.", windowDays), "bigFont.fnt", 0.34f, ccp(184.f, 128.f));
                makeLabel(m_contentRoot, fmt::format("D{} = average of the first {} day buckets.", windowDays, windowDays), "chatFont.fnt", 0.44f, ccp(184.f, 105.f));
            } else {
                makeLabel(m_contentRoot, "Need at least two snapshots to draw a timeline.", "bigFont.fnt", 0.34f, ccp(184.f, 128.f));
                makeLabel(m_contentRoot, "Use Snapshot Now, then check again after your stats change.", "chatFont.fnt", 0.44f, ccp(184.f, 105.f));
            }
            footerLabel->setString("Settings controls display days, Avg/d window, and day-boundary UTC offset.");
            return;
        }

        std::vector<GraphPoint> shown = points;
        size_t totalPoints = points.size();

        double minY = std::numeric_limits<double>::infinity();
        double maxY = -std::numeric_limits<double>::infinity();
        for (auto const& p : shown) {
            minY = std::min(minY, p.value);
            maxY = std::max(maxY, p.value);
        }
        minY = std::min(minY, 0.0);
        maxY = std::max(maxY, 0.0);
        if (std::abs(maxY - minY) < 1e-9) {
            maxY += 1.0;
            minY -= 1.0;
        }

        constexpr float graphX = 45.f;
        constexpr float graphY = 86.f;
        constexpr float graphW = 278.f;
        constexpr float graphH = 72.f;

        auto xFor = [&](size_t i) -> float {
            if (shown.size() <= 1) return graphX + graphW / 2.f;
            return graphX + graphW * static_cast<float>(i) / static_cast<float>(shown.size() - 1);
        };
        auto yFor = [&](double value) -> float {
            return graphY + graphH * static_cast<float>((value - minY) / (maxY - minY));
        };

        auto draw = CCDrawNode::create();
        m_contentRoot->addChild(draw);

        ccColor4F axisColor = {0.55f, 0.55f, 0.55f, 0.85f};
        ccColor4F zeroColor = {0.85f, 0.85f, 0.85f, 0.70f};
        auto linePreset = getColorPreset(m_settings.lineColor[m_modeIndex]);
        auto nodePreset = getColorPreset(m_settings.nodeColor[m_modeIndex]);
        ccColor4F lineColor = linePreset.color;
        ccColor4F dotColor = nodePreset.color;
        ccColor4B dotColorB = ccc4(
            static_cast<GLubyte>(std::round(dotColor.r * 255.f)),
            static_cast<GLubyte>(std::round(dotColor.g * 255.f)),
            static_cast<GLubyte>(std::round(dotColor.b * 255.f)),
            static_cast<GLubyte>(std::round(dotColor.a * 255.f))
        );

        draw->drawSegment(ccp(graphX, graphY), ccp(graphX + graphW, graphY), 0.8f, axisColor);
        draw->drawSegment(ccp(graphX, graphY), ccp(graphX, graphY + graphH), 0.8f, axisColor);
        draw->drawSegment(ccp(graphX + graphW, graphY), ccp(graphX + graphW, graphY + graphH), 0.5f, axisColor);
        draw->drawSegment(ccp(graphX, graphY + graphH), ccp(graphX + graphW, graphY + graphH), 0.5f, axisColor);

        float zeroY = yFor(0.0);
        draw->drawSegment(ccp(graphX, zeroY), ccp(graphX + graphW, zeroY), 0.8f, zeroColor);

        for (size_t i = 1; i < shown.size(); i++) {
            draw->drawSegment(
                ccp(xFor(i - 1), yFor(shown[i - 1].value)),
                ccp(xFor(i), yFor(shown[i].value)),
                1.5f,
                lineColor
            );
        }

        for (size_t i = 0; i < shown.size(); i++) {
            float px = xFor(i);
            float py = yFor(shown[i].value);

            auto square = CCLayerColor::create(dotColorB, 5.f, 5.f);
            square->setPosition(ccp(px - 2.5f, py - 2.5f));
            m_contentRoot->addChild(square, 3);

            m_hoverHits.push_back({
                CCRectMake(px - 8.f, py - 8.f, 16.f, 16.f),
                ccp(px, py),
                fmt::format("{}: {}", shown[i].label, fmtGraphValue(shown[i].value, mode.averagePerDay))
            });
        }

        m_hoverShadow = makeLabel(m_contentRoot, "", "bigFont.fnt", 0.42f, ccp(184.f, 162.f));
        m_hoverShadow->setColor(ccc3(0, 0, 0));
        m_hoverShadow->setOpacity(190);
        m_hoverShadow->setVisible(false);

        m_hoverLabel = makeLabel(m_contentRoot, "", "bigFont.fnt", 0.42f, ccp(184.f, 164.f));
        m_hoverLabel->setColor(nodePreset.labelColor);
        m_hoverLabel->setVisible(false);

        auto yTop = makeLabel(m_contentRoot, fmtGraphValue(maxY, mode.averagePerDay), "chatFont.fnt", 0.28f, ccp(graphX - 8.f, graphY + graphH), ccp(1.f, 0.5f));
        yTop->setColor(ccc3(210, 210, 210));
        auto yZero = makeLabel(m_contentRoot, "0", "chatFont.fnt", 0.28f, ccp(graphX - 8.f, zeroY), ccp(1.f, 0.5f));
        yZero->setColor(ccc3(210, 210, 210));
        auto yBottom = makeLabel(m_contentRoot, fmtGraphValue(minY, mode.averagePerDay), "chatFont.fnt", 0.28f, ccp(graphX - 8.f, graphY), ccp(1.f, 0.5f));
        yBottom->setColor(ccc3(210, 210, 210));

        for (size_t i = 0; i < shown.size(); i++) {
            bool labelThis = shown.size() <= 7 || i == 0 || i == shown.size() - 1 || i == shown.size() / 2;
            if (!labelThis) continue;
            auto label = makeLabel(m_contentRoot, shown[i].label, "chatFont.fnt", 0.30f, ccp(xFor(i), graphY - 12.f));
            label->setColor(ccc3(210, 210, 210));
        }

        auto last = shown.back();
        auto lastLabel = fmt::format("Latest {}: {}", mode.shortName, fmtGraphValue(last.value, mode.averagePerDay));
        makeLabel(m_contentRoot, lastLabel, "bigFont.fnt", 0.40f, ccp(184.f, 78.f));

        std::string footer;
        if (mode.rollingAverage) {
            int windowDays = clampAvgWindowDays(m_settings.avgWindowDays);
            footer = fmt::format("Rolling Avg/d uses {}-day buckets; D{} is the first full window.", windowDays, windowDays);
        } else if (mode.bucketDays == 1) {
            footer = "Daily buckets reset at 12:00 in the selected UTC offset.";
        } else {
            footer = "Weekly buckets use 7 of those configured day buckets.";
        }
        footer += fmt::format(" Showing last {} days. Hover a square for value.", clampDisplayDays(m_settings.displayDays));
        footerLabel->setString(footer.c_str());
    }

    void buildSettingsView() {
        m_settings = loadGraphSettings();
        auto mode = MODES[m_settingModeIndex];
        auto linePreset = getColorPreset(m_settings.lineColor[m_settingModeIndex]);
        auto nodePreset = getColorPreset(m_settings.nodeColor[m_settingModeIndex]);

        this->addSettingsBackArrow();

        makeLabel(m_contentRoot, "Graph Settings", "goldFont.fnt", 0.58f, ccp(184.f, 226.f));
        makeLabel(m_contentRoot, "Use the top-left arrow to return to Graph View.", "chatFont.fnt", 0.50f, ccp(184.f, 208.f));

        auto addRow = [&](std::string const& name, std::string const& value, float y, ccColor3B color = ccc3(255, 255, 255), float valueScale = 0.43f) {
            auto left = makeLabel(m_contentRoot, name, "chatFont.fnt", 0.60f, ccp(48.f, y), ccp(0.f, 0.5f));
            left->setColor(ccc3(230, 230, 230));
            auto val = makeLabel(m_contentRoot, value, "bigFont.fnt", valueScale, ccp(206.f, y));
            val->setColor(color);
        };

        addRow("Displayed days", "", 186.f);
        addRow("Avg/d window", "", 160.f);
        addRow("UTC offset", "", 134.f);
        addRow("Mode colors", modeDisplayName(mode, m_settings), 108.f, ccc3(255, 230, 70), 0.32f);
        addRow("Line color", linePreset.name, 82.f, linePreset.labelColor);
        addRow("Node color", nodePreset.name, 56.f, nodePreset.labelColor);

        this->addNumberInput(
            clampDisplayDays(m_settings.displayDays),
            3,
            40,
            ccp(206.f, 186.f),
            [this](int value) {
                m_settings.displayDays = clampDisplayDays(value);
                saveGraphSettings(m_settings);
            }
        );

        this->addNumberInput(
            clampAvgWindowDays(m_settings.avgWindowDays),
            2,
            40,
            ccp(206.f, 160.f),
            [this](int value) {
                m_settings.avgWindowDays = clampAvgWindowDays(value);
                saveGraphSettings(m_settings);
            }
        );

        this->addSignedNumberInput(
            clampTimezoneOffsetHours(m_settings.timezoneOffsetHours),
            -12,
            14,
            ccp(206.f, 134.f),
            [this](int value) {
                m_settings.timezoneOffsetHours = clampTimezoneOffsetHours(value);
                saveGraphSettings(m_settings);
            }
        );

        this->addGameButton("Next", 0.44f, ccp(312.f, 108.f), menu_selector(StatDeltaPopup::onSettingsNextMode));
        this->addGameButton("Change", 0.44f, ccp(312.f, 82.f), menu_selector(StatDeltaPopup::onNextLineColor));
        this->addGameButton("Change", 0.44f, ccp(312.f, 56.f), menu_selector(StatDeltaPopup::onNextNodeColor));

        auto note = makeLabel(
            m_contentRoot,
            "Days reset at 12:00 in the selected UTC offset. Defaults to UTC+8.",
            "chatFont.fnt",
            0.48f,
            ccp(184.f, 30.f)
        );
        note->setColor(ccc3(235, 235, 235));
    }

    void update(float) override {
        if (m_view != ViewMode::Graph || !m_hoverLabel || !m_hoverShadow || !m_contentRoot) {
            if (m_hoverLabel) m_hoverLabel->setVisible(false);
            if (m_hoverShadow) m_hoverShadow->setVisible(false);
            return;
        }

        CCPoint mouse;
        if (!getMouseGLPosition(mouse)) {
            m_hoverLabel->setVisible(false);
            m_hoverShadow->setVisible(false);
            return;
        }

        auto localMouse = m_contentRoot->convertToNodeSpace(mouse);
        for (auto const& hit : m_hoverHits) {
            if (hit.rect.containsPoint(localMouse)) {
                CCPoint pos = ccp(hit.labelPos.x, hit.labelPos.y + 18.f);
                pos.x = std::max(54.f, std::min(314.f, pos.x));
                pos.y = std::max(104.f, std::min(166.f, pos.y));

                m_hoverShadow->setString(hit.text.c_str());
                m_hoverShadow->setPosition(ccp(pos.x + 1.f, pos.y - 1.f));
                m_hoverShadow->setVisible(true);

                m_hoverLabel->setString(hit.text.c_str());
                m_hoverLabel->setPosition(pos);
                m_hoverLabel->setVisible(true);
                return;
            }
        }

        m_hoverLabel->setVisible(false);
        m_hoverShadow->setVisible(false);
    }

    void onBackFromSettings(CCObject*) {
        m_modeIndex = m_settingModeIndex;
        m_settings = loadGraphSettings();
        m_view = ViewMode::Graph;
        this->rebuild();
    }

    void onGraphView(CCObject*) {
        if (m_view == ViewMode::Settings) {
            m_modeIndex = m_settingModeIndex;
        }
        m_settings = loadGraphSettings();
        m_view = ViewMode::Graph;
        this->rebuild();
    }

    void onSettingsView(CCObject*) {
        m_settingModeIndex = m_modeIndex;
        m_settings = loadGraphSettings();
        m_view = ViewMode::Settings;
        this->rebuild();
    }

    void onSummaryView(CCObject*) {
        m_view = ViewMode::Summary;
        this->rebuild();
    }

    void onPrevStat(CCObject*) {
        m_statIndex--;
        if (m_statIndex < 0) m_statIndex = static_cast<int>(STATS.size()) - 1;
        m_view = ViewMode::Graph;
        this->rebuild();
    }

    void onNextStat(CCObject*) {
        m_statIndex++;
        if (m_statIndex >= static_cast<int>(STATS.size())) m_statIndex = 0;
        m_view = ViewMode::Graph;
        this->rebuild();
    }

    void onNextMode(CCObject*) {
        m_modeIndex++;
        if (m_modeIndex >= static_cast<int>(MODES.size())) m_modeIndex = 0;
        m_view = ViewMode::Graph;
        this->rebuild();
    }


    void onLessDays(CCObject*) {
        m_settings.displayDays = clampDisplayDays(m_settings.displayDays - 1);
        saveGraphSettings(m_settings);
        m_view = ViewMode::Settings;
        this->rebuild();
    }

    void onMoreDays(CCObject*) {
        m_settings.displayDays = clampDisplayDays(m_settings.displayDays + 1);
        saveGraphSettings(m_settings);
        m_view = ViewMode::Settings;
        this->rebuild();
    }

    void onLessAvgWindow(CCObject*) {
        m_settings.avgWindowDays = clampAvgWindowDays(m_settings.avgWindowDays - 1);
        saveGraphSettings(m_settings);
        m_view = ViewMode::Settings;
        this->rebuild();
    }

    void onMoreAvgWindow(CCObject*) {
        m_settings.avgWindowDays = clampAvgWindowDays(m_settings.avgWindowDays + 1);
        saveGraphSettings(m_settings);
        m_view = ViewMode::Settings;
        this->rebuild();
    }

    void onSettingsNextMode(CCObject*) {
        m_settingModeIndex++;
        if (m_settingModeIndex >= static_cast<int>(MODES.size())) m_settingModeIndex = 0;
        m_view = ViewMode::Settings;
        this->rebuild();
    }

    void onNextLineColor(CCObject*) {
        m_settings.lineColor[m_settingModeIndex] = (clampColorIndex(m_settings.lineColor[m_settingModeIndex]) + 1) % static_cast<int>(COLOR_PRESETS.size());
        saveGraphSettings(m_settings);
        m_view = ViewMode::Settings;
        this->rebuild();
    }

    void onNextNodeColor(CCObject*) {
        m_settings.nodeColor[m_settingModeIndex] = (clampColorIndex(m_settings.nodeColor[m_settingModeIndex]) + 1) % static_cast<int>(COLOR_PRESETS.size());
        saveGraphSettings(m_settings);
        m_view = ViewMode::Settings;
        this->rebuild();
    }

    void onSnapshotNow(CCObject*) {
        addSnapshot(true);
        this->rebuild();
        FLAlertLayer::create(
            "Snapshot Saved",
            fmt::format("Saved current stats to:\n{}", snapshotPath().string()).c_str(),
            "OK"
        )->show();
    }

public:
    static StatDeltaPopup* create() {
        auto ret = new StatDeltaPopup();
        if (ret && ret->init()) {
            ret->autorelease();
            return ret;
        }
        CC_SAFE_DELETE(ret);
        return nullptr;
    }
};

class $modify(StatDeltaProfilePage, ProfilePage) {
    void loadPageFromUserInfo(GJUserScore* userScore) {
        ProfilePage::loadPageFromUserInfo(userScore);

        // Snapshot when the profile finishes loading, so opening your profile is enough
        // to keep the history fresh. This still respects the 12-hour cooldown.
        addSnapshot(false);

        if (!m_mainLayer) return;
        if (m_mainLayer->getChildByID("net-stat-changes-menu"_spr)) return;

        auto winSize = CCDirector::sharedDirector()->getWinSize();
        auto menu = CCMenu::create();
        menu->setID("net-stat-changes-menu"_spr);

        // Put the button directly under the existing left-side blue comment button.
        menu->setPosition(ccp(winSize.width / 2.f - 198.f, winSize.height / 2.f - 38.f));

        auto makeCircleButtonSprite = []() -> CCNode* {
            // Wrap the blue star circle in a fixed-size centered node. The old
            // version scaled the menu item itself, which made the native press
            // animation look off-center on this custom circle sprite. This keeps
            // the visual size the same but lets CCMenuItemSpriteExtra animate the
            // wrapper from the real center like normal GD buttons.
            auto holder = CCNode::create();
            holder->setContentSize(CCSizeMake(42.f, 42.f));
            holder->setAnchorPoint(ccp(0.5f, 0.5f));

            auto circle = CircleButtonSprite::create(
                createStatButtonIcon(),
                CircleBaseColor::Blue,
                CircleBaseSize::Small
            );
            if (circle) {
                circle->setTopRelativeScale(0.74f);
                circle->setScale(0.78f);
                circle->setAnchorPoint(ccp(0.5f, 0.5f));
                circle->setPosition(ccp(21.f, 21.f));
                holder->addChild(circle);
                return holder;
            }

            auto fallback = ButtonSprite::create("*");
            fallback->setScale(0.78f);
            fallback->setAnchorPoint(ccp(0.5f, 0.5f));
            fallback->setPosition(ccp(21.f, 21.f));
            holder->addChild(fallback);
            return holder;
        };

        // Use native GD-style button animation on a centered wrapper.
        auto button = CCMenuItemSpriteExtra::create(
            makeCircleButtonSprite(),
            this,
            menu_selector(StatDeltaProfilePage::onStatDelta)
        );
        button->setID("open-net-stat-changes"_spr);
        menu->addChild(button);
        m_mainLayer->addChild(menu, 1000);
    }

    void onStatDelta(CCObject*) {
        StatDeltaPopup::create()->show();
    }
};
