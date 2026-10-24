#include <Geode/Geode.hpp>
#include <Geode/modify/MenuLayer.hpp>

using namespace geode::prelude;

static void applyFPS() {
    auto mod = Mod::get();
    if (!mod->getSettingValue<bool>("enabled")) return;

    int64_t fps = mod->getSettingValue<int64_t>("fps");
    if (fps < 30) fps = 30;

    CCDirector::sharedDirector()->setAnimationInterval(1.0 / static_cast<double>(fps));
}

class $modify(FPSMenuLayer, MenuLayer) {
    bool init() {
        if (!MenuLayer::init()) return false;
        applyFPS();
        return true;
    }
};

$on_mod(Loaded) {
    listenForSettingChanges<int64_t>("fps", [](int64_t) { applyFPS(); });
    listenForSettingChanges<bool>("enabled", [](bool) { applyFPS(); });
}
