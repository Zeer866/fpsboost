#include <Geode/Geode.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/CCScheduler.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/CCNode.hpp>
#include <Geode/modify/CCMotionStreak.hpp>
#include <Geode/modify/HardStreak.hpp>
#include <Geode/modify/GameObject.hpp>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>
#ifdef GEODE_IS_ANDROID
#include <dlfcn.h>
#endif

using namespace geode::prelude;

static double g_current = 0.0;
static CCLabelBMFont* g_label = nullptr;
static bool g_diagQueued = false;
static bool g_pending = false;
static bool g_pendingPopup = false;
static int g_wait = 0;

// ---------- Ajustes ----------
static bool flag(char const* k) { return Mod::get()->getSettingValue<bool>(k); }
static bool on() { return flag("enabled"); }

// -1 sin perfil, 0 bajo, 1 medio, 2 alto
static int profileTier() {
    if (!on() || !flag("auto-profile")) return -1;
    return static_cast<int>(Mod::get()->getSavedValue<int64_t>("tier", -1));
}

static bool lowDetail() { return on() && (flag("low-detail") || profileTier() == 0); }
static bool renderLite() {
    int t = profileTier();
    return on() && (flag("render-lite") || (t >= 0 && t <= 1));
}
static bool noTrails() {
    int t = profileTier();
    return on() && (flag("no-trails") || (t >= 0 && t <= 1));
}
static bool noGlow() { return on() && (flag("no-glow") || profileTier() == 0); }
static bool hideDeco() { return on() && flag("hide-deco"); }
static bool autoLDM() { return on() && (flag("auto-ldm") || profileTier() == 0); }
static bool forced() { return on() && flag("force-fps"); }
static double target() {
    return static_cast<double>(Mod::get()->getSettingValue<int64_t>("fps"));
}

// ---------- FPS ----------
static void setFPS(double fps) {
    if (fps < 30.0) fps = 30.0;
    g_current = fps;
    CCDirector::sharedDirector()->setAnimationInterval(1.0 / fps);
}

static void applyFPS() {
    if (on()) setFPS(target());
}

static void applyRender() {
    if (renderLite()) {
        CCDirector::sharedDirector()->setDepthTest(false);
    }
}

static void enforceFPS() {
    if (!forced()) return;
    double want = 1.0 / target();
    auto dir = CCDirector::sharedDirector();
    if (std::abs(dir->getAnimationInterval() - want) > 0.000001) {
        setFPS(target());
    }
}

// ---------- Contador ----------
static void updateCounter() {
    bool show = on() && flag("fps-counter");
    if (show && !g_label) {
        g_label = CCLabelBMFont::create("FPS: --", "bigFont.fnt");
        g_label->retain();
        g_label->setAnchorPoint({0.f, 1.f});
        g_label->setScale(0.4f);
        g_label->setOpacity(180);
        auto size = CCDirector::sharedDirector()->getWinSize();
        g_label->setPosition({4.f, size.height - 4.f});
    }
    if (g_label) g_label->setVisible(show);
}

// ---------- GPU (OpenCL, solo Android) ----------
struct GPUInfo {
    bool ok = false;
    std::string name = "?";
    std::string vendor = "?";
    std::string error;
    uint32_t units = 0;
    uint64_t mem = 0;
    double ms = -1.0;
};

#ifdef GEODE_IS_ANDROID
template <class F>
static F sym(void* h, char const* n) {
    return reinterpret_cast<F>(dlsym(h, n));
}

static GPUInfo probeGPU() {
    GPUInfo g;
    void* h = nullptr;
    for (auto p : {"libOpenCL.so", "/vendor/lib64/libOpenCL.so",
                   "/system/vendor/lib64/libOpenCL.so", "/system/lib64/libOpenCL.so",
                   "/vendor/lib/libOpenCL.so"}) {
        h = dlopen(p, RTLD_NOW);
        if (h) break;
    }
    if (!h) {
        g.error = "libOpenCL.so no disponible";
        return g;
    }

    auto getPlat = sym<int32_t (*)(uint32_t, void**, uint32_t*)>(h, "clGetPlatformIDs");
    auto getDev = sym<int32_t (*)(void*, uint64_t, uint32_t, void**, uint32_t*)>(h, "clGetDeviceIDs");
    auto getInfo = sym<int32_t (*)(void*, uint32_t, size_t, void*, size_t*)>(h, "clGetDeviceInfo");
    if (!getPlat || !getDev || !getInfo) {
        g.error = "funciones de OpenCL no encontradas";
        return g;
    }

    void* plat = nullptr;
    uint32_t np = 0;
    if (getPlat(1, &plat, &np) != 0 || np == 0) {
        g.error = "sin plataformas OpenCL";
        return g;
    }
    void* dev = nullptr;
    uint32_t nd = 0;
    if (getDev(plat, 4, 1, &dev, &nd) != 0 || nd == 0) {
        g.error = "sin GPU OpenCL";
        return g;
    }

    char b1[256] = {};
    if (getInfo(dev, 0x102B, sizeof(b1) - 1, b1, nullptr) == 0) g.name = b1;
    char b2[256] = {};
    if (getInfo(dev, 0x102C, sizeof(b2) - 1, b2, nullptr) == 0) g.vendor = b2;
    uint32_t cu = 0;
    getInfo(dev, 0x1002, sizeof(cu), &cu, nullptr);
    g.units = cu;
    uint64_t mem = 0;
    getInfo(dev, 0x101F, sizeof(mem), &mem, nullptr);
    g.mem = mem;
    g.ok = true;

    // Benchmark pequeño
    auto createCtx = sym<void* (*)(void const*, uint32_t, void* const*, void*, void*, int32_t*)>(h, "clCreateContext");
    auto createQ = sym<void* (*)(void*, void*, uint64_t, int32_t*)>(h, "clCreateCommandQueue");
    auto createProg = sym<void* (*)(void*, uint32_t, char const**, size_t const*, int32_t*)>(h, "clCreateProgramWithSource");
    auto build = sym<int32_t (*)(void*, uint32_t, void* const*, char const*, void*, void*)>(h, "clBuildProgram");
    auto createKernel = sym<void* (*)(void*, char const*, int32_t*)>(h, "clCreateKernel");
    auto createBuf = sym<void* (*)(void*, uint64_t, size_t, void*, int32_t*)>(h, "clCreateBuffer");
    auto setArg = sym<int32_t (*)(void*, uint32_t, size_t, void const*)>(h, "clSetKernelArg");
    auto enqueue = sym<int32_t (*)(void*, void*, uint32_t, size_t const*, size_t const*, size_t const*, uint32_t, void const*, void*)>(h, "clEnqueueNDRangeKernel");
    auto finish = sym<int32_t (*)(void*)>(h, "clFinish");
    if (!createCtx || !createQ || !createProg || !build || !createKernel || !createBuf || !setArg || !enqueue || !finish) {
        g.error = "benchmark no disponible";
        return g;
    }

    int32_t err = 0;
    void* ctx = createCtx(nullptr, 1, &dev, nullptr, nullptr, &err);
    if (!ctx || err != 0) {
        g.error = "no se pudo crear el contexto";
        return g;
    }
    void* q = createQ(ctx, dev, 0, &err);
    if (!q || err != 0) {
        g.error = "no se pudo crear la cola";
        return g;
    }
    char const* src =
        "__kernel void bench(__global float* d){"
        "int i=get_global_id(0);float x=d[i];"
        "for(int k=0;k<2000;k++){x=x*1.0001f+0.5f;x=sqrt(x)+0.1f;}"
        "d[i]=x;}";
    void* prog = createProg(ctx, 1, &src, nullptr, &err);
    if (!prog || err != 0 || build(prog, 1, &dev, nullptr, nullptr, nullptr) != 0) {
        g.error = "no se pudo compilar el kernel";
        return g;
    }
    void* k = createKernel(prog, "bench", &err);
    size_t n = 262144;
    void* buf = createBuf(ctx, 1, n * sizeof(float), nullptr, &err);
    if (!k || !buf || err != 0) {
        g.error = "no se pudo preparar el kernel";
        return g;
    }
    setArg(k, 0, sizeof(void*), &buf);

    auto t0 = std::chrono::steady_clock::now();
    enqueue(q, k, 1, nullptr, &n, nullptr, 0, nullptr, nullptr);
    finish(q);
    auto t1 = std::chrono::steady_clock::now();
    g.ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    return g;
}
#else
static GPUInfo probeGPU() {
    GPUInfo g;
    g.error = "OpenCL solo se usa en Android en esta version";
    return g;
}
#endif

static int tierFrom(GPUInfo const& g) {
#ifdef GEODE_IS_ANDROID
    if (!g.ok || g.ms < 0.0) return 0; // sin OpenCL: configuracion mas conservadora
    if (g.ms < 40.0) return 2;
    if (g.ms < 160.0) return 1;
    return 0;
#else
    (void)g;
    return 2;
#endif
}

static char const* tierName(int t) {
    return t >= 2 ? "alto" : (t == 1 ? "medio" : "bajo");
}

static void runDiagnostics(bool popup) {
    auto g = probeGPU();
    int t = tierFrom(g);
    Mod::get()->setSavedValue<int64_t>("tier", t);

    std::string text;
    if (g.ok) {
        std::string bench = g.ms >= 0.0 ? fmt::format("{:.1f} ms", g.ms) : std::string("no disponible");
        text = fmt::format(
            "GPU: {}\nFabricante: {}\nUnidades de calculo: {}\nMemoria: {} MB\nBenchmark: {}\nNivel: {}",
            g.name, g.vendor, g.units, g.mem / (1024 * 1024), bench, tierName(t));
    } else {
        text = fmt::format("OpenCL no disponible ({}).\nNivel asignado: {}", g.error, tierName(t));
    }
    log::info("[FPS Boost] {}", text);
    if (popup) {
        FLAlertLayer::create("Info de GPU", text, "OK")->show();
    }
    applyRender();
}

// ---------- Auto LDM ----------
template <class T>
static int toInt(T const& v) {
    if constexpr (requires { v.value(); }) {
        return static_cast<int>(v.value());
    } else {
        return static_cast<int>(v);
    }
}

// ---------- Hooks ----------
class $modify(FPSMenuLayer, MenuLayer) {
    bool init() {
        if (!MenuLayer::init()) return false;
        applyFPS();
        applyRender();
        updateCounter();
        if (!g_diagQueued) {
            g_diagQueued = true;
            int64_t t = Mod::get()->getSavedValue<int64_t>("tier", -1);
            if (t < 0) {
                g_pending = true;
                g_pendingPopup = flag("gpu-info-popup");
                g_wait = 0;
            } else {
                log::info("[FPS Boost] nivel de GPU guardado: {}", tierName(static_cast<int>(t)));
            }
        }
        return true;
    }
};

class $modify(FPSPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (autoLDM() && level) {
            int thr = static_cast<int>(Mod::get()->getSettingValue<int64_t>("auto-ldm-threshold"));
            if (thr <= 0 || toInt(level->m_objectCount) >= thr) {
                level->m_lowDetailMode = true;
                level->m_lowDetailModeToggled = true;
            }
        }
        return PlayLayer::init(level, useReplay, dontCreateObjects);
    }
};

class $modify(FPSScheduler, CCScheduler) {
    void update(float dt) {
        CCScheduler::update(dt);

        enforceFPS();

        // Diagnostico de GPU una vez que el menu ya cargo
        if (g_pending && ++g_wait > 90) {
            g_pending = false;
            runDiagnostics(g_pendingPopup);
        }

        if (g_label && g_label->isVisible()) {
            auto scene = CCDirector::sharedDirector()->getRunningScene();
            if (scene && g_label->getParent() != scene) {
                g_label->removeFromParentAndCleanup(false);
                scene->addChild(g_label, 100000);
            }
        }

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

        static float acc = 0.f;
        static int frames = 0;
        acc += dt;
        frames++;
        if (acc < 2.f) return;

        double measured = frames / static_cast<double>(acc);
        acc = 0.f;
        frames = 0;

        if (!on() || forced() || !flag("safe-mode")) return;
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

// Estela del jugador
class $modify(FPSStreak, CCMotionStreak) {
    void draw() {
        if (noTrails()) return;
        CCMotionStreak::draw();
    }
};

// Estela de la wave
class $modify(FPSHardStreak, HardStreak) {
    void updateStroke(float dt) {
        if (noTrails()) {
            this->clear();
            return;
        }
        HardStreak::updateStroke(dt);
    }
};

// Brillos y decoracion
class $modify(FPSObject, GameObject) {
    void addGlow(gd::string name) {
        if (noGlow()) return;
        GameObject::addGlow(name);
    }

    void activateObject() {
        GameObject::activateObject();
        if (hideDeco() && m_objectType == GameObjectType::Decoration) {
            this->setVisible(false);
        }
    }
};

$on_mod(Loaded) {
    listenForSettingChanges<int64_t>("fps", [](int64_t) { applyFPS(); });
    listenForSettingChanges<bool>("enabled", [](bool) { applyFPS(); applyRender(); updateCounter(); });
    listenForSettingChanges<bool>("force-fps", [](bool) { applyFPS(); });
    listenForSettingChanges<bool>("render-lite", [](bool) { applyRender(); });
    listenForSettingChanges<bool>("auto-profile", [](bool) { applyRender(); });
    listenForSettingChanges<bool>("fps-counter", [](bool) { updateCounter(); });
    listenForSettingChanges<bool>("run-benchmark", [](bool v) {
        if (!v) return;
        g_pending = true;
        g_pendingPopup = true;
        g_wait = 0;
        Mod::get()->setSettingValue<bool>("run-benchmark", false);
    });
}
