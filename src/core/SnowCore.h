// SnowCore — физика снега без зависимостей от Win32 и Direct3D.
// Координаты в логических единицах (DIP), начало сверху слева.
#pragma once

#include <cmath>
#include <cstdint>
#include <optional>
#include <vector>

namespace snow {

constexpr double kPi = 3.14159265358979323846;

struct Point {
    double x = 0, y = 0;
    constexpr Point() = default;
    constexpr Point(double x, double y) : x(x), y(y) {}
    double distance(Point p) const { return std::hypot(x - p.x, y - p.y); }
    bool operator==(const Point&) const = default;
};

struct Rect {
    double x = 0, y = 0, width = 0, height = 0;
    double maxX() const { return x + width; }
    double maxY() const { return y + height; }
    bool contains(Point p) const { return p.x >= x && p.x < maxX() && p.y >= y && p.y < maxY(); }
    bool operator==(const Rect&) const = default;
};

struct WindowFrame {
    uint32_t id = 0;
    Rect rect;
};

struct SurfaceGeometry {
    static constexpr double spacing = 4.0;
    uint32_t id = 0;
    double x = 0, y = 0, width = 0;
    std::vector<uint8_t> exposed;

    SurfaceGeometry() = default;
    SurfaceGeometry(uint32_t id, double x, double y, double width);
    int binCount() const { return static_cast<int>(exposed.size()); }
    /// Индекс ячейки под координатой x или -1.
    int bin(double px) const;
    bool operator==(const SurfaceGeometry&) const = default;
};

namespace SurfaceResolver {
/// Окна упорядочены спереди назад (как отдаёт EnumWindows).
std::vector<SurfaceGeometry> resolve(const std::vector<WindowFrame>& windows, double width, double height);
/// Экранные физические пиксели → логические единицы монитора (начало — его левый верхний угол).
inline Rect localRect(const Rect& screenPx, const Rect& monitorPx, double scale) {
    return Rect{(screenPx.x - monitorPx.x) / scale, (screenPx.y - monitorPx.y) / scale, screenPx.width / scale,
                screenPx.height / scale};
}
}

// ---------------------------------------------------------------- выбор окон

enum class OwnerKind {
    application,   // обычное окно приложения
    taskbar,       // панель задач (аналог Dock)
    toolOverlay,   // поверхностное tool-окно поверх всего (оверлеи других программ)
    ignoredSystem  // рабочий стол, обои, собственные окна
};

/// Метаданные окна без заголовков и содержимого.
struct WindowCandidate {
    uint32_t id = 0;
    uint32_t processID = 0;
    Rect rect;
    double alpha = 1;
    OwnerKind owner = OwnerKind::application;
};

namespace WindowSelection {
/// Сохраняет порядок спереди назад.
std::vector<WindowFrame> select(const std::vector<WindowCandidate>& candidates, uint32_t ownPID,
                                const std::vector<Rect>& displays);
}

// ---------------------------------------------------------------- мир

enum class Snowfall { light = 0, steady = 1, blizzard = 2 };
enum class SnowTool { mitten = 0, shovel = 1, snowball = 2 };

double birthRate(Snowfall s);
double fallSpeed(Snowfall s);

struct SnowConfiguration {
    Snowfall snowfall = Snowfall::steady;
    Point wind;
    bool accumulation = true;
    double maxDepth = 22;
    double quality = 1;
};

struct SnowParticle {
    Point position;
    Point velocity;
    double radius = 1;
    double opacity = 1;
    double phase = 0;
    bool debris = false;
    double lifetime = 0;
};

struct SnowSurface {
    SurfaceGeometry geometry;
    std::vector<double> depths;
    /// Верх сугроба под x; false, если ячейка закрыта или вне поверхности.
    bool height(double x, double& out) const;
};

struct Snowball {
    uint64_t id = 0;
    Point position;
    double radius = 7;
    Point velocity;
    std::optional<uint32_t> surfaceID;
    std::optional<uint64_t> supportingBall;
};

struct SnowChunkColumn {
    double x = 0, top = 0, bottom = 0;
};

/// Оторванный кусок сугроба в локальных координатах тела.
struct SnowChunk {
    uint64_t id = 0;
    Point position;
    Point velocity;
    std::vector<SnowChunkColumn> columns;
    double angle = 0;
    double angularVelocity = 0;
    double lifetime = 0;
    uint32_t sourceSurfaceID = 0;
    int generation = 0;

    double opacity() const;
    double width() const;
    double area() const;
    Point transformed(Point p) const;
    double bottom() const;
};

struct RandomSource {
    uint64_t state = 1;
    double unit() {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return static_cast<double>(state >> 11) / static_cast<double>(uint64_t(1) << 53);
    }
    double value(double lower, double upper) { return lower + unit() * (upper - lower); }
};

struct RemovedSnowSample {
    double x = 0, top = 0, bottom = 0;
};

struct SnowChunkImpact {
    Point position;
    uint32_t surfaceID = 0;
    double area = 0;
};

class SnowChunkSystem {
public:
    static constexpr size_t limit = 128;
    explicit SnowChunkSystem(uint64_t seed);
    const std::vector<SnowChunk>& chunks() const { return chunks_; }
    void clear() { chunks_.clear(); }
    void detach(const std::vector<RemovedSnowSample>& samples, uint32_t surfaceID, Point cursor, Point velocity, SnowTool tool);
    std::vector<SnowChunkImpact> update(double dt, const std::vector<SnowSurface>& surfaces, double width, double height,
                                        Point wind);

private:
    std::vector<SnowChunk> split(const SnowChunk& parent, uint32_t surfaceID);
    std::vector<SnowChunk> chunks_;
    RandomSource random_;
    uint64_t nextID_ = 1;
};

class SnowWorld {
public:
    SnowWorld(double width, double height, uint64_t seed = 0x57494E544552ull);

    const double width;
    const double height;
    SnowConfiguration configuration;

    const std::vector<SnowParticle>& particles() const { return particles_; }
    const std::vector<SnowSurface>& surfaces() const { return surfaces_; }
    const std::vector<Snowball>& balls() const { return balls_; }
    const std::vector<SnowChunk>& chunks() const { return chunkSystem_.chunks(); }
    double elapsed() const { return elapsed_; }
    std::optional<uint64_t> draggingID() const { return draggingID_; }

    void warmStart();
    void setSurfaces(const std::vector<SurfaceGeometry>& geometries);
    void seedSnow(double depth = 10);
    void clear();
    void update(double delta);
    void brush(Point p, SnowTool tool, Point velocity = Point());
    bool hitTest(Point p) const;
    bool beginDrag(Point p);
    void drag(Point p);
    void endDrag();
    std::vector<Snowball> snowmanHeads() const;

private:
    void spawn(bool scattered);
    bool deposit(Point before, const SnowParticle& particle);
    void settlePiles();
    void depositChunk(const SnowChunkImpact& impact);
    void burst(Point p);
    double capacity(uint32_t id, int bin) const;
    void updateBalls(double dt);
    bool dependsOn(const Snowball& ball, uint64_t ballID) const;
    const Snowball* findBall(uint64_t id) const;
    int ballIndex(uint64_t id) const;

    std::vector<SnowParticle> particles_;
    std::vector<SnowSurface> surfaces_;
    std::vector<Snowball> balls_;
    double elapsed_ = 0;
    std::optional<uint64_t> draggingID_;
    RandomSource random_;
    SnowChunkSystem chunkSystem_;
    double birthBudget_ = 0;
    uint64_t nextBallID_ = 1;
    Point lastDrag_;
    Point dragOffset_;
    double dragStartY_ = 0;
    bool dragLifted_ = false;
    int step_ = 0;
};

}  // namespace snow
