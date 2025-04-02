#ifndef CUBE_H
#define CUBE_H

#include <SFML/Graphics.hpp>
#include <vector>
#include "face.h"

class cube {
public:
    sf::Vector2f origin;  // The 2D projection center (e.g. window center).
    std::vector<face> faces;

    float size;  // Cube edge length.
    float rotX;  // Rotation angle about the X axis (in radians).
    float rotY;  // Rotation angle about the Y axis (in radians).

    cube(sf::Vector2f origin, float size);
    void updateFaces();
    void draw(sf::RenderWindow &window);
};

#endif // CUBE_H
