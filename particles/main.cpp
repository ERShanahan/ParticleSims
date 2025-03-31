#define SFML_STATIC
#include <SFML/Graphics.hpp>
#include <cstdlib>
#include <cmath>
#include <vector>
#include <thread>
#include <mutex>

#include "particle.h"
#include "matrix.h"
#include "cblas.h"

#define WINDOW_X 1200
#define WINDOW_Y 800

#define GRAVITY 1000.0f

#define NUM_PARTICLES 100000
#define RADIUS 0.5f
#define DIMENSION 2
#define CELL_SIZE 10
#define X 0
#define Y 1

struct CellKey {
    int x, y;
    bool operator==(const CellKey &other) const { return x == other.x && y == other.y; }
};

namespace std {
    template <>
    struct hash<CellKey> {
        std::size_t operator()(const CellKey &k) const {
            return (std::hash<int>()(k.x) ^ (std::hash<int>()(k.y) << 1));
        }
    };
}

CellKey computeCellKey(float x, float y) {
    return CellKey{static_cast<int>(x) / CELL_SIZE, static_cast<int>(y) / CELL_SIZE};
}


int main() {
    sf::RenderWindow window(sf::VideoMode({WINDOW_X, WINDOW_Y}), "Particle Simulation");

    // Create matrices for positions, velocities, and accelerations.
    matrix positions(NUM_PARTICLES, DIMENSION);
    matrix velocities(NUM_PARTICLES, DIMENSION);
    matrix accelerations(NUM_PARTICLES, DIMENSION);

    //Mapping of Keys to indices
    std::unordered_map<CellKey, std::vector<int>> grid;

    //Vector of Mutexes for particles (safety for velocity updates)
    std::vector<std::mutex> particleMutexes(NUM_PARTICLES);
    unsigned int numThreads = std::thread::hardware_concurrency();
    if (numThreads == 0) {
        numThreads = 4; // fallback if hardware_concurrency() returns 0
    }
    std::vector<std::thread> threads;

    // Pre-allocate an array of Particle objects.
    Particle** particles = new Particle*[NUM_PARTICLES];

    const float gravityY = GRAVITY;
    float radius = RADIUS;
    sf::Color color = sf::Color::White;

    // Initialize particles and matrix data.
    for (int i = 0; i < NUM_PARTICLES; ++i) {
        // Random position within window bounds.
        float x = static_cast<float>(std::rand() % WINDOW_X);
        float y = static_cast<float>(std::rand() % WINDOW_Y);
        positions(i, X) = x;
        positions(i, Y) = y;

        // Random velocity components.
        float vx = static_cast<float>((std::rand() % 2) - 1);
        float vy = static_cast<float>((std::rand() % 2) - 1);
        velocities(i, X) = vx;
        velocities(i, Y) = vy;

        // Constant acceleration (gravity).
        accelerations(i, X) = 0.0;
        accelerations(i, Y) = gravityY;

        // Construct the Particle object using pointers to the matrix row.
        // We assume that the matrix stores data row-major, so &positions.data[i * DIMENSION] points to this particle's state.
        double* pos_ptr = &positions.data[i * DIMENSION];
        double* vel_ptr = &velocities.data[i * DIMENSION];
        double* acc_ptr = &accelerations.data[i * DIMENSION];

        particles[i] = new Particle(pos_ptr, vel_ptr, acc_ptr, radius, color);
    }

    sf::Clock clock;
    while (window.isOpen()) {
        while (std::optional event = window.pollEvent()) {
            if (event->is<sf::Event::Closed>())
                window.close();
        }

        float dt = clock.restart().asSeconds();

        // Update positions: positions = positions + velocities * dt
        cblas_daxpy(positions.data.size(), dt, velocities.data.data(), 1, positions.data.data(), 1);
        // Update velocities: velocities = velocities + accelerations * dt
        cblas_daxpy(positions.data.size(), dt, accelerations.data.data(), 1, velocities.data.data(), 1);

        // Sync updated data to each Particle's shape and handle boundary collisions.
        for (int i = 0; i < NUM_PARTICLES; ++i) {
            // Since particles reference matrix data, their pos/vel are already updated.
            particles[i]->handleBoundaryCollision(window.getSize());
            particles[i]->syncShape();
        }

        grid.clear();
        for (int i = 0; i < NUM_PARTICLES; ++i) {
            float x = static_cast<float>(particles[i]->pos[X]);
            float y = static_cast<float>(particles[i]->pos[Y]);
            CellKey key = computeCellKey(x, y);
            grid[key].push_back(i);
        }

        std::vector<CellKey> CellKeys;
        for (const auto &cellPair : grid) {
            CellKeys.push_back(cellPair.first);
        }
        int totalCells = CellKeys.size();
        int cellsPerThread = totalCells / numThreads;
        int extraCells = totalCells % numThreads;
        int currentIndex = 0;

        auto processCells = [&](int start, int end) {
            for (int idx = start; idx < end; ++idx) {
                CellKey key = CellKeys[idx];
                const auto &cellParticles = grid[key];
        
                // Process collisions within the same cell.
                for (size_t a = 0; a < cellParticles.size(); ++a) {
                    int i = cellParticles[a];
                    for (size_t b = a + 1; b < cellParticles.size(); ++b) {
                        int j = cellParticles[b];
        
                        float x1 = static_cast<float>(particles[i]->pos[X]);
                        float y1 = static_cast<float>(particles[i]->pos[Y]);
                        float x2 = static_cast<float>(particles[j]->pos[X]);
                        float y2 = static_cast<float>(particles[j]->pos[Y]);
                        float dx = x2 - x1;
                        float dy = y2 - y1;
                        float dist2 = dx * dx + dy * dy;
                        float radiusSum = particles[i]->radius + particles[j]->radius;
                        
                        if (dist2 < radiusSum * radiusSum) {
                            float distance = std::sqrt(dist2);
                            if (distance == 0.f) {
                                distance = 0.1f;
                                dx = radiusSum;
                                dy = 0.f;
                            }
                            float nx = dx / distance;
                            float ny = dy / distance;
                            
                            float v1x = static_cast<float>(particles[i]->vel[X]);
                            float v1y = static_cast<float>(particles[i]->vel[Y]);
                            float v2x = static_cast<float>(particles[j]->vel[X]);
                            float v2y = static_cast<float>(particles[j]->vel[Y]);
                            
                            float relVel = (v1x - v2x) * nx + (v1y - v2y) * ny;
                            float impulse = relVel;
                            
                            // Lock both particles to update velocities safely.
                            std::scoped_lock lock(particleMutexes[i], particleMutexes[j]);
                            particles[i]->vel[X] = v1x - impulse * nx * (1 - ENTROPY);
                            particles[i]->vel[Y] = v1y - impulse * ny * (1 - ENTROPY);
                            particles[j]->vel[X] = v2x + impulse * nx * (1 - ENTROPY);
                            particles[j]->vel[Y] = v2y + impulse * ny * (1 - ENTROPY);
                        }
                    }
                }
        
                // Process collisions with neighboring cells.
                for (int dx = -1; dx <= 1; ++dx) {
                    for (int dy = -1; dy <= 1; ++dy) {
                        if (dx == 0 && dy == 0) continue;
                        CellKey neighborKey{ key.x + dx, key.y + dy };
                        if (grid.count(neighborKey)) {
                            const auto &neighborParticles = grid[neighborKey];
                            for (int i : cellParticles) {
                                for (int j : neighborParticles) {
                                    // To avoid duplicate processing, you might restrict to i < j.
                                    if (i < j) {
                                        float x1 = static_cast<float>(particles[i]->pos[X]);
                                        float y1 = static_cast<float>(particles[i]->pos[Y]);
                                        float x2 = static_cast<float>(particles[j]->pos[X]);
                                        float y2 = static_cast<float>(particles[j]->pos[Y]);
                                        float dx = x2 - x1;
                                        float dy = y2 - y1;
                                        float dist2 = dx * dx + dy * dy;
                                        float radiusSum = particles[i]->radius + particles[j]->radius;
                                        
                                        if (dist2 < radiusSum * radiusSum) {
                                            float distance = std::sqrt(dist2);
                                            if (distance == 0.f) {
                                                distance = 0.1f;
                                                dx = radiusSum;
                                                dy = 0.f;
                                            }
                                            float nx = dx / distance;
                                            float ny = dy / distance;
                                            
                                            float v1x = static_cast<float>(particles[i]->vel[X]);
                                            float v1y = static_cast<float>(particles[i]->vel[Y]);
                                            float v2x = static_cast<float>(particles[j]->vel[X]);
                                            float v2y = static_cast<float>(particles[j]->vel[Y]);
                                            
                                            float relVel = (v1x - v2x) * nx + (v1y - v2y) * ny;
                                            float impulse = relVel;
                                            
                                            std::scoped_lock lock(particleMutexes[i], particleMutexes[j]);
                                            particles[i]->vel[X] = v1x - impulse * nx * (1 - ENTROPY);
                                            particles[i]->vel[Y] = v1y - impulse * ny * (1 - ENTROPY);
                                            particles[j]->vel[X] = v2x + impulse * nx * (1 - ENTROPY);
                                            particles[j]->vel[Y] = v2y + impulse * ny * (1 - ENTROPY);
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        };

        for (unsigned int t = 0; t < numThreads; t++) {
            int start = currentIndex;
            int count = cellsPerThread + (t < extraCells ? 1 : 0);
            int end = start + count;
            threads.emplace_back(processCells, start, end);
            currentIndex = end;
        }

        for (auto &th : threads) {
            th.join();
        }
        threads.clear();

        // Drawing.
        window.clear();
        for (int i = 0; i < NUM_PARTICLES; ++i) {
            particles[i]->draw(window);
        }
        window.display();
    }

    for (int i = 0; i < NUM_PARTICLES; ++i) {
        delete particles[i];
    }
    delete[] particles;

    return 0;
}