//
//  Cloud.h
//  interface
//
//  Created by Philip Rosedale on 11/17/12.
//  Copyright (c) 2012 High Fidelity, Inc. All rights reserved.
//

#ifndef __interface__Cloud__
#define __interface__Cloud__

#include "Field.h"

#define PARTICLE_WORLD_SIZE 256.0

class Cloud {
public:
    Cloud();
    void simulate(float deltaTime);
    void render();
    
private:
    struct Particle {
        glm::vec3 position, velocity, color;
       }* _particles;
    
    unsigned int _count;
    glm::vec3 _bounds;
    Field* _field;
};

#endif
