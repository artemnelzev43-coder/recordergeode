#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>

#include <deque>
#include <memory>
#include <thread>
#include <unordered_map>
#include <filesystem>
#include <fstream>
#include <ctime>
#include <cmath>
#include <cstdlib>

using namespace geode::prelude;

// ---------------------------------------------------------------- data

constexpr float FRAME_DT = 0.07f; // 7 centiseconds = GIF frame delay (~14 fps)

using Frame = std::shared_ptr<std::vector<uint8_t>>; // RGB, w*h*3

struct Attempt {
    std::string level;
    int number = 0;
    bool won = false;
    bool live = false;
    int percent = 0;
    int w = 0, h = 0;
    std::deque<Frame> frames;

    float duration() const { return frames.size() * FRAME_DT; }
};

// ---------------------------------------------------------------- recorder

class Recorder {
public:
    static Recorder& get() {
        static Recorder r;
        return r;
    }

    std::deque<std::shared_ptr<Attempt>> history;
    std::shared_ptr<Attempt> cur;
    float accum = 0.f;
    CCRenderTexture* rt = nullptr; // intentionally never released (avoid shutdown crash)

    size_t maxFrames() {
        return (size_t)(Mod::get()->getSettingValue<int64_t>("max-seconds") / FRAME_DT);
    }
    size_t maxAttempts() {
        return (size_t)Mod::get()->getSettingValue<int64_t>("max-attempts");
    }

    void begin(PlayLayer* pl) {
        std::string name = pl->m_level->m_levelName;
        if (!history.empty() && history.back()->level != name) history.clear();
        cur = std::make_shared<Attempt>();
        cur->level = name;
        cur->number = pl->m_attempts;
        accum = 0.f;
    }

    void finish(PlayLayer* pl) {
        if (!cur) return;
        if (cur->frames.size() >= 8) {
            cur->percent = cur->won ? 100 : (int)pl->getCurrentPercent();
            history.push_back(cur);
            while (history.size() > maxAttempts()) history.pop_front();
        }
        cur = nullptr;
    }

    void capture(PlayLayer* pl, float dt) {
        if (!cur) return;
        accum += dt;
        if (accum < FRAME_DT) return;
        accum -= FRAME_DT;
        if (accum > FRAME_DT) accum = 0.f;

        auto win = CCDirector::get()->getWinSize();
        if (!rt) {
            rt = CCRenderTexture::create(win.width, win.height);
            if (!rt) return;
            rt->retain();
        }
        rt->beginWithClear(0.f, 0.f, 0.f, 1.f);
        pl->visit();
        rt->end();

        CCImage* img = rt->newCCImage(true);
        if (!img) return;
        int sw = img->getWidth(), sh = img->getHeight();
        auto src = img->getData();
        if (src && sw > 0 && sh > 0) {
            int ow = (int)Mod::get()->getSettingValue<int64_t>("frame-width");
            int oh = std::max(2, (int)std::lround((double)ow * sh / sw));
            auto frame = std::make_shared<std::vector<uint8_t>>(ow * oh * 3);
            auto& dst = *frame;
            for (int y = 0; y < oh; ++y) {
                int sy = std::min(sh - 1, (int)((y + 0.5) * sh / oh));
                for (int x = 0; x < ow; ++x) {
                    int sx = std::min(sw - 1, (int)((x + 0.5) * sw / ow));
                    const uint8_t* p = src + ((size_t)sy * sw + sx) * 4;
                    uint8_t* d = &dst[((size_t)y * ow + x) * 3];
                    d[0] = p[0]; d[1] = p[1]; d[2] = p[2];
                }
            }
            if (cur->w != ow || cur->h != oh) { // settings changed mid-attempt
                cur->frames.clear();
                cur->w = ow; cur->h = oh;
            }
            cur->frames.push_back(frame);
            while (cur->frames.size() > maxFrames()) cur->frames.pop_front();
            cur->percent = (int)pl->getCurrentPercent();
        }
        img->release();
    }
};

// ---------------------------------------------------------------- GIF encoder

static void lzwEncode(std::vector<uint8_t> const& px, std::vector<uint8_t>& out) {
    out.push_back(8); // min code size
    if (px.empty()) { out.push_back(0); return; }
    std::vector<uint8_t> blk;
    blk.reserve(255);
    uint32_t buf = 0;
    int bits = 0, size = 9, next = 258;
    std::unordered_map<uint32_t, uint16_t> dict;
    dict.reserve(8192);

    auto flushBlk = [&] {
        if (!blk.empty()) {
            out.push_back((uint8_t)blk.size());
            out.insert(out.end(), blk.begin(), blk.end());
            blk.clear();
        }
    };
    auto emit = [&](int code) {
        buf |= (uint32_t)code << bits;
        bits += size;
        while (bits >= 8) {
            blk.push_back((uint8_t)(buf & 0xFF));
            buf >>= 8;
            bits -= 8;
            if (blk.size() == 255) flushBlk();
        }
    };

    emit(256);
    uint32_t prefix = px[0];
    for (size_t i = 1; i < px.size(); ++i) {
        uint8_t c = px[i];
        uint32_t key = (prefix << 8) | c;
        auto it = dict.find(key);
        if (it != dict.end()) { prefix = it->second; continue; }
        emit((int)prefix);
        if (next < 4096) {
            dict[key] = (uint16_t)next++;
            if (next > (1 << size) && size < 12) size++;
        } else {
            emit(256);
            dict.clear();
            size = 9;
            next = 258;
        }
        prefix = c;
    }
    emit((int)prefix);
    emit(257);
    if (bits > 0) blk.push_back((uint8_t)(buf & 0xFF));
    flushBlk();
    out.push_back(0);
}

static std::vector<uint8_t> encodeGif(Attempt const& at) {
    std::vector<uint8_t> o;
    auto u16 = [&](int v) { o.push_back(v & 0xFF); o.push_back((v >> 8) & 0xFF); };
    for (char c : std::string("GIF89a")) o.push_back((uint8_t)c);
    u16(at.w); u16(at.h);
    o.push_back(0xF7); o.push_back(0); o.push_back(0);
    // fixed 6x6x6 palette
    for (int i = 0; i < 256; ++i) {
        if (i < 216) {
            o.push_back((uint8_t)((i / 36) * 51));
            o.push_back((uint8_t)(((i / 6) % 6) * 51));
            o.push_back((uint8_t)((i % 6) * 51));
        } else { o.push_back(0); o.push_back(0); o.push_back(0); }
    }
    // loop forever
    const uint8_t loop[] = {0x21, 0xFF, 0x0B, 'N','E','T','S','C','A','P','E','2','.','0', 0x03, 0x01, 0x00, 0x00, 0x00};
    o.insert(o.end(), std::begin(loop), std::end(loop));

    std::vector<uint8_t> idx((size_t)at.w * at.h);
    for (size_t f = 0; f < at.frames.size(); ++f) {
        auto const& rgb = *at.frames[f];
        for (size_t i = 0; i < idx.size(); ++i) {
            int r = (rgb[i * 3] * 5 + 127) / 255;
            int g = (rgb[i * 3 + 1] * 5 + 127) / 255;
            int b = (rgb[i * 3 + 2] * 5 + 127) / 255;
            idx[i] = (uint8_t)(r * 36 + g * 6 + b);
        }
        bool last = f + 1 == at.frames.size();
        int delay = last ? 100 : 7; // pause 1s on the last frame (death/win)
        o.push_back(0x21); o.push_back(0xF9); o.push_back(4); o.push_back(0);
        u16(delay); o.push_back(0); o.push_back(0);
        o.push_back(0x2C); u16(0); u16(0); u16(at.w); u16(at.h); o.push_back(0);
        lzwEncode(idx, o);
    }
    o.push_back(0x3B);
    return o;
}

static std::filesystem::path downloadsDir() {
#if defined(GEODE_IS_ANDROID)
    return "/storage/emulated/0/Download";
#elif defined(GEODE_IS_WINDOWS)
    if (auto p = std::getenv("USERPROFILE")) return std::filesystem::path(p) / "Downloads";
#else
    if (auto p = std::getenv("HOME")) return std::filesystem::path(p) / "Downloads";
#endif
    return Mod::get()->getSaveDir();
}

// ---------------------------------------------------------------- popup

class RecordingsPopup : public Popup<> {
protected:
    std::vector<std::shared_ptr<Attempt>> m_items;
    int m_sel = 0;
    size_t m_frame = 0;
    float m_accum = 0.f;
    bool m_playing = true;
    CCSprite* m_view = nullptr;
    CCLabelBMFont* m_info = nullptr;
    ButtonSprite* m_playSpr = nullptr;

    bool setup() override {
        this->setTitle("Attempt recordings");

        auto& rec = Recorder::get();
        for (auto& a : rec.history) m_items.push_back(a);
        if (rec.cur && rec.cur->frames.size() >= 8) {
            auto snap = std::make_shared<Attempt>(*rec.cur);
            snap->live = true;
            m_items.push_back(snap);
        }
        m_sel = (int)m_items.size() - 1;

        if (m_items.empty()) {
            auto l = CCLabelBMFont::create("No recordings yet.\nPlay and die :)", "bigFont.fnt");
            l->setScale(0.5f);
            m_mainLayer->addChildAtPosition(l, Anchor::Center);
            return true;
        }

        auto bg = CCScale9Sprite::create("square02_001.png");
        bg->setContentSize({244.f, 139.f});
        bg->setColor({0, 0, 0});
        m_mainLayer->addChildAtPosition(bg, Anchor::Center, {0.f, 25.f});

        m_view = CCSprite::create();
        m_mainLayer->addChildAtPosition(m_view, Anchor::Center, {0.f, 25.f});

        m_info = CCLabelBMFont::create("", "goldFont.fnt");
        m_info->setScale(0.6f);
        m_mainLayer->addChildAtPosition(m_info, Anchor::Center, {0.f, -58.f});

        auto left = CCSprite::createWithSpriteFrameName("GJ_arrow_01_001.png");
        auto right = CCSprite::createWithSpriteFrameName("GJ_arrow_01_001.png");
        right->setFlipX(true);
        auto lb = CCMenuItemSpriteExtra::create(left, this, menu_selector(RecordingsPopup::onPrev));
        auto rb = CCMenuItemSpriteExtra::create(right, this, menu_selector(RecordingsPopup::onNext));
        m_buttonMenu->addChildAtPosition(lb, Anchor::Center, {-158.f, 25.f});
        m_buttonMenu->addChildAtPosition(rb, Anchor::Center, {158.f, 25.f});

        m_playSpr = ButtonSprite::create("Pause", 70, true, "goldFont.fnt", "GJ_button_01.png", 30.f, 0.7f);
        auto pb = CCMenuItemSpriteExtra::create(m_playSpr, this, menu_selector(RecordingsPopup::onPlay));
        m_buttonMenu->addChildAtPosition(pb, Anchor::Center, {-60.f, -95.f});

        auto saveSpr = ButtonSprite::create("Save GIF", 90, true, "goldFont.fnt", "GJ_button_02.png", 30.f, 0.7f);
        auto sb = CCMenuItemSpriteExtra::create(saveSpr, this, menu_selector(RecordingsPopup::onSave));
        m_buttonMenu->addChildAtPosition(sb, Anchor::Center, {60.f, -95.f});

        this->showFrame();
        this->refreshInfo();
        this->schedule(schedule_selector(RecordingsPopup::tick));
        return true;
    }

    void refreshInfo() {
        auto const& a = *m_items[m_sel];
        std::string res = a.live ? "in progress (" + std::to_string(a.percent) + "%)"
                        : a.won  ? "WIN"
                                 : "died at " + std::to_string(a.percent) + "%";
        char dur[16];
        std::snprintf(dur, sizeof dur, "%.1fs", a.duration());
        m_info->setString(("#" + std::to_string(a.number) + "  " + res + "  " + dur + "   [" +
                           std::to_string(m_sel + 1) + "/" + std::to_string(m_items.size()) + "]").c_str());
    }

    void showFrame() {
        auto const& a = *m_items[m_sel];
        if (a.frames.empty()) return;
        m_frame %= a.frames.size();
        auto const& rgb = *a.frames[m_frame];
        std::vector<uint8_t> rgba((size_t)a.w * a.h * 4);
        for (size_t i = 0; i < (size_t)a.w * a.h; ++i) {
            rgba[i * 4] = rgb[i * 3];
            rgba[i * 4 + 1] = rgb[i * 3 + 1];
            rgba[i * 4 + 2] = rgb[i * 3 + 2];
            rgba[i * 4 + 3] = 255;
        }
        auto tex = new CCTexture2D();
        if (tex->initWithData(rgba.data(), kCCTexture2DPixelFormat_RGBA8888, a.w, a.h,
                              CCSize((float)a.w, (float)a.h))) {
            tex->autorelease();
            m_view->setTexture(tex);
            m_view->setTextureRect(CCRect(0.f, 0.f, (float)a.w, (float)a.h));
            m_view->setScale(240.f / a.w);
        } else {
            tex->release();
        }
    }

    void tick(float dt) {
        if (!m_playing || m_items.empty()) return;
        m_accum += dt;
        bool moved = false;
        while (m_accum >= FRAME_DT) { m_accum -= FRAME_DT; m_frame++; moved = true; }
        if (moved) this->showFrame();
    }

    void switchTo(int dir) {
        if (m_items.empty()) return;
        m_sel = (m_sel + dir + (int)m_items.size()) % (int)m_items.size();
        m_frame = 0;
        m_accum = 0.f;
        this->showFrame();
        this->refreshInfo();
    }

    void onPrev(CCObject*) { this->switchTo(-1); }
    void onNext(CCObject*) { this->switchTo(1); }

    void onPlay(CCObject*) {
        m_playing = !m_playing;
        m_playSpr->setString(m_playing ? "Pause" : "Play");
    }

    void onSave(CCObject*) {
        if (m_items.empty()) return;
        auto at = std::make_shared<Attempt>(*m_items[m_sel]);
        auto dir = downloadsDir();
        auto fallback = Mod::get()->getSaveDir();

        std::string safe;
        for (char c : at->level) safe += std::isalnum((unsigned char)c) ? c : '_';
        if (safe.empty()) safe = "level";
        char stamp[32];
        std::time_t t = std::time(nullptr);
        std::strftime(stamp, sizeof stamp, "%Y%m%d_%H%M%S", std::localtime(&t));
        std::string name = "GD_" + safe + "_attempt" + std::to_string(at->number) + "_" + stamp + ".gif";

        Notification::create("Saving GIF...", NotificationIcon::Loading, 1.f)->show();

        std::thread([at, dir, fallback, name] {
            auto bytes = encodeGif(*at);
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            auto path = dir / name;
            bool ok = false;
            {
                std::ofstream f(path, std::ios::binary);
                if (f) { f.write((char const*)bytes.data(), (std::streamsize)bytes.size()); ok = f.good(); }
            }
            if (!ok) { // Downloads not writable -> mod save folder
                path = fallback / name;
                std::ofstream f(path, std::ios::binary);
                if (f) { f.write((char const*)bytes.data(), (std::streamsize)bytes.size()); ok = f.good(); }
            }
            Loader::get()->queueInMainThread([ok, path] {
                if (ok) Notification::create("Saved: " + path.filename().string(), NotificationIcon::Success)->show();
                else Notification::create("Failed to save GIF", NotificationIcon::Error)->show();
            });
        }).detach();
    }

public:
    static RecordingsPopup* create() {
        auto ret = new RecordingsPopup();
        if (ret->initAnchored(390.f, 270.f)) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }
};

// ---------------------------------------------------------------- hooks

class $modify(RecPlayLayer, PlayLayer) {
    void resetLevel() {
        Recorder::get().finish(this);
        PlayLayer::resetLevel();
        Recorder::get().begin(this);
    }

    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);
        Recorder::get().capture(this, dt);
    }

    void levelComplete() {
        PlayLayer::levelComplete();
        if (Recorder::get().cur) Recorder::get().cur->won = true;
    }

    void onQuit() {
        Recorder::get().finish(this);
        PlayLayer::onQuit();
    }
};

class $modify(RecPauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();

        auto spr = CCSprite::create("camera-btn.png"_spr);
        if (!spr) return;
        spr->setScale(0.8f);
        auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(RecPauseLayer::onRecordings));
        btn->setID("recordings-button"_spr);

        if (auto menu = typeinfo_cast<CCMenu*>(this->getChildByID("left-button-menu"))) {
            menu->addChild(btn);
            menu->updateLayout();
        } else {
            auto menu2 = CCMenu::create();
            menu2->addChild(btn);
            menu2->setPosition({45.f, 45.f});
            this->addChild(menu2);
        }
    }

    void onRecordings(CCObject*) {
        if (auto p = RecordingsPopup::create()) p->show();
    }
};
