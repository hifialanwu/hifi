//
//  CollisionInfo.h
//  hifi
//
//  Created by Andrew Meadows on 2014.01.13
//  Copyright (c) 2014 High Fidelity, Inc. All rights reserved.
//

#ifndef __hifi__CollisionInfo__
#define __hifi__CollisionInfo__

#include <glm/glm.hpp>

class CollisionInfo {
public:
    CollisionInfo() 
        : _damping(0.f),
        _elasticity(1.f),
        _penetration(0.f), 
        _addedVelocity(0.f) { 
        }

    ~CollisionInfo() {}

    //glm::vec3 _point;
    //glm::vec3 _normal;
    float _damping;
    float _elasticity;
    glm::vec3 _penetration; // depth that bodyA is penetrates bodyB
    glm::vec3 _addedVelocity;
};


#endif /* defined(__hifi__CollisionInfo__) */
