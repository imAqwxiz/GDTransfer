// GDTransfer - import / export / share .gmd files without the GD servers
// Built for Geode v5.10.1 (C++23, non-templated Popup, async file picker)

#include <Geode/Geode.hpp>
#include <Geode/modify/LevelBrowserLayer.hpp>
#include <Geode/modify/EditLevelLayer.hpp>
#include <Geode/utils/file.hpp>
#include <Geode/utils/async.hpp>
#include <hjfod.gmd-api/include/GMD.hpp>

using namespace geode::prelude;

namespace {
    using PickResult = Result<std::optional<std::filesystem::path>>;

    // keep the icon inside a sane button size, fall back to a GD sprite if the png is missing
    CCSprite* fitIcon(CCSprite* spr, float size = 40.f) {
        if (!spr) spr = CCSprite::createWithSpriteFrameName("GJ_plusBtn_001.png");
        limitNodeSize(spr, { size, size }, 1.f, .1f);
        return spr;
    }

    // "My Level: v2?" -> "My Level_ v2_"
    std::string safeFileName(std::string name) {
        constexpr std::string_view bad = "\\/:*?\"<>|";
        for (auto& c : name) {
            if (bad.find(c) != std::string_view::npos) c = '_';
        }
        while (!name.empty() && (name.back() == ' ' || name.back() == '.')) name.pop_back();
        return name.empty() ? std::string("level") : name;
    }
}

// ---------------------------------------------------------------------------
// The popup: [Pick File] [Cancel]. Cancel (or the X) just closes it.
// ---------------------------------------------------------------------------
class GDTransferPopup : public Popup {
public:
    enum class Mode { Import, Export };

    static GDTransferPopup* create(Mode mode, GJGameLevel* level = nullptr) {
        auto ret = new GDTransferPopup();
        if (ret->init(mode, level)) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }

protected:
    Mode m_mode = Mode::Import;
    Ref<GJGameLevel> m_level;
    bool m_busy = false;
    std::optional<std::filesystem::path> m_pending;
    // destroying this (popup closed) automatically cancels a running picker
    async::TaskHolder<PickResult> m_picker;

    bool init(Mode mode, GJGameLevel* level) {
        if (!Popup::init(300.f, 170.f)) return false;

        m_mode = mode;
        m_level = level;

        std::string body;
        if (mode == Mode::Import) {
            this->setTitle("Import Level");
            body = "Pick a .gmd file to add\nto your My Levels.";
        }
        else {
            this->setTitle("Export Level");
            auto name = level ? static_cast<std::string>(level->m_levelName) : std::string("level");
            body = fmt::format("Save \"{}\" as a .gmd file.\nShare it anywhere, no GD servers.", name);
        }

        auto label = CCLabelBMFont::create(body.c_str(), "chatFont.fnt");
        label->setAlignment(kCCTextAlignmentCenter);
        label->setScale(.7f);
        m_mainLayer->addChildAtPosition(label, Anchor::Center, { 0.f, 12.f });

        auto pickBtn = CCMenuItemSpriteExtra::create(
            ButtonSprite::create(mode == Mode::Import ? "Pick File" : "Save As"),
            this, menu_selector(GDTransferPopup::onPick)
        );
        auto cancelBtn = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Cancel"),
            this, menu_selector(GDTransferPopup::onCancel)
        );
        m_buttonMenu->addChildAtPosition(pickBtn, Anchor::Bottom, { -62.f, 32.f });
        m_buttonMenu->addChildAtPosition(cancelBtn, Anchor::Bottom, { 62.f, 32.f });

        return true;
    }

    void onCancel(CCObject*) {
        this->onClose(nullptr);
    }

    void onPick(CCObject*) {
        if (m_busy) return;

        file::FilePickOptions options;
        options.filters = { { "Geometry Dash level (.gmd)", { "*.gmd" } } };

        auto pickMode = file::PickMode::OpenFile;
        if (m_mode == Mode::Export) {
            pickMode = file::PickMode::SaveFile;
            auto name = m_level ? static_cast<std::string>(m_level->m_levelName) : std::string("level");
            options.defaultPath = std::filesystem::path(safeFileName(name) + ".gmd");
        }

        m_busy = true;
        m_picker.spawn(
            file::pick(pickMode, options),
            [this](PickResult result) {
                m_busy = false;

                if (result.isErr()) {
                    FLAlertLayer::create(
                        "GDTransfer",
                        fmt::format("The file picker failed:\n{}", result.unwrapErr()),
                        "OK"
                    )->show();
                    return;
                }

                auto picked = result.unwrap();
                if (!picked) return; // system picker was cancelled, popup stays so they can retry / Cancel

                // don't tear the popup down from inside the picker's own callback,
                // finish on the next frame instead
                m_pending = *picked;
                this->scheduleOnce(schedule_selector(GDTransferPopup::onFinish), 0.f);
            }
        );
    }

    void onFinish(float) {
        Ref<GDTransferPopup> keepAlive = this; // popup closes itself below
        if (!m_pending) return;
        auto path = *m_pending;
        m_pending.reset();

        if (m_mode == Mode::Import) this->doImport(path);
        else this->doExport(path);
    }

    void doImport(std::filesystem::path const& path) {
        auto res = gmd::importGmdAsLevel(path);
        if (res.isErr()) {
            FLAlertLayer::create(
                "Import failed",
                fmt::format("{}", res.unwrapErr()),
                "OK"
            )->show();
            return;
        }

        auto level = res.unwrap();
        if (!level) {
            FLAlertLayer::create("Import failed", "The file didn't contain a level.", "OK")->show();
            return;
        }

        // newest level goes to the top of My Levels
        LocalLevelManager::sharedState()->m_localLevels->insertObject(level, 0);
        // save now so a crash / force close doesn't lose the import
        AppDelegate::get()->trySaveGame(false);

        this->onClose(nullptr);

        // reopen My Levels so the new level shows up in the list
        auto scene = LevelBrowserLayer::scene(GJSearchObject::create(SearchType::MyLevels));
        CCDirector::sharedDirector()->replaceScene(CCTransitionFade::create(.5f, scene));
    }

    void doExport(std::filesystem::path path) {
        if (!m_level) {
            FLAlertLayer::create("Export failed", "No level to export.", "OK")->show();
            return;
        }

        if (utils::string::toLower(path.extension().string()) != ".gmd") {
            path += ".gmd";
        }

        auto res = gmd::exportLevelAsGmd(m_level, path);
        if (res.isErr()) {
            FLAlertLayer::create(
                "Export failed",
                fmt::format("{}", res.unwrapErr()),
                "OK"
            )->show();
            return;
        }

        this->onClose(nullptr);
        FLAlertLayer::create(
            "Exported!",
            fmt::format("Saved to:\n{}", path.string()),
            "OK"
        )->show();
    }
};

// ---------------------------------------------------------------------------
// Import button: My Levels screen
// ---------------------------------------------------------------------------
class $modify(GTBrowserLayer, LevelBrowserLayer) {
    bool init(GJSearchObject* search) {
        if (!LevelBrowserLayer::init(search)) return false;

        // only on "My Levels"
        if (!search || search->m_searchType != SearchType::MyLevels) return true;

        auto winSize = CCDirector::sharedDirector()->getWinSize();

        // top right corner, clear of the level list
        CCPoint pos = { winSize.width - 30.f, winSize.height - 30.f };

        auto btn = CCMenuItemSpriteExtra::create(
            fitIcon(CCSprite::create("import.png"_spr), 32.f),
            this, menu_selector(GTBrowserLayer::onGTImport)
        );
        btn->setID("import-button"_spr);

        auto menu = CCMenu::create();
        menu->setID("import-menu"_spr);
        menu->setPosition(pos);
        btn->setPosition({ 0.f, 0.f });
        menu->addChild(btn);
        this->addChild(menu);

        return true;
    }

    void onGTImport(CCObject*) {
        if (auto popup = GDTransferPopup::create(GDTransferPopup::Mode::Import)) {
            popup->show();
        }
    }
};

// ---------------------------------------------------------------------------
// Export button: level page (name / description screen), not the editor itself
// ---------------------------------------------------------------------------
class $modify(GTEditLevelLayer, EditLevelLayer) {
    bool init(GJGameLevel* level) {
        if (!EditLevelLayer::init(level)) return false;

        auto btn = CCMenuItemSpriteExtra::create(
            fitIcon(CCSprite::create("export.png"_spr)),
            this, menu_selector(GTEditLevelLayer::onGTExport)
        );
        btn->setID("export-button"_spr);

        // prefer GD's own side menu so it lines up with the other buttons
        auto actions = typeinfo_cast<CCMenu*>(this->getChildByID("level-actions-menu"));
        if (actions && actions->getLayout()) {
            actions->addChild(btn);
            actions->updateLayout();
        }
        else {
            auto winSize = CCDirector::sharedDirector()->getWinSize();
            auto menu = CCMenu::create();
            menu->setID("export-menu"_spr);
            menu->setPosition({ winSize.width - 40.f, winSize.height / 2.f + 40.f });
            btn->setPosition({ 0.f, 0.f });
            menu->addChild(btn);
            this->addChild(menu);
        }

        return true;
    }

    void onGTExport(CCObject*) {
        if (!m_level) return;
        if (auto popup = GDTransferPopup::create(GDTransferPopup::Mode::Export, m_level)) {
            popup->show();
        }
    }
};

