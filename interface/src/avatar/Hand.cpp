//
//  Hand.cpp
//  interface
//
//  Copyright (c) 2013 High Fidelity, Inc. All rights reserved.

#include <QImage>

#include <NodeList.h>

#include <GeometryUtil.h>

#include "Application.h"
#include "Avatar.h"
#include "Hand.h"
#include "Menu.h"
#include "Util.h"
#include "renderer/ProgramObject.h"

using namespace std;

const float FINGERTIP_COLLISION_RADIUS = 0.01f;
const float FINGERTIP_VOXEL_SIZE = 0.05f;
const float PALM_COLLISION_RADIUS = 0.03f;


Hand::Hand(Avatar* owningAvatar) :
    HandData((AvatarData*)owningAvatar),

    _owningAvatar(owningAvatar),
    _renderAlpha(1.0),
    _ballColor(0.0, 0.0, 0.4),
    _collisionCenter(0,0,0),
    _collisionAge(0),
    _collisionDuration(0),
    _pitchUpdate(0),
    _grabDelta(0, 0, 0),
    _grabDeltaVelocity(0, 0, 0),
    _grabStartRotation(0, 0, 0, 1),
    _grabCurrentRotation(0, 0, 0, 1)
{
}

void Hand::init() {
    // Different colors for my hand and others' hands
    if (_owningAvatar && _owningAvatar->isMyAvatar()) {
        _ballColor = glm::vec3(0.0, 0.4, 0.0);
    }
    else {
        _ballColor = glm::vec3(0.0, 0.0, 0.4);
    }
}

void Hand::reset() {
}

glm::vec3 Hand::getAndResetGrabDelta() {
    const float HAND_GRAB_SCALE_DISTANCE = 2.f;
    glm::vec3 delta = _grabDelta * _owningAvatar->getScale() * HAND_GRAB_SCALE_DISTANCE;
    _grabDelta = glm::vec3(0,0,0);
    glm::quat avatarRotation = _owningAvatar->getOrientation();
    return avatarRotation * -delta;
}

glm::vec3 Hand::getAndResetGrabDeltaVelocity() {
    const float HAND_GRAB_SCALE_VELOCITY = 5.f;
    glm::vec3 delta = _grabDeltaVelocity * _owningAvatar->getScale() * HAND_GRAB_SCALE_VELOCITY;
    _grabDeltaVelocity = glm::vec3(0,0,0);
    glm::quat avatarRotation = _owningAvatar->getOrientation();
    return avatarRotation * -delta;
    
}
glm::quat Hand::getAndResetGrabRotation() {
    glm::quat diff = _grabCurrentRotation * glm::inverse(_grabStartRotation);
    _grabStartRotation = _grabCurrentRotation;
    return diff;
}

void Hand::simulate(float deltaTime, bool isMine) {
    
    if (_collisionAge > 0.f) {
        _collisionAge += deltaTime;
    }
    
    if (isMine) {
        _buckyBalls.simulate(deltaTime);
        updateCollisions();
    }
    
    calculateGeometry();
    
    if (isMine) {
        
        //  Iterate hand controllers, take actions as needed
        
        for (size_t i = 0; i < getNumPalms(); ++i) {
            PalmData& palm = getPalms()[i];
            if (palm.isActive()) {
                FingerData& finger = palm.getFingers()[0];   //  Sixense has only one finger
                glm::vec3 fingerTipPosition = finger.getTipPosition();
                
                _buckyBalls.grab(palm, fingerTipPosition, _owningAvatar->getOrientation(), deltaTime);
                
                if (palm.getControllerButtons() & BUTTON_4) {
                    _grabDelta +=  palm.getRawVelocity() * deltaTime;
                    _grabCurrentRotation = palm.getRawRotation();
                }
                if ((palm.getLastControllerButtons() & BUTTON_4) && !(palm.getControllerButtons() & BUTTON_4)) {
                    // Just ending grab, capture velocity
                    _grabDeltaVelocity = palm.getRawVelocity();
                }
                if (!(palm.getLastControllerButtons() & BUTTON_4) && (palm.getControllerButtons() & BUTTON_4)) {
                    // Just starting grab, capture starting rotation
                    _grabStartRotation = palm.getRawRotation();
                }
                
                if (palm.getControllerButtons() & BUTTON_1) {
                    if (glm::length(fingerTipPosition - _lastFingerAddVoxel) > (FINGERTIP_VOXEL_SIZE / 2.f)) {
                        QColor paintColor = Application::getInstance()->getMenu()->getActionForOption(MenuOption::VoxelPaintColor)->data().value<QColor>();
                        Application::getInstance()->makeVoxel(fingerTipPosition,
                                                              FINGERTIP_VOXEL_SIZE,
                                                              paintColor.red(),
                                                              paintColor.green(),
                                                              paintColor.blue(),
                                                              true);
                        _lastFingerAddVoxel = fingerTipPosition;
                    }
                } else if (palm.getControllerButtons() & BUTTON_2) {
                    if (glm::length(fingerTipPosition - _lastFingerDeleteVoxel) > (FINGERTIP_VOXEL_SIZE / 2.f)) {
                        Application::getInstance()->removeVoxel(fingerTipPosition, FINGERTIP_VOXEL_SIZE);
                        _lastFingerDeleteVoxel = fingerTipPosition;
                    }
                }
                
                //  Voxel Drumming with fingertips if enabled
                if (Application::getInstance()->getMenu()->isOptionChecked(MenuOption::VoxelDrumming)) {
                    VoxelTreeElement* fingerNode = Application::getInstance()->getVoxels()->getVoxelEnclosing(
                                                                                glm::vec3(fingerTipPosition / (float)TREE_SCALE));
                    if (fingerNode) {
                        if (!palm.getIsCollidingWithVoxel()) {
                            //  Collision has just started
                            palm.setIsCollidingWithVoxel(true);
                            handleVoxelCollision(&palm, fingerTipPosition, fingerNode, deltaTime);
                            //  Set highlight voxel
                            VoxelDetail voxel;
                            glm::vec3 pos = fingerNode->getCorner();
                            voxel.x = pos.x;
                            voxel.y = pos.y;
                            voxel.z = pos.z;
                            voxel.s = fingerNode->getScale();
                            voxel.red = fingerNode->getColor()[0];
                            voxel.green = fingerNode->getColor()[1];
                            voxel.blue = fingerNode->getColor()[2];
                            Application::getInstance()->setHighlightVoxel(voxel);
                            Application::getInstance()->setIsHighlightVoxel(true);
                        }
                    } else {
                        if (palm.getIsCollidingWithVoxel()) {
                            //  Collision has just ended
                            palm.setIsCollidingWithVoxel(false);
                            Application::getInstance()->setIsHighlightVoxel(false);
                        }
                    }
                }
            }
            palm.setLastControllerButtons(palm.getControllerButtons());
        }
    }
}

void Hand::updateCollisions() {
    // use position to obtain the left and right palm indices
    int leftPalmIndex, rightPalmIndex;   
    getLeftRightPalmIndices(leftPalmIndex, rightPalmIndex);
    
    // check for collisions
    for (size_t i = 0; i < getNumPalms(); i++) {
        PalmData& palm = getPalms()[i];
        if (!palm.isActive()) {
            continue;
        }        
        float scaledPalmRadius = PALM_COLLISION_RADIUS * _owningAvatar->getScale();
        glm::vec3 totalPenetration;
        
        // check other avatars
        foreach (const AvatarSharedPointer& avatarPointer, Application::getInstance()->getAvatarManager().getAvatarHash()) {
            Avatar* avatar = static_cast<Avatar*>(avatarPointer.data());
            if (avatar == _owningAvatar) {
                // don't collid with our own hands
                continue;
            }
            if (Application::getInstance()->getMenu()->isOptionChecked(MenuOption::PlaySlaps)) {
                //  Check for palm collisions
                glm::vec3 myPalmPosition = palm.getPosition();
                float palmCollisionDistance = 0.1f;
                bool wasColliding = palm.getIsCollidingWithPalm();
                palm.setIsCollidingWithPalm(false);
                //  If 'Play Slaps' is enabled, look for palm-to-palm collisions and make sound
                for (size_t j = 0; j < avatar->getHand().getNumPalms(); j++) {
                    PalmData& otherPalm = avatar->getHand().getPalms()[j];
                    if (!otherPalm.isActive()) {
                        continue;
                    }
                    glm::vec3 otherPalmPosition = otherPalm.getPosition();
                    if (glm::length(otherPalmPosition - myPalmPosition) < palmCollisionDistance) {
                        palm.setIsCollidingWithPalm(true);
                        if (!wasColliding) {
                            const float PALM_COLLIDE_VOLUME = 1.f;
                            const float PALM_COLLIDE_FREQUENCY = 1000.f;
                            const float PALM_COLLIDE_DURATION_MAX = 0.75f;
                            const float PALM_COLLIDE_DECAY_PER_SAMPLE = 0.01f;
                            Application::getInstance()->getAudio()->startDrumSound(PALM_COLLIDE_VOLUME,
                                                                                PALM_COLLIDE_FREQUENCY,
                                                                                PALM_COLLIDE_DURATION_MAX,
                                                                                PALM_COLLIDE_DECAY_PER_SAMPLE);
                            //  If the other person's palm is in motion, move mine downward to show I was hit
                            const float MIN_VELOCITY_FOR_SLAP = 0.05f;
                            if (glm::length(otherPalm.getVelocity()) > MIN_VELOCITY_FOR_SLAP) {
                                // add slapback here
                            }
                        }
                        
                        
                    }
                }
            }
            glm::vec3 avatarPenetration;
            if (avatar->findSpherePenetration(palm.getPosition(), scaledPalmRadius, avatarPenetration)) {
                totalPenetration = addPenetrations(totalPenetration, avatarPenetration);
                //  Check for collisions with the other avatar's leap palms
            }
        }
            
        if (Application::getInstance()->getMenu()->isOptionChecked(MenuOption::HandsCollideWithSelf)) {
            // and the current avatar (ignoring everything below the parent of the parent of the last free joint)
            glm::vec3 owningPenetration;
            const Model& skeletonModel = _owningAvatar->getSkeletonModel();
            int skipIndex = skeletonModel.getParentJointIndex(skeletonModel.getParentJointIndex(
                skeletonModel.getLastFreeJointIndex((i == leftPalmIndex) ? skeletonModel.getLeftHandJointIndex() :
                    (i == rightPalmIndex) ? skeletonModel.getRightHandJointIndex() : -1)));
            if (_owningAvatar->findSpherePenetration(palm.getPosition(), scaledPalmRadius, owningPenetration, skipIndex)) {
                totalPenetration = addPenetrations(totalPenetration, owningPenetration);
            }
        }
        
        // un-penetrate
        palm.addToPosition(-totalPenetration);
    }
}

void Hand::handleVoxelCollision(PalmData* palm, const glm::vec3& fingerTipPosition, VoxelTreeElement* voxel, float deltaTime) {
    //  Collision between finger and a voxel plays sound
    const float LOWEST_FREQUENCY = 100.f;
    const float HERTZ_PER_RGB = 3.f;
    const float DECAY_PER_SAMPLE = 0.0005f;
    const float DURATION_MAX = 2.0f;
    const float MIN_VOLUME = 0.1f;
    float volume = MIN_VOLUME + glm::clamp(glm::length(palm->getRawVelocity()), 0.f, (1.f - MIN_VOLUME));
    float duration = volume;
    _collisionCenter = fingerTipPosition;
    _collisionAge = deltaTime;
    _collisionDuration = duration;
    int voxelBrightness = voxel->getColor()[0] + voxel->getColor()[1] + voxel->getColor()[2];
    float frequency = LOWEST_FREQUENCY + (voxelBrightness * HERTZ_PER_RGB);
    Application::getInstance()->getAudio()->startDrumSound(volume,
                                                           frequency,
                                                           DURATION_MAX,
                                                           DECAY_PER_SAMPLE);
}

void Hand::calculateGeometry() {
    // generate finger tip balls....
    _leapFingerTipBalls.clear();
    for (size_t i = 0; i < getNumPalms(); ++i) {
        PalmData& palm = getPalms()[i];
        if (palm.isActive()) {
            for (size_t f = 0; f < palm.getNumFingers(); ++f) {
                FingerData& finger = palm.getFingers()[f];
                if (finger.isActive()) {
                    const float standardBallRadius = FINGERTIP_COLLISION_RADIUS;
                    _leapFingerTipBalls.resize(_leapFingerTipBalls.size() + 1);
                    HandBall& ball = _leapFingerTipBalls.back();
                    ball.rotation = getBaseOrientation();
                    ball.position = finger.getTipPosition();
                    ball.radius         = standardBallRadius;
                    ball.touchForce     = 0.0;
                    ball.isCollidable   = true;
                    ball.isColliding    = false;
                }
            }
        }
    }

    // generate finger root balls....
    _leapFingerRootBalls.clear();
    for (size_t i = 0; i < getNumPalms(); ++i) {
        PalmData& palm = getPalms()[i];
        if (palm.isActive()) {
            for (size_t f = 0; f < palm.getNumFingers(); ++f) {
                FingerData& finger = palm.getFingers()[f];
                if (finger.isActive()) {
                    const float standardBallRadius = 0.005f;
                    _leapFingerRootBalls.resize(_leapFingerRootBalls.size() + 1);
                    HandBall& ball = _leapFingerRootBalls.back();
                    ball.rotation = getBaseOrientation();
                    ball.position = finger.getRootPosition();
                    ball.radius         = standardBallRadius;
                    ball.touchForce     = 0.0;
                    ball.isCollidable   = true;
                    ball.isColliding    = false;
                }
            }
        }
    }
}

void Hand::render(bool isMine) {
    
    _renderAlpha = 1.0;
    
    if (isMine) {
        _buckyBalls.render();
    }
    
    const Menu* menu = Application::getInstance()->getMenu();
    if(menu){
      if (menu->isOptionChecked(MenuOption::CollisionProxies)) {
        for (size_t i = 0; i < getNumPalms(); i++) {
	  PalmData& palm = getPalms()[i];
	  if (!palm.isActive()) {
	    continue;
	  }
	  glm::vec3 position = palm.getPosition();
	  glPushMatrix();
	  glTranslatef(position.x, position.y, position.z);
	  glColor3f(0.0f, 1.0f, 0.0f);
	  glutSolidSphere(PALM_COLLISION_RADIUS * _owningAvatar->getScale(), 10, 10);
	  glPopMatrix();
        }
      }

    
      if (menu->isOptionChecked(MenuOption::DisplayLeapHands)) {
        renderLeapHands(isMine);
      }
    }

    if (isMine) {
        //  If hand/voxel collision has happened, render a little expanding sphere
        if (_collisionAge > 0.f) {
            float opacity = glm::clamp(1.f - (_collisionAge / _collisionDuration), 0.f, 1.f);
            glColor4f(1, 0, 0, 0.5 * opacity);
            glPushMatrix();
            glTranslatef(_collisionCenter.x, _collisionCenter.y, _collisionCenter.z);
            glutSolidSphere(_collisionAge * 0.25f, 20, 20);
            glPopMatrix();
            if (_collisionAge > _collisionDuration) {
                _collisionAge = 0.f;
            }
        }
    }

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_RESCALE_NORMAL);
    
}

void Hand::renderLeapHands(bool isMine) {

    const float alpha = 1.0f;
    
    //const glm::vec3 handColor = _ballColor;
    const glm::vec3 handColor(1.0, 0.84, 0.66); // use the skin color
    
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);

    if (isMine && Application::getInstance()->getMenu()->isOptionChecked(MenuOption::DisplayHandTargets)) {
        for (size_t i = 0; i < getNumPalms(); ++i) {
            PalmData& palm = getPalms()[i];
            if (!palm.isActive()) {
                continue;
            }
            glm::vec3 targetPosition;
            palm.getBallHoldPosition(targetPosition);
            glPushMatrix();
        
            const float collisionRadius = 0.05f;
            glColor4f(0.5f,0.5f,0.5f, alpha);
            glutWireSphere(collisionRadius, 10.f, 10.f);
            glPopMatrix();
        }
    }
    
    glPushMatrix();
    // Draw the leap balls
    for (size_t i = 0; i < _leapFingerTipBalls.size(); i++) {
        if (alpha > 0.0f) {
            if (_leapFingerTipBalls[i].isColliding) {
                glColor4f(handColor.r, 0, 0, alpha);
            } else {
                glColor4f(handColor.r, handColor.g, handColor.b, alpha);
            }
            glPushMatrix();
            glTranslatef(_leapFingerTipBalls[i].position.x, _leapFingerTipBalls[i].position.y, _leapFingerTipBalls[i].position.z);
            glutSolidSphere(_leapFingerTipBalls[i].radius, 20.0f, 20.0f);
            glPopMatrix();
        }
    }
        
    // Draw the finger root cones
    for (size_t i = 0; i < getNumPalms(); ++i) {
        PalmData& palm = getPalms()[i];
        if (palm.isActive()) {
            for (size_t f = 0; f < palm.getNumFingers(); ++f) {
                FingerData& finger = palm.getFingers()[f];
                if (finger.isActive()) {
                    glColor4f(handColor.r, handColor.g, handColor.b, 0.5);
                    glm::vec3 tip = finger.getTipPosition();
                    glm::vec3 root = finger.getRootPosition();
                    Avatar::renderJointConnectingCone(root, tip, 0.001f, 0.003f);
                }
            }
        }
    }

    // Draw the hand paddles
    int MAX_NUM_PADDLES = 2; // one for left and one for right
    glColor4f(handColor.r, handColor.g, handColor.b, 0.3f);
    for (int i = 0; i < MAX_NUM_PADDLES; i++) {
        const PalmData* palm = getPalm(i);
        if (palm) {
            // compute finger axis
            glm::vec3 fingerAxis(0.f);
            for (size_t f = 0; f < palm->getNumFingers(); ++f) {
                const FingerData& finger = (palm->getFingers())[f];
                if (finger.isActive()) {
                    glm::vec3 fingerTip = finger.getTipPosition();
                    glm::vec3 fingerRoot = finger.getRootPosition();
                    fingerAxis = glm::normalize(fingerTip - fingerRoot);
                    break;
                }
            }
            // compute paddle position
            glm::vec3 handPosition;
            if (i == SIXENSE_CONTROLLER_ID_LEFT_HAND) {
                _owningAvatar->getSkeletonModel().getLeftHandPosition(handPosition);
            } else if (i == SIXENSE_CONTROLLER_ID_RIGHT_HAND) {
                _owningAvatar->getSkeletonModel().getRightHandPosition(handPosition);
            }
            glm::vec3 tip = handPosition + HAND_PADDLE_OFFSET * fingerAxis;
            glm::vec3 root = tip + palm->getNormal() * HAND_PADDLE_THICKNESS;
            // render a very shallow cone as the paddle
            Avatar::renderJointConnectingCone(root, tip, HAND_PADDLE_RADIUS, 0.f);
        }
    }

    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);

    glPopMatrix();
}


void Hand::setLeapHands(const std::vector<glm::vec3>& handPositions,
                          const std::vector<glm::vec3>& handNormals) {
    for (size_t i = 0; i < getNumPalms(); ++i) {
        PalmData& palm = getPalms()[i];
        if (i < handPositions.size()) {
            palm.setActive(true);
            palm.setRawPosition(handPositions[i]);
            palm.setRawNormal(handNormals[i]);
        }
        else {
            palm.setActive(false);
        }
    }
}






