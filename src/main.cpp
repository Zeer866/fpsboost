#include <Geode/Geode.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/modify/CCScheduler.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/CCNode.hpp>

using namespace geode::prelude;

static double g_current = 0.0;
static CCLabelBMFont* g_label = nullptr;

static bool on() { return Mod::get()->getSettingValue<bool>("enabled"); }
static bool lowDetail() { return on() && Mod::get()->getSettingValue<bool>("low-detail"); }
static double target() {
    return static_cast<double>(Mod::get()->getSettingValue<int64_t>("fps"));
}

static void setFPS(double fps) {
    if (fps < 30.0) fps = 30.0;
    g_current = fps;
    CCDirector::sharedDirector()->setAnimationInterval(1.0 / fps);
}

static void applyFPS() {
    if (on()) setFPS(target());
}

static void updateCounter() {
    bool show = on() && Mod::get()->getSettingValue<bool>("fps-counter");
    if (show && !g_label) {
        g_label = CCLabelBMFont::create("FPS: --", "bigFont.fnt");
        g_label->setAnchorPoint({0.f, 1.f});
        g_label->setScale(0.4f);
        g_label->setOpacity(180);
        auto size = CCDirector::sharedDirector()->getWinSize();
        g_label->setPosition({4.f, size.height - 4.f});
        g_label->setZOrder(10000);
        SceneManager::get()->keepAcrossScenes(g_label);
    }
    if (g_label) g_label->setVisible(show);
}

class $modify(FPSMenuLayer, MenuLayer) {
    bool init() {
        if (!MenuLayer::init()) return false;
        applyFPS();
        updateCounter();
        return true;
    }
};

class $modify(FPSScheduler, CCScheduler) {
    void update(float dt) {
        CCScheduler::update(dt);

        // Contador de FPS (cada 0.5 s)
        static float cacc = 0.f;
        static int cframes = 0;
        cacc += dt;
        cframes++;
        if (cacc >= 0.5f) {
            if (g_label && g_label->isVisible()) {
                int shown = static_cast<int>(cframes / cacc);
                g_label->setString(fmt::format("FPS: {}", shown).c_str());
            }
            cacc = 0.f;
            cframes = 0;
        }

        // Modo seguro (cada 2 s)
        static float acc = 0.f;
        static int frames = 0;
        acc += dt;
        frames++;
        if (acc < 2.f) return;

        double measured = frames / static_cast<double>(acc);
        acc = 0.f;
        frames = 0;

        if (!on() || !Mod::get()->getSettingValue<bool>("safe-mode")) return;
        if (g_current <= 0.0) return;

        if (measured < g_current * 0.88 && g_current > 30.0) {
            setFPS(std::max(30.0, measured * 1.05));
        } else if (measured >= g_current * 0.97 && g_current < target()) {
            setFPS(std::min(target(), g_current * 1.15));
        }
    }
};

class $modify(FPSGameLayer, GJBaseGameLayer) {
    CCParticleSystemQuad* createParticle(int a, char const* b, int c, tCCPositionType d) {
        auto p = GJBaseGameLayer::createParticle(a, b, c, d);
        if (p && lowDetail()) {
            p->stopSystem();
            p->setVisible(false);
        }
        return p;
    }
};

class $modify(FPSNode, CCNode) {
    CCAction* runAction(CCAction* action) {
        auto res = CCNode::runAction(action);
        if (lowDetail()) {
            if (auto rf = typeinfo_cast<CCRepeatForever*>(action)) {
                if (typeinfo_cast<CCRotateBy*>(rf->getInnerAction())) {
                    this->stopAction(action);
                }
            }
        }
        return res;
    }
};

$on_mod(Loaded) {
    listenForSettingChanges<int64_t>("fps", [](int64_t) { applyFPS(); });
    listenForSettingChanges<bool>("enabled", [](bool) { applyFPS(); updateCounter(); });
    listenForSettingChanges<bool>("fps-counter", [](bool) { updateCounter(); });
}
