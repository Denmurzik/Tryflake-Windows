// Самостоятельный набор проверок физики и геометрии (порт Tests/SnowCoreChecks).
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <numeric>
#include <string>
#include <vector>

#include "../src/core/SnowCore.h"

using namespace snow;

static int failures = 0;
static int assertions = 0;

static void record(bool ok, const std::string& detail, int line) {
    ++assertions;
    if (!ok) {
        ++failures;
        std::printf("  FAIL line %d: %s\n", line, detail.c_str());
    }
}
#define EXPECT_TRUE(v) record((v), "Expected true: " #v, __LINE__)
#define EXPECT_FALSE(v) record(!(v), "Expected false: " #v, __LINE__)
#define EXPECT_EQ(a, b) record((a) == (b), #a " != " #b, __LINE__)
#define EXPECT_NEAR(a, b, eps) \
    record(std::abs((a) - (b)) <= (eps), #a " != " #b " (" + std::to_string(a) + " vs " + std::to_string(b) + ")", __LINE__)
#define EXPECT_GT(a, b) record((a) > (b), #a " must be > " #b " (" + std::to_string(a) + ")", __LINE__)
#define EXPECT_LT(a, b) record((a) < (b), #a " must be < " #b " (" + std::to_string(a) + ")", __LINE__)
#define EXPECT_LE(a, b) record((a) <= (b), #a " must be <= " #b " (" + std::to_string(a) + ")", __LINE__)

static double totalDepth(const SnowWorld& w) {
    double sum = 0;
    for (const auto& s : w.surfaces()) sum += std::accumulate(s.depths.begin(), s.depths.end(), 0.0);
    return sum;
}
static double maxDepth(const SnowWorld& w) {
    double m = 0;
    for (const auto& s : w.surfaces())
        for (double d : s.depths) m = std::max(m, d);
    return m;
}
static const SnowSurface* surface(const SnowWorld& w, uint32_t id) {
    for (const auto& s : w.surfaces())
        if (s.geometry.id == id) return &s;
    return nullptr;
}
static const SurfaceGeometry* find(const std::vector<SurfaceGeometry>& v, uint32_t id) {
    for (const auto& s : v)
        if (s.id == id) return &s;
    return nullptr;
}
static bool hasDebris(const SnowWorld& w) {
    for (const auto& p : w.particles())
        if (p.debris) return true;
    return false;
}
static double maxChunkWidth(const SnowWorld& w) {
    double m = 0;
    for (const auto& c : w.chunks()) m = std::max(m, c.width());
    return m;
}

// ----------------------------------------------------------------- геометрия

static void testTaskbarBackdropDoesNotHideOrdinaryWindows() {
    const Rect display{0, 0, 1710, 1107};
    for (auto [width, height] : std::vector<std::pair<double, double>>{{240, 180}, {640, 400}, {1280, 720}}) {
        // Прозрачный хост размером с экран (как Dock на macOS или оверлей поверх всего).
        std::vector<WindowCandidate> candidates = {
            {33, 1216, display, 1, OwnerKind::toolOverlay},
            {766, 1219, Rect{180, 170, width, height}, 1, OwnerKind::application}};
        auto selected = WindowSelection::select(candidates, 99, {display});
        auto surfaces = SurfaceResolver::resolve(selected, display.width, display.height);
        auto s = find(surfaces, 766);
        EXPECT_TRUE(s && std::any_of(s->exposed.begin(), s->exposed.end(), [](uint8_t e) { return e != 0; }));
        SnowWorld world(display.width, display.height);
        world.setSurfaces(surfaces);
        world.warmStart();
        for (int i = 0; i < 1200; ++i) world.update(1.0 / 60);
        auto acc = surface(world, 766);
        double sum = acc ? std::accumulate(acc->depths.begin(), acc->depths.end(), 0.0) : 0;
        EXPECT_GT(sum, 0.0);
    }
}

static void testVisibleTaskbarAndFullscreenApplicationRemainSurfaces() {
    const Rect display{0, 0, 1710, 1107};
    auto selected = WindowSelection::select({{1, 1, Rect{0, 1059, 1710, 48}, 1, OwnerKind::taskbar},
                                             {2, 2, display, 1, OwnerKind::application},
                                             {3, 99, display, 1, OwnerKind::application}},
                                            99, {display});
    EXPECT_EQ(selected.size(), size_t(2));
    if (selected.size() == 2) {
        EXPECT_EQ(selected[0].id, 1u);
        EXPECT_EQ(selected[1].id, 2u);
    }
    auto surfaces = SurfaceResolver::resolve(selected, display.width, display.height);
    EXPECT_TRUE(find(surfaces, 1) != nullptr);
}

static void testOverlayOnOffsetDisplayDoesNotHideWindows() {
    const Rect display{-1440, -600, 1440, 900};
    auto selected = WindowSelection::select({{1, 1, display, 1, OwnerKind::toolOverlay},
                                             {2, 2, Rect{-1200, -400, 600, 400}, 1, OwnerKind::application}},
                                            99, {display});
    EXPECT_EQ(selected.size(), size_t(1));
    if (!selected.empty()) EXPECT_EQ(selected[0].id, 2u);
}

static void testInvisibleAndSystemWindowsAreSkipped() {
    const Rect display{0, 0, 1920, 1080};
    auto selected = WindowSelection::select({{1, 1, Rect{100, 100, 500, 400}, 0.0, OwnerKind::application},
                                             {2, 2, display, 1, OwnerKind::ignoredSystem},
                                             {3, 3, Rect{100, 100, 30, 400}, 1, OwnerKind::application},
                                             {4, 4, Rect{300, 300, 400, 300}, 1, OwnerKind::application}},
                                            99, {display});
    EXPECT_EQ(selected.size(), size_t(1));
    if (!selected.empty()) EXPECT_EQ(selected[0].id, 4u);
}

static void testForegroundWindowClipsSnowOnBackgroundWindow() {
    auto surfaces = SurfaceResolver::resolve(
        {{1, Rect{120, 60, 200, 200}}, {2, Rect{40, 140, 400, 240}}}, 800, 600);
    auto back = find(surfaces, 2);
    EXPECT_TRUE(back != nullptr);
    if (!back) return;
    EXPECT_TRUE(back->exposed[back->bin(80)]);
    EXPECT_FALSE(back->exposed[back->bin(200)]);
    EXPECT_TRUE(back->exposed[back->bin(400)]);
}

static void testOffscreenAndTopEdgeWindowsHaveNoSnowSurface() {
    auto surfaces = SurfaceResolver::resolve(
        {{1, Rect{-500, 100, 200, 200}}, {2, Rect{0, 0, 800, 24}}}, 800, 600);
    EXPECT_EQ(surfaces.size(), size_t(1));
    if (!surfaces.empty()) EXPECT_EQ(surfaces[0].id, 0u);
}

static void testDisplayCoordinatesLeftAndAbovePrimaryWithScaling() {
    // Монитор слева-сверху от основного, масштаб 150%.
    const Rect monitor{-2160, -1350, 2160, 1350};
    Rect r = SurfaceResolver::localRect(Rect{-1800, -900, 900, 600}, monitor, 1.5);
    EXPECT_NEAR(r.x, 240.0, 1e-9);
    EXPECT_NEAR(r.y, 300.0, 1e-9);
    EXPECT_NEAR(r.width, 600.0, 1e-9);
    EXPECT_NEAR(r.height, 400.0, 1e-9);
}

static void testGroundDoesNotShowThroughForegroundWindows() {
    auto surfaces = SurfaceResolver::resolve({{8, Rect{0, 100, 400, 600}}}, 800, 600);
    auto ground = find(surfaces, 0);
    EXPECT_TRUE(ground != nullptr);
    if (!ground) return;
    EXPECT_FALSE(ground->exposed[20]);
    EXPECT_TRUE(ground->exposed[150]);
}

// ----------------------------------------------------------------- физика

static SnowWorld makeWorld(uint64_t seed = 42) {
    SnowWorld world(800, 600, seed);
    world.setSurfaces(SurfaceResolver::resolve({{1, Rect{40, 200, 700, 200}}}, 800, 600));
    return world;
}

static void testSnowAccumulatesAndStaysWithinDepthLimit() {
    auto world = makeWorld();
    world.warmStart();
    for (int i = 0; i < 3600; ++i) world.update(1.0 / 60);
    EXPECT_GT(totalDepth(world), 100.0);
    EXPECT_LE(maxDepth(world), world.configuration.maxDepth);
    EXPECT_LE(world.particles().size(), size_t(2400));
}

static void testSeededSimulationIsDeterministic() {
    auto a = makeWorld(), b = makeWorld();
    a.warmStart();
    b.warmStart();
    for (int i = 0; i < 180; ++i) {
        a.update(1.0 / 60);
        b.update(1.0 / 60);
    }
    bool same = a.particles().size() == b.particles().size();
    for (size_t i = 0; same && i < a.particles().size(); ++i)
        same = a.particles()[i].position == b.particles()[i].position;
    EXPECT_TRUE(same);
    bool sameDepths = a.surfaces().size() == b.surfaces().size();
    for (size_t i = 0; sameDepths && i < a.surfaces().size(); ++i)
        sameDepths = a.surfaces()[i].depths == b.surfaces()[i].depths;
    EXPECT_TRUE(sameDepths);
}

static void testWindChangesHorizontalTravel() {
    auto a = makeWorld(), b = makeWorld();
    a.warmStart();
    b.warmStart();
    b.configuration.wind.x = 1;
    a.update(1.0 / 60);
    b.update(1.0 / 60);
    EXPECT_NEAR(b.particles()[0].position.x - a.particles()[0].position.x, 160.0 / 60, 0.001);
}

static void testDeltaIsClampedAfterSleepAndRejectsInvalidTime() {
    auto world = makeWorld();
    world.update(3600);
    EXPECT_NEAR(world.elapsed(), 0.04, 0.00001);
    world.update(std::nan(""));
    world.update(-INFINITY);
    world.update(-1);
    EXPECT_NEAR(world.elapsed(), 0.04, 0.00001);
}

static void testSnowFollowsWindowMoveAndResizesWithoutReset() {
    auto world = makeWorld();
    world.seedSnow(12);
    const auto before = *surface(world, 1);
    world.setSurfaces({SurfaceGeometry(1, 80, 280, 350)});
    const auto& after = world.surfaces()[0];
    EXPECT_EQ(after.geometry.y, 280.0);
    EXPECT_EQ(after.depths.front(), before.depths.front());
    EXPECT_EQ(after.depths.back(), before.depths.back());
    EXPECT_GT(totalDepth(world), 0.0);
}

static void testCoveredBinsLoseSnow() {
    auto world = makeWorld();
    world.seedSnow();
    auto geometry = surface(world, 1)->geometry;
    geometry.exposed[10] = 0;
    world.setSurfaces({geometry});
    EXPECT_EQ(world.surfaces()[0].depths[10], 0.0);
}

static void testMittenClearsSnowAndCreatesShortLivedDebris() {
    auto world = makeWorld();
    world.seedSnow(16);
    const double before = totalDepth(world);
    world.brush(Point(200, 194), SnowTool::mitten);
    EXPECT_LT(totalDepth(world), before);
    EXPECT_TRUE(hasDebris(world));
    for (int i = 0; i < 450; ++i) world.update(1.0 / 60);
    EXPECT_FALSE(hasDebris(world));
}

static void testBrushDetachesPiecesWithMomentum() {
    auto world = makeWorld();
    world.seedSnow(18);
    world.brush(Point(200, 194), SnowTool::mitten, Point(500, 100));
    EXPECT_GT(world.chunks().size(), size_t(0));
    if (world.chunks().empty()) return;
    const SnowChunk first = world.chunks().front();
    EXPECT_GT(maxChunkWidth(world), 10.0);
    bool allValid = true;
    for (const auto& c : world.chunks()) allValid = allValid && c.area() > 0 && c.sourceSurfaceID == 1;
    EXPECT_TRUE(allValid);
    for (int i = 0; i < 15; ++i) world.update(1.0 / 60);
    const SnowChunk* fallen = nullptr;
    for (const auto& c : world.chunks())
        if (c.id == first.id) fallen = &c;
    EXPECT_TRUE(fallen != nullptr);
    if (!fallen) return;
    const SnowChunk snapshot = *fallen;
    EXPECT_GT(snapshot.position.y, first.position.y + 10);
    EXPECT_GT(snapshot.position.x, first.position.x);
    EXPECT_GT(std::abs(snapshot.angle - first.angle), 0.00001);
    world.setSurfaces({SurfaceGeometry(1, 90, 250, 600)});
    bool unchanged = false;
    for (const auto& c : world.chunks())
        if (c.id == snapshot.id) unchanged = c.position == snapshot.position;
    EXPECT_TRUE(unchanged);
}

static void testPiecesBreakOnLowerWindowAndExpire() {
    auto world = makeWorld();
    world.setSurfaces({SurfaceGeometry(1, 40, 200, 700), SurfaceGeometry(2, 40, 360, 700), SurfaceGeometry(0, 0, 598, 800)});
    world.seedSnow(18);
    world.brush(Point(200, 194), SnowTool::shovel);
    bool broke = false;
    for (int i = 0; i < 450; ++i) {
        world.update(1.0 / 60);
        for (const auto& c : world.chunks())
            if (c.generation == 1 && c.sourceSurfaceID == 2) broke = true;
    }
    EXPECT_TRUE(broke);
    EXPECT_TRUE(world.chunks().empty());
    EXPECT_FALSE(hasDebris(world));
}

static void testShovelDislodgesWiderStripThanMitten() {
    auto mitten = makeWorld(), shovel = makeWorld();
    mitten.seedSnow(18);
    shovel.seedSnow(18);
    const double before = totalDepth(mitten);
    mitten.brush(Point(350, 194), SnowTool::mitten);
    shovel.brush(Point(350, 194), SnowTool::shovel);
    const double mittenRemoved = before - totalDepth(mitten);
    const double shovelRemoved = before - totalDepth(shovel);
    EXPECT_GT(shovelRemoved, mittenRemoved * 1.5);
    EXPECT_GT(maxChunkWidth(shovel), 16.0);
}

static void testPieceBudgetAndClear() {
    auto world = makeWorld();
    for (int i = 0; i < 160; ++i) {
        world.seedSnow(18);
        world.brush(Point(350, 194), SnowTool::shovel);
    }
    EXPECT_GT(world.chunks().size(), size_t(0));
    EXPECT_LE(world.chunks().size(), size_t(128));
    world.clear();
    EXPECT_TRUE(world.chunks().empty());
    EXPECT_FALSE(hasDebris(world));
}

static void testNoAccumulationStillLetsSnowfallRun() {
    auto world = makeWorld();
    world.configuration.accumulation = false;
    world.warmStart();
    for (int i = 0; i < 600; ++i) world.update(1.0 / 60);
    EXPECT_EQ(totalDepth(world), 0.0);
    EXPECT_GT(world.particles().size(), size_t(0));
}

static void testRollingGrowsBallAndKeepsItOnSurface() {
    auto world = makeWorld();
    world.seedSnow(18);
    EXPECT_TRUE(world.beginDrag(Point(100, 193)));
    for (double x = 105; x <= 300; x += 5) world.drag(Point(x, 193));
    EXPECT_GT(world.balls()[0].radius, 9.0);
    world.endDrag();
    for (int i = 0; i < 60; ++i) world.update(1.0 / 60);
    EXPECT_TRUE(world.balls()[0].surfaceID == std::optional<uint32_t>(1u));
    EXPECT_LT(world.balls()[0].position.y, 200.0);
}

static void testThreeHandPlacedBallsBecomeSnowman() {
    auto world = makeWorld();
    auto makeBall = [&](double x, std::optional<Snowball> support) {
        world.seedSnow(18);
        const Point start(x, 193);
        EXPECT_TRUE(world.beginDrag(start));
        const Snowball ball = world.balls().back();
        if (support) {
            const double offsetY = ball.position.y - start.y;
            world.drag(Point(support->position.x, support->position.y - support->radius - ball.radius - offsetY));
        }
        world.endDrag();
        for (int i = 0; i < 4; ++i) world.update(1.0 / 60);
    };
    makeBall(350, std::nullopt);
    makeBall(100, world.balls()[0]);
    makeBall(150, world.balls()[1]);
    EXPECT_EQ(world.balls().size(), size_t(3));
    const auto heads = world.snowmanHeads();
    EXPECT_EQ(heads.size(), size_t(1));
    if (heads.size() == 1 && world.balls().size() == 3) EXPECT_EQ(heads[0].id, world.balls()[2].id);
}

static void testClearRemovesPilesBallsAndCapture() {
    auto world = makeWorld();
    world.seedSnow();
    EXPECT_TRUE(world.beginDrag(Point(150, 197)));
    world.clear();
    EXPECT_TRUE(world.balls().empty());
    EXPECT_FALSE(world.draggingID().has_value());
    EXPECT_EQ(totalDepth(world), 0.0);
    EXPECT_FALSE(world.hitTest(Point(150, 197)));
}

int main() {
    const std::vector<std::pair<const char*, std::function<void()>>> checks = {
        {"Taskbar-sized overlay with three window sizes", testTaskbarBackdropDoesNotHideOrdinaryWindows},
        {"Visible taskbar and fullscreen apps", testVisibleTaskbarAndFullscreenApplicationRemainSurfaces},
        {"Overlay on offset display", testOverlayOnOffsetDisplayDoesNotHideWindows},
        {"Invisible, tiny and system windows", testInvisibleAndSystemWindowsAreSkipped},
        {"Foreground occlusion", testForegroundWindowClipsSnowOnBackgroundWindow},
        {"Offscreen and top-edge filtering", testOffscreenAndTopEdgeWindowsHaveNoSnowSurface},
        {"Multiple display coordinates and DPI", testDisplayCoordinatesLeftAndAbovePrimaryWithScaling},
        {"Ground occlusion", testGroundDoesNotShowThroughForegroundWindows},
        {"Accumulation and bounded particles", testSnowAccumulatesAndStaysWithinDepthLimit},
        {"Deterministic simulation", testSeededSimulationIsDeterministic},
        {"Wind travel", testWindChangesHorizontalTravel},
        {"Sleep delta and invalid input", testDeltaIsClampedAfterSleepAndRejectsInvalidTime},
        {"Moving and resizing windows", testSnowFollowsWindowMoveAndResizesWithoutReset},
        {"Covered snow removal", testCoveredBinsLoseSnow},
        {"Mitten and temporary debris", testMittenClearsSnowAndCreatesShortLivedDebris},
        {"Detached pieces, gravity, rotation and momentum", testBrushDetachesPiecesWithMomentum},
        {"Pieces break on lower windows and expire", testPiecesBreakOnLowerWindowAndExpire},
        {"Shovel dislodges a wider strip", testShovelDislodgesWiderStripThanMitten},
        {"Piece budget and clearing", testPieceBudgetAndClear},
        {"Disabled accumulation", testNoAccumulationStillLetsSnowfallRun},
        {"Rolling and resting snowball", testRollingGrowsBallAndKeepsItOnSurface},
        {"Hand-built snowman", testThreeHandPlacedBallsBecomeSnowman},
        {"Clear and mouse capture", testClearRemovesPilesBallsAndCapture},
    };
    const auto start = std::chrono::steady_clock::now();
    for (const auto& [name, body] : checks) {
        const int before = failures;
        body();
        std::printf("%s %s\n", failures == before ? "PASS" : "FAIL", name);
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::printf("%zu checks, %d assertions, %d failures (%.3fs)\n", checks.size(), assertions, failures, seconds);
    return failures > 0 ? 1 : 0;
}
