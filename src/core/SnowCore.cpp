#include "SnowCore.h"

#include <algorithm>
#include <limits>
#include <numeric>
#include <unordered_map>
#include <unordered_set>

namespace snow {

// ================================================================= геометрия

SurfaceGeometry::SurfaceGeometry(uint32_t id, double x, double y, double width) : id(id), x(x), y(y), width(width) {
    const int count = std::max(2, static_cast<int>(std::ceil(width / spacing)) + 1);
    exposed.assign(static_cast<size_t>(count), 1);
}

int SurfaceGeometry::bin(double px) const {
    if (!(px >= x && px <= x + width)) return -1;
    return std::min(binCount() - 1, std::max(0, static_cast<int>((px - x) / spacing)));
}

std::vector<SurfaceGeometry> SurfaceResolver::resolve(const std::vector<WindowFrame>& windows, double width,
                                                      double height) {
    std::vector<SurfaceGeometry> surfaces;
    std::vector<Rect> occluders;
    auto occluded = [&](Point p) {
        return std::any_of(occluders.begin(), occluders.end(), [&](const Rect& r) { return r.contains(p); });
    };
    for (const auto& window : windows) {
        const Rect& r = window.rect;
        if (!(r.maxX() > 0 && r.x < width && r.maxY() > 0 && r.y < height)) continue;
        if (r.y > 18 && r.y < height - 8 && r.width > 40) {
            // Исходные x/width сохраняются, чтобы снег не отрывался от окна, уходящего за край.
            SurfaceGeometry s(window.id, r.x, r.y, r.width);
            bool any = false;
            for (int i = 0; i < s.binCount(); ++i) {
                Point p(r.x + i * SurfaceGeometry::spacing, r.y - 0.5);
                s.exposed[i] = (p.x >= 0 && p.x <= width && !occluded(p)) ? 1 : 0;
                any = any || s.exposed[i];
            }
            if (any) surfaces.push_back(std::move(s));
        }
        occluders.push_back(r);
    }
    SurfaceGeometry ground(0, 0, height - 2, width);
    for (int i = 0; i < ground.binCount(); ++i) {
        Point p(i * SurfaceGeometry::spacing, height - 2.5);
        ground.exposed[i] = occluded(p) ? 0 : 1;
    }
    surfaces.push_back(std::move(ground));
    std::stable_sort(surfaces.begin(), surfaces.end(),
                     [](const SurfaceGeometry& a, const SurfaceGeometry& b) { return a.y < b.y; });
    return surfaces;
}

std::vector<WindowFrame> WindowSelection::select(const std::vector<WindowCandidate>& candidates, uint32_t ownPID,
                                                 const std::vector<Rect>& displays) {
    std::vector<WindowFrame> result;
    for (const auto& c : candidates) {
        if (c.processID == ownPID || c.rect.width <= 40 || c.rect.height <= 32 || c.alpha <= 0.01 ||
            c.owner == OwnerKind::ignoredSystem)
            continue;
        // Прозрачные хосты размером с экран (панель задач, оверлеи других программ)
        // не должны закрывать собой все окна под ними.
        if (c.owner == OwnerKind::taskbar || c.owner == OwnerKind::toolOverlay) {
            const double tolerance = 2;
            bool coversDisplay = std::any_of(displays.begin(), displays.end(), [&](const Rect& d) {
                return c.rect.x <= d.x + tolerance && c.rect.y <= d.y + tolerance &&
                       c.rect.maxX() >= d.maxX() - tolerance && c.rect.maxY() >= d.maxY() - tolerance;
            });
            if (coversDisplay) continue;
        }
        result.push_back(WindowFrame{c.id, c.rect});
    }
    return result;
}

// ================================================================= режимы

double birthRate(Snowfall s) {
    switch (s) {
        case Snowfall::light: return 24;
        case Snowfall::steady: return 65;
        case Snowfall::blizzard: return 155;
    }
    return 65;
}

double fallSpeed(Snowfall s) {
    switch (s) {
        case Snowfall::light: return 0.7;
        case Snowfall::steady: return 1;
        case Snowfall::blizzard: return 1.65;
    }
    return 1;
}

bool SnowSurface::height(double x, double& out) const {
    const int i = geometry.bin(x);
    if (i < 0 || !geometry.exposed[i]) return false;
    out = geometry.y - depths[i];
    return true;
}

// ================================================================= куски сугроба

double SnowChunk::opacity() const { return std::min(1.0, std::max(0.0, lifetime) / 0.55); }

double SnowChunk::width() const {
    if (columns.empty()) return 0;
    return columns.back().x - columns.front().x;
}

double SnowChunk::area() const {
    double total = 0;
    for (size_t i = 1; i < columns.size(); ++i) {
        const auto& a = columns[i - 1];
        const auto& b = columns[i];
        total += (b.x - a.x) * ((a.bottom - a.top) + (b.bottom - b.top)) * 0.5;
    }
    return total;
}

Point SnowChunk::transformed(Point p) const {
    const double c = std::cos(angle), s = std::sin(angle);
    return Point(position.x + p.x * c - p.y * s, position.y + p.x * s + p.y * c);
}

double SnowChunk::bottom() const {
    double value = -std::numeric_limits<double>::infinity();
    for (const auto& col : columns) {
        value = std::max(value, std::max(transformed(Point(col.x, col.top)).y, transformed(Point(col.x, col.bottom)).y));
    }
    return value;
}

SnowChunkSystem::SnowChunkSystem(uint64_t seed) { random_.state = seed == 0 ? 1 : seed; }

void SnowChunkSystem::detach(const std::vector<RemovedSnowSample>& samples, uint32_t surfaceID, Point cursor,
                             Point velocity, SnowTool tool) {
    size_t start = 0;
    const bool shovel = tool == SnowTool::shovel;
    while (start < samples.size() && chunks_.size() < limit) {
        const size_t count = static_cast<size_t>(random_.value(shovel ? 4 : 2, shovel ? 12 : 8));
        size_t end = start + 1;
        while (end < std::min(samples.size(), start + count) &&
               samples[end].x - samples[end - 1].x <= SurfaceGeometry::spacing + 0.01) {
            ++end;
        }
        const size_t first = start, last = end - 1;
        start = end;
        double volume = 0, minTop = std::numeric_limits<double>::infinity(),
               maxBottom = -std::numeric_limits<double>::infinity();
        for (size_t i = first; i <= last; ++i) {
            volume += std::max(0.0, samples[i].bottom - samples[i].top) * SurfaceGeometry::spacing;
            minTop = std::min(minTop, samples[i].top);
            maxBottom = std::max(maxBottom, samples[i].bottom);
        }
        if (volume < 8) continue;
        const auto& f = samples[first];
        const auto& l = samples[last];
        const double centerX = (f.x + l.x) * 0.5;
        const double centerY = (minTop + maxBottom) * 0.5;
        SnowChunk chunk;
        chunk.columns.push_back({f.x - 2 - centerX, f.top + 0.4 - centerY, f.bottom - centerY});
        for (size_t i = first; i <= last; ++i) {
            chunk.columns.push_back({samples[i].x - centerX, samples[i].top - centerY, samples[i].bottom - centerY});
        }
        chunk.columns.push_back({l.x + 2 - centerX, l.top + 0.4 - centerY, l.bottom - centerY});
        const double direction = centerX >= cursor.x ? 1.0 : -1.0;
        const double kickX = std::min(240.0, std::max(-240.0, velocity.x * 0.22)) + direction * random_.value(12, 45);
        const double kickY = std::min(130.0, std::max(-60.0, velocity.y * 0.15)) + random_.value(25, 70);
        const double angle = random_.value(-0.07, 0.07);
        const double spin = random_.value(-4, 4);
        chunk.id = nextID_++;
        chunk.position = Point(centerX, centerY);
        chunk.velocity = Point(kickX, kickY);
        chunk.angle = angle;
        chunk.angularVelocity = spin;
        chunk.lifetime = 5;
        chunk.sourceSurfaceID = surfaceID;
        chunk.generation = 0;
        chunks_.push_back(std::move(chunk));
    }
}

std::vector<SnowChunkImpact> SnowChunkSystem::update(double dt, const std::vector<SnowSurface>& surfaces, double width,
                                                     double height, Point wind) {
    std::vector<SnowChunk> survivors, fragments;
    std::vector<SnowChunkImpact> impacts;
    survivors.reserve(chunks_.size());
    for (auto chunk : chunks_) {
        const double beforeBottom = chunk.bottom();
        chunk.lifetime -= dt;
        chunk.velocity.x = (chunk.velocity.x + wind.x * 30 * dt) * std::exp(-0.12 * dt);
        chunk.velocity.y += 520 * dt;
        chunk.position.x += chunk.velocity.x * dt;
        chunk.position.y += chunk.velocity.y * dt;
        chunk.angle += chunk.angularVelocity * dt;
        if (!(chunk.lifetime > 0 && chunk.position.y < height + 80 && chunk.position.x > -100 &&
              chunk.position.x < width + 100))
            continue;
        const double afterBottom = chunk.bottom();
        // Проверка пересечения за шаг не даёт быстрым кускам проскочить тонкий край.
        const SnowSurface* landing = nullptr;
        double top = 0;
        for (const auto& surface : surfaces) {
            double h;
            if (surface.geometry.id == chunk.sourceSurfaceID || !surface.height(chunk.position.x, h)) continue;
            if (chunk.velocity.y > 0 && beforeBottom <= h && afterBottom >= h) {
                landing = &surface;
                top = h;
                break;
            }
        }
        if (landing) {
            chunk.position.y -= afterBottom - top;
            const long long remaining = static_cast<long long>(limit) - static_cast<long long>(survivors.size()) -
                                        static_cast<long long>(fragments.size());
            const bool canSplit = chunk.generation == 0 && chunk.columns.size() >= 4 && chunk.width() >= 12 &&
                                  landing->geometry.id != 0 && remaining >= 2;
            if (canSplit) {
                auto parts = split(chunk, landing->geometry.id);
                fragments.insert(fragments.end(), parts.begin(), parts.end());
            }
            impacts.push_back({Point(chunk.position.x, top), landing->geometry.id, chunk.area() * (canSplit ? 0.45 : 1)});
        } else {
            survivors.push_back(std::move(chunk));
        }
    }
    survivors.insert(survivors.end(), fragments.begin(), fragments.end());
    if (survivors.size() > limit) survivors.resize(limit);
    chunks_ = std::move(survivors);
    return impacts;
}

std::vector<SnowChunk> SnowChunkSystem::split(const SnowChunk& parent, uint32_t surfaceID) {
    const size_t cut = parent.columns.size() / 2;
    std::vector<std::vector<SnowChunkColumn>> sections = {
        std::vector<SnowChunkColumn>(parent.columns.begin(), parent.columns.begin() + cut + 1),
        std::vector<SnowChunkColumn>(parent.columns.begin() + cut, parent.columns.end())};
    std::vector<SnowChunk> result;
    for (const auto& section : sections) {
        if (section.empty() || !(section.back().x > section.front().x)) continue;
        const double centerX = (section.front().x + section.back().x) * 0.5;
        double minTop = std::numeric_limits<double>::infinity(), maxBottom = -std::numeric_limits<double>::infinity();
        for (const auto& c : section) {
            minTop = std::min(minTop, c.top);
            maxBottom = std::max(maxBottom, c.bottom);
        }
        const double centerY = (minTop + maxBottom) * 0.5;
        SnowChunk chunk;
        for (const auto& column : section) {
            const double middle = (column.top + column.bottom) * 0.5;
            const double half = (column.bottom - column.top) * 0.275;
            chunk.columns.push_back({column.x - centerX, middle - half - centerY, middle + half - centerY});
        }
        const double vx = parent.velocity.x * 0.25 + random_.value(-100, 100);
        const double vy = random_.value(-95, -30);
        const double spin = parent.angularVelocity + random_.value(-4, 4);
        chunk.id = nextID_++;
        chunk.position = parent.transformed(Point(centerX, centerY));
        chunk.velocity = Point(vx, vy);
        chunk.angle = parent.angle;
        chunk.angularVelocity = spin;
        chunk.lifetime = std::min(parent.lifetime, 1.8);
        chunk.sourceSurfaceID = surfaceID;
        chunk.generation = 1;
        result.push_back(std::move(chunk));
    }
    return result;
}

// ================================================================= мир

SnowWorld::SnowWorld(double width, double height, uint64_t seed)
    : width(std::max(1.0, width)), height(std::max(1.0, height)), chunkSystem_(seed ^ 0x534E4F574348554Eull) {
    random_.state = seed == 0 ? 1 : seed;
}

void SnowWorld::warmStart() {
    const int count = static_cast<int>(std::min(800.0, width * height / 4200) * configuration.quality);
    for (int i = 0; i < count; ++i) spawn(true);
}

void SnowWorld::setSurfaces(const std::vector<SurfaceGeometry>& geometries) {
    std::unordered_map<uint32_t, size_t> oldIndex;
    for (size_t i = 0; i < surfaces_.size(); ++i) oldIndex.emplace(surfaces_[i].geometry.id, i);
    std::vector<SnowSurface> next;
    next.reserve(geometries.size());
    for (const auto& geometry : geometries) {
        SnowSurface surface{geometry, std::vector<double>(static_cast<size_t>(geometry.binCount()), 0.0)};
        auto found = oldIndex.find(geometry.id);
        if (found != oldIndex.end()) {
            const auto& previous = surfaces_[found->second];
            const int prevCount = static_cast<int>(previous.depths.size());
            const int count = static_cast<int>(surface.depths.size());
            for (int i = 0; i < count; ++i) {
                // Пересэмплирование по относительной ширине при изменении размера окна.
                const int index = std::min(prevCount - 1, static_cast<int>(static_cast<double>(i) /
                                                                            static_cast<double>(std::max(1, count - 1)) *
                                                                            static_cast<double>(prevCount - 1)));
                surface.depths[i] = geometry.exposed[i] ? previous.depths[index] : 0;
            }
        }
        next.push_back(std::move(surface));
    }
    for (auto& ball : balls_) {
        if (!ball.surfaceID) continue;
        auto found = oldIndex.find(*ball.surfaceID);
        if (found == oldIndex.end()) continue;
        const auto& previous = surfaces_[found->second].geometry;
        auto current = std::find_if(next.begin(), next.end(),
                                    [&](const SnowSurface& s) { return s.geometry.id == *ball.surfaceID; });
        if (current != next.end()) {
            ball.position.x += current->geometry.x - previous.x;
            ball.position.y += current->geometry.y - previous.y;
        } else {
            ball.surfaceID.reset();
        }
    }
    surfaces_ = std::move(next);
}

void SnowWorld::seedSnow(double depth) {
    for (auto& s : surfaces_) {
        for (size_t i = 0; i < s.depths.size(); ++i) {
            const double wave = 0.72 + 0.18 * std::sin(i * 0.14) + 0.1 * std::cos(i * 0.37);
            s.depths[i] = s.geometry.exposed[i] ? std::min(configuration.maxDepth, depth * wave) : 0;
        }
    }
}

void SnowWorld::clear() {
    for (auto& s : surfaces_) std::fill(s.depths.begin(), s.depths.end(), 0.0);
    balls_.clear();
    draggingID_.reset();
    chunkSystem_.clear();
    std::erase_if(particles_, [](const SnowParticle& p) { return p.debris; });
}

void SnowWorld::update(double delta) {
    if (!std::isfinite(delta) || delta <= 0) return;
    const double dt = std::min(delta, 1.0 / 25);
    elapsed_ += dt;
    step_ += 1;
    const double area = std::min(3.5, std::max(0.22, width * height / 1'250'000));
    birthBudget_ += birthRate(configuration.snowfall) * area * configuration.quality * dt;
    const int births = static_cast<int>(birthBudget_);
    birthBudget_ -= births;
    const int limitCount = static_cast<int>(2400 * configuration.quality);
    const int room = std::max(0, limitCount - static_cast<int>(particles_.size()));
    for (int i = 0; i < std::min(births, room); ++i) spawn(false);

    const double speed = fallSpeed(configuration.snowfall);
    std::vector<SnowParticle> survivors;
    survivors.reserve(particles_.size());
    for (auto particle : particles_) {
        const Point before = particle.position;
        if (particle.debris) {
            particle.velocity.y += 220 * dt;
            particle.lifetime -= dt;
            particle.opacity = std::min(particle.opacity, particle.lifetime * 1.4);
            particle.position.x += particle.velocity.x * dt;
            particle.position.y += particle.velocity.y * dt;
        } else {
            const double sway = std::sin(elapsed_ * 0.8 + particle.phase) * (9 + particle.radius * 4);
            particle.position.x += (particle.velocity.x + configuration.wind.x * 160 + sway) * dt;
            particle.position.y += std::max(10.0, particle.velocity.y * speed + configuration.wind.y * 45) * dt;
            if (particle.position.x < -12) particle.position.x = width + 10;
            if (particle.position.x > width + 12) particle.position.x = -10;
            if (configuration.accumulation && deposit(before, particle)) continue;
        }
        if (particle.position.y < height + 20 && particle.lifetime > 0) survivors.push_back(particle);
    }
    particles_ = std::move(survivors);
    if (step_ % 4 == 0) settlePiles();
    updateBalls(dt);
    const auto impacts = chunkSystem_.update(dt, surfaces_, width, height, configuration.wind);
    for (const auto& impact : impacts) {
        if (configuration.accumulation) depositChunk(impact);
        const int puffs = std::min(4, std::max(1, static_cast<int>(impact.area / 40)));
        for (int i = 0; i < puffs; ++i) burst(impact.position);
    }
}

void SnowWorld::brush(Point p, SnowTool tool, Point velocity) {
    if (tool == SnowTool::snowball || p.x < 0 || p.x > width || p.y < 0 || p.y > height) return;
    const double radius = tool == SnowTool::shovel ? 85.0 : 29.0;
    std::vector<Point> dust;
    for (auto& surface : surfaces_) {
        const auto& geometry = surface.geometry;
        if (!(std::abs(p.y - geometry.y) < radius + configuration.maxDepth)) continue;
        std::vector<RemovedSnowSample> removedSamples;
        for (size_t i = 0; i < surface.depths.size(); ++i) {
            if (!(surface.depths[i] > 0.1)) continue;
            const double x = geometry.x + i * SurfaceGeometry::spacing;
            const Point top(x, geometry.y - surface.depths[i] * 0.5);
            const double distance = p.distance(top);
            if (distance < radius) {
                const double oldTop = geometry.y - surface.depths[i];
                const double removed = surface.depths[i] * (1 - distance / radius);
                surface.depths[i] -= removed;
                if (removed > 0.6) removedSamples.push_back({x, oldTop, geometry.y - surface.depths[i]});
                if (dust.size() < 8 && removed > 0.3 && i % 4 == 0) dust.push_back(top);
            }
        }
        chunkSystem_.detach(removedSamples, geometry.id, p, velocity, tool);
    }
    for (const auto& point : dust) burst(point);
    if (tool == SnowTool::mitten) {
        for (auto& particle : particles_) {
            if (!particle.debris && particle.position.distance(p) < 42) {
                const double direction = particle.position.x >= p.x ? 1.0 : -1.0;
                particle.position.x += direction * 4;
            }
        }
    }
}

bool SnowWorld::hitTest(Point p) const {
    if (draggingID_) return true;
    for (const auto& ball : balls_) {
        if (ball.position.distance(p) < ball.radius + 5) return true;
    }
    for (const auto& surface : surfaces_) {
        const int i = surface.geometry.bin(p.x);
        if (i < 0 || !surface.geometry.exposed[i] || !(surface.depths[i] > 1)) continue;
        if (p.y > surface.geometry.y - surface.depths[i] - 8 && p.y < surface.geometry.y + 3) return true;
    }
    return false;
}

bool SnowWorld::beginDrag(Point p) {
    const Snowball* hit = nullptr;
    for (auto it = balls_.rbegin(); it != balls_.rend(); ++it) {
        if (it->position.distance(p) <= it->radius + 5) {
            hit = &*it;
            break;
        }
    }
    if (hit) {
        draggingID_ = hit->id;
        dragOffset_ = Point(hit->position.x - p.x, hit->position.y - p.y);
    } else {
        const SnowSurface* surface = nullptr;
        for (const auto& s : surfaces_) {
            const int i = s.geometry.bin(p.x);
            if (i < 0) continue;
            if (s.geometry.exposed[i] && s.depths[i] > 1 && std::abs(p.y - s.geometry.y) < s.depths[i] + 12) {
                surface = &s;
                break;
            }
        }
        if (!surface) return false;
        if (balls_.size() >= 36) return false;
        const double radius = 7.0;
        Snowball ball;
        ball.id = nextBallID_++;
        ball.position = Point(p.x, surface->geometry.y - radius - 2);
        ball.radius = radius;
        ball.surfaceID = surface->geometry.id;
        balls_.push_back(ball);
        draggingID_ = ball.id;
        dragOffset_ = Point(0, ball.position.y - p.y);
    }
    lastDrag_ = p;
    dragStartY_ = p.y;
    dragLifted_ = false;
    return true;
}

void SnowWorld::drag(Point p) {
    if (!draggingID_) return;
    const int b = ballIndex(*draggingID_);
    if (b < 0) return;
    auto& ball = balls_[b];
    const double distance = std::min(80.0, std::abs(p.x - lastDrag_.x));
    if (std::abs(p.y - dragStartY_) > 14) dragLifted_ = true;
    ball.position = Point(std::min(width - ball.radius, std::max(ball.radius, p.x + dragOffset_.x)), p.y + dragOffset_.y);
    ball.surfaceID.reset();
    ball.supportingBall.reset();
    if (distance > 0.5 && !dragLifted_) {
        for (auto& surface : surfaces_) {
            if (!(std::abs(p.y - surface.geometry.y) < 34)) continue;
            const int i = surface.geometry.bin(p.x);
            if (i < 0 || !surface.geometry.exposed[i]) continue;
            double volume = 0;
            const int last = static_cast<int>(surface.depths.size()) - 1;
            for (int j = std::max(0, i - 3); j <= std::min(last, i + 3); ++j) {
                const double removed = std::min(surface.depths[j], distance * 0.32);
                surface.depths[j] -= removed;
                volume += removed * SurfaceGeometry::spacing;
            }
            ball.radius = std::min(44.0, std::sqrt(ball.radius * ball.radius + volume * 0.35));
            ball.position.y = surface.geometry.y - ball.radius;
            ball.surfaceID = surface.geometry.id;
        }
    }
    ball.velocity = Point();
    lastDrag_ = p;
}

void SnowWorld::endDrag() {
    if (!draggingID_) return;
    const uint64_t id = *draggingID_;
    const int b = ballIndex(id);
    if (b < 0) {
        draggingID_.reset();
        return;
    }
    const Snowball candidate = balls_[b];
    // Небольшой магнетизм делает башни устойчивыми без точного прицеливания.
    const Snowball* support = nullptr;
    double best = std::numeric_limits<double>::infinity();
    for (const auto& other : balls_) {
        if (other.id == id || dependsOn(other, id) || !(other.position.y > candidate.position.y)) continue;
        if (!(std::abs(other.position.x - candidate.position.x) < std::max(20.0, other.radius * 0.95))) continue;
        if (!(std::abs((other.position.y - other.radius) - (candidate.position.y + candidate.radius)) < 34)) continue;
        const double d = other.position.distance(candidate.position);
        if (d < best) {
            best = d;
            support = &other;
        }
    }
    if (support) {
        balls_[b].position =
            Point(support->position.x, support->position.y - support->radius - candidate.radius + 1);
        balls_[b].supportingBall = support->id;
        balls_[b].surfaceID.reset();
    }
    draggingID_.reset();
}

std::vector<Snowball> SnowWorld::snowmanHeads() const {
    std::vector<Snowball> heads;
    for (const auto& head : balls_) {
        if (!head.supportingBall) continue;
        const Snowball* middle = findBall(*head.supportingBall);
        if (!middle || !middle->supportingBall) continue;
        const bool hasTop = std::any_of(balls_.begin(), balls_.end(), [&](const Snowball& b) {
            return b.supportingBall && *b.supportingBall == head.id;
        });
        if (!hasTop) heads.push_back(head);
    }
    return heads;
}

void SnowWorld::spawn(bool scattered) {
    SnowParticle p;
    p.radius = random_.value(0.8, 3.2);
    const double x = random_.value(0, width);
    const double y = scattered ? random_.value(0, height) : -6;
    const double vx = random_.value(-9, 9);
    const double vy = random_.value(24, 62) + p.radius * 9;
    p.position = Point(x, y);
    p.velocity = Point(vx, vy);
    p.opacity = random_.value(0.35, 0.88);
    p.phase = random_.value(0, kPi * 2);
    p.debris = false;
    p.lifetime = 1000;
    particles_.push_back(p);
}

bool SnowWorld::deposit(Point before, const SnowParticle& particle) {
    for (auto& s : surfaces_) {
        const int i = s.geometry.bin(particle.position.x);
        if (i < 0 || !s.geometry.exposed[i]) continue;
        const double top = s.geometry.y - s.depths[i];
        if (!(before.y <= top && particle.position.y >= top)) continue;
        const int last = static_cast<int>(s.depths.size()) - 1;
        for (int j = std::max(0, i - 2); j <= std::min(last, i + 2); ++j) {
            if (!s.geometry.exposed[j]) continue;
            const double weight = std::exp(-static_cast<double>((j - i) * (j - i)) / 2);
            s.depths[j] = std::min(capacity(s.geometry.id, j), s.depths[j] + particle.radius * 0.19 * weight);
        }
        return true;
    }
    return false;
}

void SnowWorld::settlePiles() {
    for (auto& s : surfaces_) {
        if (s.depths.size() <= 2) continue;
        const std::vector<double> old = s.depths;
        for (size_t i = 1; i + 1 < old.size(); ++i) {
            if (!s.geometry.exposed[i]) continue;
            s.depths[i] = std::min(capacity(s.geometry.id, static_cast<int>(i)),
                                   old[i] * 0.92 + (old[i - 1] + old[i + 1]) * 0.04);
        }
    }
}

void SnowWorld::depositChunk(const SnowChunkImpact& impact) {
    auto it = std::find_if(surfaces_.begin(), surfaces_.end(),
                           [&](const SnowSurface& s) { return s.geometry.id == impact.surfaceID; });
    if (it == surfaces_.end()) return;
    auto& s = *it;
    const int center = s.geometry.bin(impact.position.x);
    if (center < 0) return;
    const int radius = std::min(9, std::max(2, static_cast<int>(std::sqrt(impact.area) / SurfaceGeometry::spacing)));
    const int last = static_cast<int>(s.depths.size()) - 1;
    std::vector<int> indices;
    std::vector<double> weights;
    double sum = 0;
    for (int index = std::max(0, center - radius); index <= std::min(last, center + radius); ++index) {
        if (!s.geometry.exposed[index]) continue;
        const double w = std::exp(-std::pow(static_cast<double>(index - center) / radius, 2) * 2);
        indices.push_back(index);
        weights.push_back(w);
        sum += w;
    }
    if (!(sum > 0)) return;
    for (size_t k = 0; k < indices.size(); ++k) {
        const double added = impact.area * weights[k] / sum / SurfaceGeometry::spacing;
        s.depths[indices[k]] = std::min(capacity(impact.surfaceID, indices[k]), s.depths[indices[k]] + added);
    }
}

void SnowWorld::burst(Point p) {
    if (particles_.size() >= 2700) return;
    for (int i = 0; i < 2; ++i) {
        SnowParticle d;
        d.position = p;
        const double vx = random_.value(-85, 85);
        const double vy = random_.value(-95, -25);
        d.velocity = Point(vx, vy);
        d.radius = random_.value(0.7, 1.8);
        d.opacity = 0.8;
        d.phase = 0;
        d.debris = true;
        d.lifetime = random_.value(0.3, 0.8);
        particles_.push_back(d);
    }
}

double SnowWorld::capacity(uint32_t id, int bin) const {
    const double phase = (id % 97) * 0.065;
    const double variation = 0.78 + 0.12 * std::sin(bin * 0.16 + phase) + 0.08 * std::cos(bin * 0.43 + phase * 0.6);
    return configuration.maxDepth * variation;
}

void SnowWorld::updateBalls(double dt) {
    // Снизу вверх: опоры обновляются раньше шаров, которые на них стоят.
    std::vector<size_t> order(balls_.size());
    std::iota(order.begin(), order.end(), size_t(0));
    std::stable_sort(order.begin(), order.end(),
                     [&](size_t a, size_t b) { return balls_[a].position.y > balls_[b].position.y; });
    for (size_t b : order) {
        if (draggingID_ && balls_[b].id == *draggingID_) continue;
        if (balls_[b].supportingBall) {
            const Snowball* support = findBall(*balls_[b].supportingBall);
            if (support && !(draggingID_ && support->id == *draggingID_)) {
                balls_[b].position =
                    Point(support->position.x, support->position.y - support->radius - balls_[b].radius + 1);
                balls_[b].velocity = Point();
                continue;
            }
        }
        balls_[b].supportingBall.reset();
        const auto restingSurface = balls_[b].surfaceID;
        const double previousBottom = balls_[b].position.y + balls_[b].radius;
        balls_[b].velocity.y += 340 * dt;
        balls_[b].position.y += balls_[b].velocity.y * dt;
        balls_[b].surfaceID.reset();
        for (const auto& surface : surfaces_) {
            double top;
            if (!surface.height(balls_[b].position.x, top)) continue;
            const bool wasAbove = previousBottom <= top + 5 || (restingSurface && *restingSurface == surface.geometry.id);
            if (!wasAbove || !(balls_[b].position.y + balls_[b].radius >= top)) continue;
            balls_[b].position.y = top - balls_[b].radius;
            balls_[b].velocity = Point();
            balls_[b].surfaceID = surface.geometry.id;
            break;
        }
        for (size_t o = 0; o < balls_.size(); ++o) {
            const Snowball& other = balls_[o];
            if (other.id == balls_[b].id || (draggingID_ && other.id == *draggingID_) ||
                dependsOn(other, balls_[b].id) || !(other.position.y > balls_[b].position.y))
                continue;
            const double sum = other.radius + balls_[b].radius;
            const double dx = std::abs(other.position.x - balls_[b].position.x);
            if (dx < std::max(8.0, other.radius * 0.7) && balls_[b].position.distance(other.position) < sum) {
                balls_[b].position = Point(other.position.x, other.position.y - sum + 1);
                balls_[b].supportingBall = other.id;
                balls_[b].surfaceID.reset();
                balls_[b].velocity = Point();
                break;
            }
        }
    }
    std::erase_if(balls_, [&](const Snowball& ball) { return ball.position.y > height + 100; });
}

bool SnowWorld::dependsOn(const Snowball& ball, uint64_t ballID) const {
    auto supportID = ball.supportingBall;
    std::unordered_set<uint64_t> visited;
    while (supportID && visited.insert(*supportID).second) {
        if (*supportID == ballID) return true;
        const Snowball* next = findBall(*supportID);
        supportID = next ? next->supportingBall : std::nullopt;
    }
    return false;
}

const Snowball* SnowWorld::findBall(uint64_t id) const {
    for (const auto& b : balls_)
        if (b.id == id) return &b;
    return nullptr;
}

int SnowWorld::ballIndex(uint64_t id) const {
    for (size_t i = 0; i < balls_.size(); ++i)
        if (balls_[i].id == id) return static_cast<int>(i);
    return -1;
}

}  // namespace snow
