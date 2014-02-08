//
//  MyAvatar.cpp
//  interface
//
//  Created by Mark Peng on 8/16/13.
//  Copyright (c) 2012 High Fidelity, Inc. All rights reserved.
//

#include <algorithm>
#include <vector>

#include <glm/gtx/vector_angle.hpp>

#include <NodeList.h>
#include <PacketHeaders.h>
#include <SharedUtil.h>

#include "Application.h"
#include "Audio.h"
#include "DataServerClient.h"
#include "Environment.h"
#include "Menu.h"
#include "MyAvatar.h"
#include "Physics.h"
#include "VoxelSystem.h"
#include "devices/Faceshift.h"
#include "devices/OculusManager.h"
#include "ui/TextRenderer.h"

using namespace std;

const glm::vec3 DEFAULT_UP_DIRECTION(0.0f, 1.0f, 0.0f);
const float YAW_MAG = 500.0f;
const float PITCH_MAG = 100.0f;
const float COLLISION_RADIUS_SCALAR = 1.2f; // pertains to avatar-to-avatar collisions
const float COLLISION_BODY_FORCE = 30.0f; // pertains to avatar-to-avatar collisions
const float COLLISION_RADIUS_SCALE = 0.125f;
const float MOUSE_RAY_TOUCH_RANGE = 0.01f;
const bool USING_HEAD_LEAN = false;
const float SKIN_COLOR[] = {1.0f, 0.84f, 0.66f};
const float DARK_SKIN_COLOR[] = {0.9f, 0.78f, 0.63f};

MyAvatar::MyAvatar() :
	Avatar(),
    _mousePressed(false),
    _bodyPitchDelta(0.0f),
    _bodyRollDelta(0.0f),
    _shouldJump(false),
    _gravity(0.0f, -1.0f, 0.0f),
    _distanceToNearestAvatar(std::numeric_limits<float>::max()),
    _elapsedTimeMoving(0.0f),
	_elapsedTimeStopped(0.0f),
    _elapsedTimeSinceCollision(0.0f),
    _lastCollisionPosition(0, 0, 0),
    _speedBrakes(false),
    _isCollisionsOn(true),
    _isThrustOn(false),
    _thrustMultiplier(1.0f),
    _moveTarget(0,0,0),
    _moveTargetStepCounter(0),
    _lookAtTargetAvatar()
{
    for (int i = 0; i < MAX_DRIVE_KEYS; i++) {
        _driveKeys[i] = 0.0f;
    }
}

MyAvatar::~MyAvatar() {
    _lookAtTargetAvatar.clear();
}

void MyAvatar::reset() {
    // TODO? resurrect headMouse stuff?
    //_headMouseX = _glWidget->width() / 2;
    //_headMouseY = _glWidget->height() / 2;
    _head.reset();
    _hand.reset();

    setVelocity(glm::vec3(0,0,0));
    setThrust(glm::vec3(0,0,0));
    _transmitter.resetLevels();
}

void MyAvatar::setMoveTarget(const glm::vec3 moveTarget) {
    _moveTarget = moveTarget;
    _moveTargetStepCounter = 0;
}

void MyAvatar::updateTransmitter(float deltaTime) {
    // no transmitter drive implies transmitter pick
  const Menu* menu = Application::getInstance()->getMenu();
  if(menu){
    if (!menu->isOptionChecked(MenuOption::TransmitterDrive) && _transmitter.isConnected()) {
      _transmitterPickStart = getChestPosition();
      glm::vec3 direction = getOrientation() * glm::quat(glm::radians(_transmitter.getEstimatedRotation())) * IDENTITY_FRONT;

      // check against voxels, avatars
      const float MAX_PICK_DISTANCE = 100.0f;
      float minDistance = MAX_PICK_DISTANCE;
      VoxelDetail detail;
      float distance;
      BoxFace face;
      VoxelSystem* voxels = Application::getInstance()->getVoxels();
      if (voxels->findRayIntersection(_transmitterPickStart, direction, detail, distance, face)) {
	minDistance = min(minDistance, distance);
      }
      _transmitterPickEnd = _transmitterPickStart + direction * minDistance;

    } else {
      _transmitterPickStart = _transmitterPickEnd = glm::vec3();
    }
  }
}

void MyAvatar::update(float deltaTime) {
    updateTransmitter(deltaTime);

    // TODO: resurrect touch interactions between avatars
    //// rotate body yaw for yaw received from multitouch
    //setOrientation(getOrientation() * glm::quat(glm::vec3(0, _yawFromTouch, 0)));
    //_yawFromTouch = 0.f;
    //
    //// apply pitch from touch
    //_head.setPitch(_head.getPitch() + _pitchFromTouch);
    //_pitchFromTouch = 0.0f;
    //
    //float TOUCH_YAW_SCALE = -0.25f;
    //float TOUCH_PITCH_SCALE = -12.5f;
    //float FIXED_TOUCH_TIMESTEP = 0.016f;
    //_yawFromTouch += ((_touchAvgX - _lastTouchAvgX) * TOUCH_YAW_SCALE * FIXED_TOUCH_TIMESTEP);
    //_pitchFromTouch += ((_touchAvgY - _lastTouchAvgY) * TOUCH_PITCH_SCALE * FIXED_TOUCH_TIMESTEP);

    // Update my avatar's state from gyros
    const Menu* menu = Application::getInstance()->getMenu();
    if(menu){
      updateFromGyros(menu->isOptionChecked(MenuOption::TurnWithHead));
    }

    // Update head mouse from faceshift if active
    Faceshift* faceshift = Application::getInstance()->getFaceshift();
    if (faceshift->isActive()) {
        glm::vec3 headVelocity = faceshift->getHeadAngularVelocity();

        // TODO? resurrect headMouse stuff?
        //// sets how quickly head angular rotation moves the head mouse
        //const float HEADMOUSE_FACESHIFT_YAW_SCALE = 40.f;
        //const float HEADMOUSE_FACESHIFT_PITCH_SCALE = 30.f;
        //_headMouseX -= headVelocity.y * HEADMOUSE_FACESHIFT_YAW_SCALE;
        //_headMouseY -= headVelocity.x * HEADMOUSE_FACESHIFT_PITCH_SCALE;
        //
        ////  Constrain head-driven mouse to edges of screen
        //_headMouseX = glm::clamp(_headMouseX, 0, _glWidget->width());
        //_headMouseY = glm::clamp(_headMouseY, 0, _glWidget->height());
    }

    if (OculusManager::isConnected()) {
        float yaw, pitch, roll;
        OculusManager::getEulerAngles(yaw, pitch, roll);

        _head.setYaw(yaw);
        _head.setPitch(pitch);
        _head.setRoll(roll);
    }

    //  Get audio loudness data from audio input device
    _head.setAudioLoudness(Application::getInstance()->getAudio()->getLastInputLoudness());
    
    if(menu){
      if (Application::getInstance()->getMenu()->isOptionChecked(MenuOption::Gravity)) {
        setGravity(Application::getInstance()->getEnvironment()->getGravity(getPosition()));
      } else {
        setGravity(glm::vec3(0.0f, 0.0f, 0.0f));
      }
    }

    simulate(deltaTime);
}

void MyAvatar::simulate(float deltaTime) {

    glm::quat orientation = getOrientation();

    // Update movement timers
    _elapsedTimeSinceCollision += deltaTime;
    const float VELOCITY_MOVEMENT_TIMER_THRESHOLD = 0.2f;
    if (glm::length(_velocity) < VELOCITY_MOVEMENT_TIMER_THRESHOLD) {
        _elapsedTimeMoving = 0.f;
        _elapsedTimeStopped += deltaTime;
    } else {
        _elapsedTimeStopped = 0.f;
        _elapsedTimeMoving += deltaTime;
    }

    if (_scale != _targetScale) {
        float scale = (1.f - SMOOTHING_RATIO) * _scale + SMOOTHING_RATIO * _targetScale;
        setScale(scale);
        Application::getInstance()->getCamera()->setScale(scale);
    }

    //  Collect thrust forces from keyboard and devices
    updateThrust(deltaTime);

    // copy velocity so we can use it later for acceleration
    glm::vec3 oldVelocity = getVelocity();

    // calculate speed
    _speed = glm::length(_velocity);

    // update the movement of the hand and process handshaking with other avatars...
    updateHandMovementAndTouching(deltaTime);

    // apply gravity
    // For gravity, always move the avatar by the amount driven by gravity, so that the collision
    // routines will detect it and collide every frame when pulled by gravity to a surface
    const float MIN_DISTANCE_AFTER_COLLISION_FOR_GRAVITY = 0.02f;
    if (glm::length(_position - _lastCollisionPosition) > MIN_DISTANCE_AFTER_COLLISION_FOR_GRAVITY) {
        _velocity += _scale * _gravity * (GRAVITY_EARTH * deltaTime);
    }

    // Only collide if we are not moving to a target
    if (_isCollisionsOn && (glm::length(_moveTarget) < EPSILON)) {

        Camera* myCamera = Application::getInstance()->getCamera();

        if (myCamera->getMode() == CAMERA_MODE_FIRST_PERSON && !OculusManager::isConnected()) {
            _collisionRadius = myCamera->getAspectRatio() * (myCamera->getNearClip() / cos(myCamera->getFieldOfView() / 2.f));
            _collisionRadius *= COLLISION_RADIUS_SCALAR;
        } else {
            _collisionRadius = getHeight() * COLLISION_RADIUS_SCALE;
        }

        updateCollisionWithEnvironment(deltaTime);
        updateCollisionWithVoxels(deltaTime);
        updateAvatarCollisions(deltaTime);
    }

    // add thrust to velocity
    _velocity += _thrust * deltaTime;

    // update body yaw by body yaw delta
    orientation = orientation * glm::quat(glm::radians(
        glm::vec3(_bodyPitchDelta, _bodyYawDelta, _bodyRollDelta) * deltaTime));
    // decay body rotation momentum

    const float BODY_SPIN_FRICTION = 7.5f;
    float bodySpinMomentum = 1.0 - BODY_SPIN_FRICTION * deltaTime;
    if (bodySpinMomentum < 0.0f) { bodySpinMomentum = 0.0f; }
    _bodyPitchDelta *= bodySpinMomentum;
    _bodyYawDelta *= bodySpinMomentum;
    _bodyRollDelta *= bodySpinMomentum;

    float MINIMUM_ROTATION_RATE = 2.0f;
    if (fabs(_bodyYawDelta) < MINIMUM_ROTATION_RATE) { _bodyYawDelta = 0.f; }
    if (fabs(_bodyRollDelta) < MINIMUM_ROTATION_RATE) { _bodyRollDelta = 0.f; }
    if (fabs(_bodyPitchDelta) < MINIMUM_ROTATION_RATE) { _bodyPitchDelta = 0.f; }

    const float MAX_STATIC_FRICTION_VELOCITY = 0.5f;
    const float STATIC_FRICTION_STRENGTH = _scale * 20.f;
    applyStaticFriction(deltaTime, _velocity, MAX_STATIC_FRICTION_VELOCITY, STATIC_FRICTION_STRENGTH);

    // Damp avatar velocity
    const float LINEAR_DAMPING_STRENGTH = 0.5f;
    const float SPEED_BRAKE_POWER = _scale * 10.0f;
    const float SQUARED_DAMPING_STRENGTH = 0.007f;

    const float SLOW_NEAR_RADIUS = 5.f;
    float linearDamping = LINEAR_DAMPING_STRENGTH;
    const float NEAR_AVATAR_DAMPING_FACTOR = 50.f;
    if (_distanceToNearestAvatar < _scale * SLOW_NEAR_RADIUS) {
        linearDamping *= 1.f + NEAR_AVATAR_DAMPING_FACTOR *
                            ((SLOW_NEAR_RADIUS - _distanceToNearestAvatar) / SLOW_NEAR_RADIUS);
    }
    if (_speedBrakes) {
        applyDamping(deltaTime, _velocity,  linearDamping * SPEED_BRAKE_POWER, SQUARED_DAMPING_STRENGTH * SPEED_BRAKE_POWER);
    } else {
        applyDamping(deltaTime, _velocity, linearDamping, SQUARED_DAMPING_STRENGTH);
    }

    // update the euler angles
    setOrientation(orientation);

    // Compute instantaneous acceleration
    float forwardAcceleration = glm::length(glm::dot(getBodyFrontDirection(), getVelocity() - oldVelocity)) / deltaTime;
    const float OCULUS_ACCELERATION_PULL_THRESHOLD = 1.0f;
    const int OCULUS_YAW_OFFSET_THRESHOLD = 10;

    if (!Application::getInstance()->getFaceshift()->isActive() && OculusManager::isConnected() &&
            fabsf(forwardAcceleration) > OCULUS_ACCELERATION_PULL_THRESHOLD &&
            fabs(_head.getYaw()) > OCULUS_YAW_OFFSET_THRESHOLD) {
            
        // if we're wearing the oculus
        // and this acceleration is above the pull threshold
        // and the head yaw if off the body by more than OCULUS_YAW_OFFSET_THRESHOLD

        // match the body yaw to the oculus yaw
        _bodyYaw = getAbsoluteHeadYaw();

        // set the head yaw to zero for this draw
        _head.setYaw(0);

        // correct the oculus yaw offset
        OculusManager::updateYawOffset();
    }

    const float WALKING_SPEED_THRESHOLD = 0.2f;
    // use speed and angular velocity to determine walking vs. standing
    if (_speed + fabs(_bodyYawDelta) > WALKING_SPEED_THRESHOLD) {
        _mode = AVATAR_MODE_WALKING;
    } else {
        _mode = AVATAR_MODE_INTERACTING;
    }

    // update moving flag based on speed
    const float MOVING_SPEED_THRESHOLD = 0.01f;
    _moving = _speed > MOVING_SPEED_THRESHOLD;

    // If a move target is set, update position explicitly
    const float MOVE_FINISHED_TOLERANCE = 0.1f;
    const float MOVE_SPEED_FACTOR = 2.f;
    const int MOVE_TARGET_MAX_STEPS = 250;
    if ((glm::length(_moveTarget) > EPSILON) && (_moveTargetStepCounter < MOVE_TARGET_MAX_STEPS))  {
        if (glm::length(_position - _moveTarget) > MOVE_FINISHED_TOLERANCE) {
            _position += (_moveTarget - _position) * (deltaTime * MOVE_SPEED_FACTOR);
            _moveTargetStepCounter++;
        } else {
            //  Move completed
            _moveTarget = glm::vec3(0,0,0);
            _moveTargetStepCounter = 0;
        }
    }

    updateChatCircle(deltaTime);

    //  Get any position, velocity, or rotation update from Grab Drag controller
    glm::vec3 moveFromGrab = _hand.getAndResetGrabDelta();
    if (glm::length(moveFromGrab) > EPSILON) {
        _position += moveFromGrab;
        _velocity = glm::vec3(0, 0, 0);
    }
    _velocity += _hand.getAndResetGrabDeltaVelocity();
    glm::quat deltaRotation = _hand.getAndResetGrabRotation();
    const float GRAB_CONTROLLER_TURN_SCALING = 0.5f;
    glm::vec3 euler = safeEulerAngles(deltaRotation) * GRAB_CONTROLLER_TURN_SCALING;
    //  Adjust body yaw by yaw from controller
    setOrientation(glm::angleAxis(-euler.y, glm::vec3(0, 1, 0)) * getOrientation());
    //  Adjust head pitch from controller
    getHead().setPitch(getHead().getPitch() - euler.x);

    _position += _velocity * deltaTime;

    // update avatar skeleton and simulate hand and head
    _hand.simulate(deltaTime, true);
    _skeletonModel.simulate(deltaTime);
    _head.setBodyRotation(glm::vec3(_bodyPitch, _bodyYaw, _bodyRoll));
    glm::vec3 headPosition;
    if (!_skeletonModel.getHeadPosition(headPosition)) {
        headPosition = _position;
    }
    _head.setPosition(headPosition);
    _head.setScale(_scale);
    _head.simulate(deltaTime, true);

    // Zero thrust out now that we've added it to velocity in this frame
    _thrust = glm::vec3(0, 0, 0);

}

const float MAX_PITCH = 90.0f;

//  Update avatar head rotation with sensor data
void MyAvatar::updateFromGyros(bool turnWithHead) {
    Faceshift* faceshift = Application::getInstance()->getFaceshift();
    glm::vec3 estimatedPosition, estimatedRotation;

    if (faceshift->isActive()) {
        estimatedPosition = faceshift->getHeadTranslation();
        estimatedRotation = safeEulerAngles(faceshift->getHeadRotation());
        //  Rotate the body if the head is turned beyond the screen
        if (turnWithHead) {
            const float FACESHIFT_YAW_TURN_SENSITIVITY = 0.5f;
            const float FACESHIFT_MIN_YAW_TURN = 15.f;
            const float FACESHIFT_MAX_YAW_TURN = 50.f;
            if ( (fabs(estimatedRotation.y) > FACESHIFT_MIN_YAW_TURN) &&
                 (fabs(estimatedRotation.y) < FACESHIFT_MAX_YAW_TURN) ) {
                if (estimatedRotation.y > 0.f) {
                    _bodyYawDelta += (estimatedRotation.y - FACESHIFT_MIN_YAW_TURN) * FACESHIFT_YAW_TURN_SENSITIVITY;
                } else {
                    _bodyYawDelta += (estimatedRotation.y + FACESHIFT_MIN_YAW_TURN) * FACESHIFT_YAW_TURN_SENSITIVITY;
                }
            }
        }
    } else {
        // restore rotation, lean to neutral positions
        const float RESTORE_RATE = 0.05f;
        _head.setYaw(glm::mix(_head.getYaw(), 0.0f, RESTORE_RATE));
        _head.setRoll(glm::mix(_head.getRoll(), 0.0f, RESTORE_RATE));
        _head.setLeanSideways(glm::mix(_head.getLeanSideways(), 0.0f, RESTORE_RATE));
        _head.setLeanForward(glm::mix(_head.getLeanForward(), 0.0f, RESTORE_RATE));
        return;
    }

    // Set the rotation of the avatar's head (as seen by others, not affecting view frustum)
    // to be scaled.  Pitch is greater to emphasize nodding behavior / synchrony.
    const float AVATAR_HEAD_PITCH_MAGNIFY = 1.0f;
    const float AVATAR_HEAD_YAW_MAGNIFY = 1.0f;
    const float AVATAR_HEAD_ROLL_MAGNIFY = 1.0f;
    _head.setPitch(estimatedRotation.x * AVATAR_HEAD_PITCH_MAGNIFY);
    _head.setYaw(estimatedRotation.y * AVATAR_HEAD_YAW_MAGNIFY);
    _head.setRoll(estimatedRotation.z * AVATAR_HEAD_ROLL_MAGNIFY);

    //  Update torso lean distance based on accelerometer data
    const float TORSO_LENGTH = 0.5f;
    glm::vec3 relativePosition = estimatedPosition - glm::vec3(0.0f, -TORSO_LENGTH, 0.0f);
    const float MAX_LEAN = 45.0f;
    _head.setLeanSideways(glm::clamp(glm::degrees(atanf(relativePosition.x * _leanScale / TORSO_LENGTH)),
        -MAX_LEAN, MAX_LEAN));
    _head.setLeanForward(glm::clamp(glm::degrees(atanf(relativePosition.z * _leanScale / TORSO_LENGTH)),
        -MAX_LEAN, MAX_LEAN));

    // if Faceshift drive is enabled, set the avatar drive based on the head position
    if (!Application::getInstance()->getMenu()->isOptionChecked(MenuOption::MoveWithLean)) {
        return;
    }

    //  Move with Lean by applying thrust proportional to leaning
    glm::quat orientation = _head.getCameraOrientation();
    glm::vec3 front = orientation * IDENTITY_FRONT;
    glm::vec3 right = orientation * IDENTITY_RIGHT;
    float leanForward = _head.getLeanForward();
    float leanSideways = _head.getLeanSideways();

    //  Degrees of 'dead zone' when leaning, and amount of acceleration to apply to lean angle
    const float LEAN_FWD_DEAD_ZONE = 15.f;
    const float LEAN_SIDEWAYS_DEAD_ZONE = 10.f;
    const float LEAN_FWD_THRUST_SCALE = 4.f;
    const float LEAN_SIDEWAYS_THRUST_SCALE = 3.f;

    if (fabs(leanForward) > LEAN_FWD_DEAD_ZONE) {
        if (leanForward > 0.f) {
            addThrust(front * -(leanForward - LEAN_FWD_DEAD_ZONE) * LEAN_FWD_THRUST_SCALE);
        } else {
            addThrust(front * -(leanForward + LEAN_FWD_DEAD_ZONE) * LEAN_FWD_THRUST_SCALE);
        }
    }
    if (fabs(leanSideways) > LEAN_SIDEWAYS_DEAD_ZONE) {
        if (leanSideways > 0.f) {
            addThrust(right * -(leanSideways - LEAN_SIDEWAYS_DEAD_ZONE) * LEAN_SIDEWAYS_THRUST_SCALE);
        } else {
            addThrust(right * -(leanSideways + LEAN_SIDEWAYS_DEAD_ZONE) * LEAN_SIDEWAYS_THRUST_SCALE);
        }
    }
}

static TextRenderer* textRenderer() {
    static TextRenderer* renderer = new TextRenderer(SANS_FONT_FAMILY, 24, -1, false, TextRenderer::SHADOW_EFFECT);
    return renderer;
}

void MyAvatar::renderDebugBodyPoints() {
    glm::vec3 torsoPosition(getPosition());
    glm::vec3 headPosition(getHead().getEyePosition());
    float torsoToHead = glm::length(headPosition - torsoPosition);
    glm::vec3 position;
    printf("head-above-torso %.2f, scale = %0.2f\n", torsoToHead, getScale());

    //  Torso Sphere
    position = torsoPosition;
    glPushMatrix();
    glColor4f(0, 1, 0, .5f);
    glTranslatef(position.x, position.y, position.z);
    glutSolidSphere(0.2, 10, 10);
    glPopMatrix();

    //  Head Sphere
    position = headPosition;
    glPushMatrix();
    glColor4f(0, 1, 0, .5f);
    glTranslatef(position.x, position.y, position.z);
    glutSolidSphere(0.15, 10, 10);
    glPopMatrix();


}
void MyAvatar::render(bool forceRenderHead) {

    // render body
    renderBody(forceRenderHead);

    //renderDebugBodyPoints();

    if (!_chatMessage.empty()) {
        int width = 0;
        int lastWidth = 0;
        for (string::iterator it = _chatMessage.begin(); it != _chatMessage.end(); it++) {
            width += (lastWidth = textRenderer()->computeWidth(*it));
        }
        glPushMatrix();

        glm::vec3 chatPosition = getHead().getEyePosition() + getBodyUpDirection() * CHAT_MESSAGE_HEIGHT * _scale;
        glTranslatef(chatPosition.x, chatPosition.y, chatPosition.z);
        glm::quat chatRotation = Application::getInstance()->getCamera()->getRotation();
        glm::vec3 chatAxis = glm::axis(chatRotation);
        glRotatef(glm::angle(chatRotation), chatAxis.x, chatAxis.y, chatAxis.z);


        glColor3f(0, 0.8f, 0);
        glRotatef(180, 0, 1, 0);
        glRotatef(180, 0, 0, 1);
        glScalef(_scale * CHAT_MESSAGE_SCALE, _scale * CHAT_MESSAGE_SCALE, 1.0f);

        glDisable(GL_LIGHTING);
        glDepthMask(false);
        if (_keyState == NO_KEY_DOWN) {
            textRenderer()->draw(-width / 2.0f, 0, _chatMessage.c_str());

        } else {
            // rather than using substr and allocating a new string, just replace the last
            // character with a null, then restore it
            int lastIndex = _chatMessage.size() - 1;
            char lastChar = _chatMessage[lastIndex];
            _chatMessage[lastIndex] = '\0';
            textRenderer()->draw(-width / 2.0f, 0, _chatMessage.c_str());
            _chatMessage[lastIndex] = lastChar;
            glColor3f(0, 1, 0);
            textRenderer()->draw(width / 2.0f - lastWidth, 0, _chatMessage.c_str() + lastIndex);
        }
        glEnable(GL_LIGHTING);
        glDepthMask(true);

        glPopMatrix();
    }
}

void MyAvatar::renderHeadMouse() const {
    // TODO? resurrect headMouse stuff?
    /*
    //  Display small target box at center or head mouse target that can also be used to measure LOD
    glColor3f(1.0, 1.0, 1.0);
    glDisable(GL_LINE_SMOOTH);
    const int PIXEL_BOX = 16;
    glBegin(GL_LINES);
    glVertex2f(_headMouseX - PIXEL_BOX/2, _headMouseY);
    glVertex2f(_headMouseX + PIXEL_BOX/2, _headMouseY);
    glVertex2f(_headMouseX, _headMouseY - PIXEL_BOX/2);
    glVertex2f(_headMouseX, _headMouseY + PIXEL_BOX/2);
    glEnd();
    glEnable(GL_LINE_SMOOTH);
    glColor3f(1.f, 0.f, 0.f);
    glPointSize(3.0f);
    glDisable(GL_POINT_SMOOTH);
    glBegin(GL_POINTS);
    glVertex2f(_headMouseX - 1, _headMouseY + 1);
    glEnd();
    //  If Faceshift is active, show eye pitch and yaw as separate pointer
    if (_faceshift.isActive()) {
        const float EYE_TARGET_PIXELS_PER_DEGREE = 40.0;
        int eyeTargetX = (_glWidget->width() / 2) -  _faceshift.getEstimatedEyeYaw() * EYE_TARGET_PIXELS_PER_DEGREE;
        int eyeTargetY = (_glWidget->height() / 2) -  _faceshift.getEstimatedEyePitch() * EYE_TARGET_PIXELS_PER_DEGREE;

        glColor3f(0.0, 1.0, 1.0);
        glDisable(GL_LINE_SMOOTH);
        glBegin(GL_LINES);
        glVertex2f(eyeTargetX - PIXEL_BOX/2, eyeTargetY);
        glVertex2f(eyeTargetX + PIXEL_BOX/2, eyeTargetY);
        glVertex2f(eyeTargetX, eyeTargetY - PIXEL_BOX/2);
        glVertex2f(eyeTargetX, eyeTargetY + PIXEL_BOX/2);
        glEnd();

    }
    */
}

void MyAvatar::renderTransmitterPickRay() const {
    if (_transmitterPickStart != _transmitterPickEnd) {
        Glower glower;
        const float TRANSMITTER_PICK_COLOR[] = { 1.0f, 1.0f, 0.0f };
        glColor3fv(TRANSMITTER_PICK_COLOR);
        glLineWidth(3.0f);
        glBegin(GL_LINES);
        glVertex3f(_transmitterPickStart.x, _transmitterPickStart.y, _transmitterPickStart.z);
        glVertex3f(_transmitterPickEnd.x, _transmitterPickEnd.y, _transmitterPickEnd.z);
        glEnd();
        glLineWidth(1.0f);

        glPushMatrix();
        glTranslatef(_transmitterPickEnd.x, _transmitterPickEnd.y, _transmitterPickEnd.z);

        const float PICK_END_RADIUS = 0.025f;
        glutSolidSphere(PICK_END_RADIUS, 8, 8);

        glPopMatrix();
    }
}

void MyAvatar::renderTransmitterLevels(int width, int height) const {
    //  Show hand transmitter data if detected
    if (_transmitter.isConnected()) {
        _transmitter.renderLevels(width, height);
    }
}

void MyAvatar::saveData(QSettings* settings) {
    settings->beginGroup("Avatar");

    settings->setValue("bodyYaw", _bodyYaw);
    settings->setValue("bodyPitch", _bodyPitch);
    settings->setValue("bodyRoll", _bodyRoll);

    settings->setValue("headPitch", _head.getPitch());

    settings->setValue("position_x", _position.x);
    settings->setValue("position_y", _position.y);
    settings->setValue("position_z", _position.z);

    settings->setValue("pupilDilation", _head.getPupilDilation());

    settings->setValue("leanScale", _leanScale);
    settings->setValue("scale", _targetScale);

    settings->endGroup();
}

void MyAvatar::loadData(QSettings* settings) {
    settings->beginGroup("Avatar");

    // in case settings is corrupt or missing loadSetting() will check for NaN
    _bodyYaw = loadSetting(settings, "bodyYaw", 0.0f);
    _bodyPitch = loadSetting(settings, "bodyPitch", 0.0f);
    _bodyRoll = loadSetting(settings, "bodyRoll", 0.0f);

    _head.setPitch(loadSetting(settings, "headPitch", 0.0f));

    _position.x = loadSetting(settings, "position_x", 0.0f);
    _position.y = loadSetting(settings, "position_y", 0.0f);
    _position.z = loadSetting(settings, "position_z", 0.0f);

    _head.setPupilDilation(settings->value("pupilDilation", 0.0f).toFloat());

    _leanScale = loadSetting(settings, "leanScale", 0.05f);
    _targetScale = loadSetting(settings, "scale", 1.0f);
    setScale(_scale);
    Application::getInstance()->getCamera()->setScale(_scale);

    settings->endGroup();
}

void MyAvatar::sendKillAvatar() {
    QByteArray killPacket = byteArrayWithPopluatedHeader(PacketTypeKillAvatar);
    NodeList::getInstance()->broadcastToNodes(killPacket, NodeSet() << NodeType::AvatarMixer);
}

void MyAvatar::orbit(const glm::vec3& position, int deltaX, int deltaY) {
    // first orbit horizontally
    glm::quat orientation = getOrientation();
    const float ANGULAR_SCALE = 0.5f;
    glm::quat rotation = glm::angleAxis(deltaX * -ANGULAR_SCALE, orientation * IDENTITY_UP);
    setPosition(position + rotation * (getPosition() - position));
    orientation = rotation * orientation;
    setOrientation(orientation);
    
    // then vertically
    float oldPitch = _head.getPitch();
    _head.setPitch(oldPitch + deltaY * -ANGULAR_SCALE);
    rotation = glm::angleAxis(_head.getPitch() - oldPitch, orientation * IDENTITY_RIGHT);

    setPosition(position + rotation * (getPosition() - position));
}

void MyAvatar::updateLookAtTargetAvatar(glm::vec3 &eyePosition) {
    Application* applicationInstance = Application::getInstance();
    
    if (!applicationInstance->isMousePressed()) {
        glm::vec3 mouseOrigin = applicationInstance->getMouseRayOrigin();
        glm::vec3 mouseDirection = applicationInstance->getMouseRayDirection();

        foreach (const AvatarSharedPointer& avatarPointer, Application::getInstance()->getAvatarManager().getAvatarHash()) {
            Avatar* avatar = static_cast<Avatar*>(avatarPointer.data());
            if (avatar == static_cast<Avatar*>(this)) {
                continue;
            }
            float distance;
            if (avatar->findRayIntersection(mouseOrigin, mouseDirection, distance)) {
                // rescale to compensate for head embiggening
                eyePosition = (avatar->getHead().calculateAverageEyePosition() - avatar->getHead().getScalePivot()) *
                    (avatar->getScale() / avatar->getHead().getScale()) + avatar->getHead().getScalePivot();
                _lookAtTargetAvatar = avatarPointer;
                return;
            }
        }
        _lookAtTargetAvatar.clear();
    }
}

void MyAvatar::clearLookAtTargetAvatar() {
    _lookAtTargetAvatar.clear();
}

float MyAvatar::getAbsoluteHeadYaw() const {
    return glm::yaw(_head.getOrientation());
}

glm::vec3 MyAvatar::getUprightHeadPosition() const {
    return _position + getWorldAlignedOrientation() * glm::vec3(0.0f, getPelvisToHeadLength(), 0.0f);
}

void MyAvatar::renderBody(bool forceRenderHead) {
    //  Render the body's voxels and head
    _skeletonModel.render(1.0f);

    //  Render head so long as the camera isn't inside it
    const float RENDER_HEAD_CUTOFF_DISTANCE = 0.10f;
    Camera* myCamera = Application::getInstance()->getCamera();
    if (forceRenderHead || (glm::length(myCamera->getPosition() - _head.calculateAverageEyePosition()) > RENDER_HEAD_CUTOFF_DISTANCE)) {
        _head.render(1.0f);
    }
    _hand.render(true);
}

void MyAvatar::updateThrust(float deltaTime) {
    //
    //  Gather thrust information from keyboard and sensors to apply to avatar motion
    //
    glm::quat orientation = getHead().getCameraOrientation();
    glm::vec3 front = orientation * IDENTITY_FRONT;
    glm::vec3 right = orientation * IDENTITY_RIGHT;
    glm::vec3 up = orientation * IDENTITY_UP;

    const float THRUST_MAG_UP = 800.0f;
    const float THRUST_MAG_DOWN = 300.f;
    const float THRUST_MAG_FWD = 500.f;
    const float THRUST_MAG_BACK = 300.f;
    const float THRUST_MAG_LATERAL = 250.f;
    const float THRUST_JUMP = 120.f;

    //  Add Thrusts from keyboard
    _thrust += _driveKeys[FWD] * _scale * THRUST_MAG_FWD * _thrustMultiplier * deltaTime * front;
    _thrust -= _driveKeys[BACK] * _scale * THRUST_MAG_BACK *  _thrustMultiplier * deltaTime * front;
    _thrust += _driveKeys[RIGHT] * _scale * THRUST_MAG_LATERAL * _thrustMultiplier * deltaTime * right;
    _thrust -= _driveKeys[LEFT] * _scale * THRUST_MAG_LATERAL * _thrustMultiplier * deltaTime * right;
    _thrust += _driveKeys[UP] * _scale * THRUST_MAG_UP * _thrustMultiplier * deltaTime * up;
    _thrust -= _driveKeys[DOWN] * _scale * THRUST_MAG_DOWN * _thrustMultiplier * deltaTime * up;
    _bodyYawDelta -= _driveKeys[ROT_RIGHT] * YAW_MAG * deltaTime;
    _bodyYawDelta += _driveKeys[ROT_LEFT] * YAW_MAG * deltaTime;
    _head.setPitch(_head.getPitch() + (_driveKeys[ROT_UP] - _driveKeys[ROT_DOWN]) * PITCH_MAG * deltaTime);

    //  If thrust keys are being held down, slowly increase thrust to allow reaching great speeds
    if (_driveKeys[FWD] || _driveKeys[BACK] || _driveKeys[RIGHT] || _driveKeys[LEFT] || _driveKeys[UP] || _driveKeys[DOWN]) {
        const float THRUST_INCREASE_RATE = 1.05f;
        const float MAX_THRUST_MULTIPLIER = 75.0f;
        //printf("m = %.3f\n", _thrustMultiplier);
        if (_thrustMultiplier < MAX_THRUST_MULTIPLIER) {
            _thrustMultiplier *= 1.f + deltaTime * THRUST_INCREASE_RATE;
        }
    } else {
        _thrustMultiplier = 1.f;
    }

    //  Add one time jumping force if requested
    if (_shouldJump) {
        if (glm::length(_gravity) > EPSILON) {
            _thrust += _scale * THRUST_JUMP * up;
        }
        _shouldJump = false;
    }

    //  Add thrusts from Transmitter
    const Menu* menu = Application::getInstance()->getMenu();
    if(menu){
      if (menu->isOptionChecked(MenuOption::TransmitterDrive) && _transmitter.isConnected()) {
        _transmitter.checkForLostTransmitter();
        glm::vec3 rotation = _transmitter.getEstimatedRotation();
        const float TRANSMITTER_MIN_RATE = 1.f;
        const float TRANSMITTER_MIN_YAW_RATE = 4.f;
        const float TRANSMITTER_LATERAL_FORCE_SCALE = 5.f;
        const float TRANSMITTER_FWD_FORCE_SCALE = 25.f;
        const float TRANSMITTER_UP_FORCE_SCALE = 100.f;
        const float TRANSMITTER_YAW_SCALE = 10.0f;
        const float TRANSMITTER_LIFT_SCALE = 3.f;
        const float TOUCH_POSITION_RANGE_HALF = 32767.f;
        if (fabs(rotation.z) > TRANSMITTER_MIN_RATE) {
	  _thrust += rotation.z * TRANSMITTER_LATERAL_FORCE_SCALE * deltaTime * right;
        }
        if (fabs(rotation.x) > TRANSMITTER_MIN_RATE) {
	  _thrust += -rotation.x * TRANSMITTER_FWD_FORCE_SCALE * deltaTime * front;
        }
        if (fabs(rotation.y) > TRANSMITTER_MIN_YAW_RATE) {
	  _bodyYawDelta += rotation.y * TRANSMITTER_YAW_SCALE * deltaTime;
        }
        if (_transmitter.getTouchState()->state == 'D') {
	  _thrust += TRANSMITTER_UP_FORCE_SCALE *
            (float)(_transmitter.getTouchState()->y - TOUCH_POSITION_RANGE_HALF) / TOUCH_POSITION_RANGE_HALF *
            TRANSMITTER_LIFT_SCALE *
            deltaTime *
            up;
        }
      }
    }
    //  Add thrust and rotation from hand controllers
    const float THRUST_MAG_HAND_JETS = THRUST_MAG_FWD;
    const float JOYSTICK_YAW_MAG = YAW_MAG;
    const float JOYSTICK_PITCH_MAG = PITCH_MAG * 0.5f;
    const int THRUST_CONTROLLER = 0;
    const int VIEW_CONTROLLER = 1;
    for (size_t i = 0; i < getHand().getPalms().size(); ++i) {
        PalmData& palm = getHand().getPalms()[i];
        if (palm.isActive() && (palm.getSixenseID() == THRUST_CONTROLLER)) {
            if (palm.getJoystickY() != 0.f) {
                FingerData& finger = palm.getFingers()[0];
                if (finger.isActive()) {
                }
                _thrust += front * _scale * THRUST_MAG_HAND_JETS * palm.getJoystickY() * _thrustMultiplier * deltaTime;
            }
            if (palm.getJoystickX() != 0.f) {
                _thrust += right * _scale * THRUST_MAG_HAND_JETS * palm.getJoystickX() * _thrustMultiplier * deltaTime;
            }
        } else if (palm.isActive() && (palm.getSixenseID() == VIEW_CONTROLLER)) {
            if (palm.getJoystickX() != 0.f) {
                _bodyYawDelta -= palm.getJoystickX() * JOYSTICK_YAW_MAG * deltaTime;
            }
            if (palm.getJoystickY() != 0.f) {
                getHand().setPitchUpdate(getHand().getPitchUpdate() +
                                         (palm.getJoystickY() * JOYSTICK_PITCH_MAG * deltaTime));
            }
        }

    }

    //  Update speed brake status
    const float MIN_SPEED_BRAKE_VELOCITY = _scale * 0.4f;
    if ((glm::length(_thrust) == 0.0f) && _isThrustOn && (glm::length(_velocity) > MIN_SPEED_BRAKE_VELOCITY)) {
        _speedBrakes = true;
    }

    if (_speedBrakes && (glm::length(_velocity) < MIN_SPEED_BRAKE_VELOCITY)) {
        _speedBrakes = false;
    }
    _isThrustOn = (glm::length(_thrust) > EPSILON);
}

void MyAvatar::updateHandMovementAndTouching(float deltaTime) {
    glm::quat orientation = getOrientation();

    // reset hand and arm positions according to hand movement
    glm::vec3 up = orientation * IDENTITY_UP;

    bool pointing = false;
    if (glm::length(_mouseRayDirection) > EPSILON && !Application::getInstance()->isMouseHidden()) {
        // confine to the approximate shoulder plane
        glm::vec3 pointDirection = _mouseRayDirection;
        if (glm::dot(_mouseRayDirection, up) > 0.0f) {
            glm::vec3 projectedVector = glm::cross(up, glm::cross(_mouseRayDirection, up));
            if (glm::length(projectedVector) > EPSILON) {
                pointDirection = glm::normalize(projectedVector);
            }
        }
        glm::vec3 shoulderPosition;
        if (_skeletonModel.getRightShoulderPosition(shoulderPosition)) {
            glm::vec3 farVector = _mouseRayOrigin + pointDirection * (float)TREE_SCALE - shoulderPosition;
            const float ARM_RETRACTION = 0.75f;
            float retractedLength = _skeletonModel.getRightArmLength() * ARM_RETRACTION;
            setHandPosition(shoulderPosition + glm::normalize(farVector) * retractedLength);
            pointing = true;
        }
    }

    if (_mousePressed) {
        _handState = HAND_STATE_GRASPING;
    } else if (pointing) {
        _handState = HAND_STATE_POINTING;
    } else {
        _handState = HAND_STATE_NULL;
    }
}

void MyAvatar::updateCollisionWithEnvironment(float deltaTime) {
    glm::vec3 up = getBodyUpDirection();
    float radius = _collisionRadius;
    const float ENVIRONMENT_SURFACE_ELASTICITY = 1.0f;
    const float ENVIRONMENT_SURFACE_DAMPING = 0.01f;
    const float ENVIRONMENT_COLLISION_FREQUENCY = 0.05f;
    glm::vec3 penetration;
    float pelvisFloatingHeight = getPelvisFloatingHeight();
    if (Application::getInstance()->getEnvironment()->findCapsulePenetration(
            _position - up * (pelvisFloatingHeight - radius),
            _position + up * (getHeight() - pelvisFloatingHeight + radius), radius, penetration)) {
        _lastCollisionPosition = _position;
        updateCollisionSound(penetration, deltaTime, ENVIRONMENT_COLLISION_FREQUENCY);
        applyHardCollision(penetration, ENVIRONMENT_SURFACE_ELASTICITY, ENVIRONMENT_SURFACE_DAMPING);
    }
}


void MyAvatar::updateCollisionWithVoxels(float deltaTime) {
    float radius = _collisionRadius;
    const float VOXEL_ELASTICITY = 0.4f;
    const float VOXEL_DAMPING = 0.0f;
    const float VOXEL_COLLISION_FREQUENCY = 0.5f;
    glm::vec3 penetration;
    float pelvisFloatingHeight = getPelvisFloatingHeight();
    if (Application::getInstance()->getVoxels()->findCapsulePenetration(
            _position - glm::vec3(0.0f, pelvisFloatingHeight - radius, 0.0f),
            _position + glm::vec3(0.0f, getHeight() - pelvisFloatingHeight + radius, 0.0f), radius, penetration)) {
        _lastCollisionPosition = _position;
        updateCollisionSound(penetration, deltaTime, VOXEL_COLLISION_FREQUENCY);
        applyHardCollision(penetration, VOXEL_ELASTICITY, VOXEL_DAMPING);
    }
}

void MyAvatar::applyHardCollision(const glm::vec3& penetration, float elasticity, float damping) {
    //
    //  Update the avatar in response to a hard collision.  Position will be reset exactly
    //  to outside the colliding surface.  Velocity will be modified according to elasticity.
    //
    //  if elasticity = 1.0, collision is inelastic.
    //  if elasticity > 1.0, collision is elastic.
    //
    _position -= penetration;
    static float HALTING_VELOCITY = 0.2f;
    // cancel out the velocity component in the direction of penetration
    float penetrationLength = glm::length(penetration);
    if (penetrationLength > EPSILON) {
        _elapsedTimeSinceCollision = 0.0f;
        glm::vec3 direction = penetration / penetrationLength;
        _velocity -= glm::dot(_velocity, direction) * direction * elasticity;
        _velocity *= glm::clamp(1.f - damping, 0.0f, 1.0f);
        if ((glm::length(_velocity) < HALTING_VELOCITY) && (glm::length(_thrust) == 0.f)) {
            // If moving really slowly after a collision, and not applying forces, stop altogether
            _velocity *= 0.f;
        }
    }
}

void MyAvatar::updateCollisionSound(const glm::vec3 &penetration, float deltaTime, float frequency) {
    //  consider whether to have the collision make a sound
    const float AUDIBLE_COLLISION_THRESHOLD = 0.02f;
    const float COLLISION_LOUDNESS = 1.f;
    const float DURATION_SCALING = 0.004f;
    const float NOISE_SCALING = 0.1f;
    glm::vec3 velocity = _velocity;
    glm::vec3 gravity = getGravity();

    if (glm::length(gravity) > EPSILON) {
        //  If gravity is on, remove the effect of gravity on velocity for this
        //  frame, so that we are not constantly colliding with the surface
        velocity -= _scale * glm::length(gravity) * GRAVITY_EARTH * deltaTime * glm::normalize(gravity);
    }
    float velocityTowardCollision = glm::dot(velocity, glm::normalize(penetration));
    float velocityTangentToCollision = glm::length(velocity) - velocityTowardCollision;

    if (velocityTowardCollision > AUDIBLE_COLLISION_THRESHOLD) {
        //  Volume is proportional to collision velocity
        //  Base frequency is modified upward by the angle of the collision
        //  Noise is a function of the angle of collision
        //  Duration of the sound is a function of both base frequency and velocity of impact
        Application::getInstance()->getAudio()->startCollisionSound(
            std::min(COLLISION_LOUDNESS * velocityTowardCollision, 1.f),
            frequency * (1.f + velocityTangentToCollision / velocityTowardCollision),
            std::min(velocityTangentToCollision / velocityTowardCollision * NOISE_SCALING, 1.f),
            1.f - DURATION_SCALING * powf(frequency, 0.5f) / velocityTowardCollision, true);
    }
}

void MyAvatar::updateAvatarCollisions(float deltaTime) {

    //  Reset detector for nearest avatar
    _distanceToNearestAvatar = std::numeric_limits<float>::max();
    // loop through all the other avatars for potential interactions
}

class SortedAvatar {
public:
    Avatar* avatar;
    float distance;
    glm::vec3 accumulatedCenter;
};

bool operator<(const SortedAvatar& s1, const SortedAvatar& s2) {
    return s1.distance < s2.distance;
}

void MyAvatar::updateChatCircle(float deltaTime) {
  const Menu* menu = Application::getInstance()->getMenu();
  if(menu){
    if (!(_isChatCirclingEnabled = menu->isOptionChecked(MenuOption::ChatCircling))) {
        return;
    }
  }

    // find all circle-enabled members and sort by distance
    QVector<SortedAvatar> sortedAvatars;
    
    foreach (const AvatarSharedPointer& avatarPointer, Application::getInstance()->getAvatarManager().getAvatarHash()) {
        Avatar* avatar = static_cast<Avatar*>(avatarPointer.data());
        if ( ! avatar->isChatCirclingEnabled() ||
                avatar == static_cast<Avatar*>(this)) {
            continue;
        }
    
        SortedAvatar sortedAvatar;
        sortedAvatar.avatar = avatar;
        sortedAvatar.distance = glm::distance(_position, sortedAvatar.avatar->getPosition());
        sortedAvatars.append(sortedAvatar);
    }
    
    qSort(sortedAvatars.begin(), sortedAvatars.end());

    // compute the accumulated centers
    glm::vec3 center = _position;
    for (int i = 0; i < sortedAvatars.size(); i++) {
        SortedAvatar& sortedAvatar = sortedAvatars[i];
        sortedAvatar.accumulatedCenter = (center += sortedAvatar.avatar->getPosition()) / (i + 2.0f);
    }

    // remove members whose accumulated circles are too far away to influence us
    const float CIRCUMFERENCE_PER_MEMBER = 0.5f;
    const float CIRCLE_INFLUENCE_SCALE = 2.0f;
    const float MIN_RADIUS = 0.3f;
    for (int i = sortedAvatars.size() - 1; i >= 0; i--) {
        float radius = qMax(MIN_RADIUS, (CIRCUMFERENCE_PER_MEMBER * (i + 2)) / PI_TIMES_TWO);
        if (glm::distance(_position, sortedAvatars[i].accumulatedCenter) > radius * CIRCLE_INFLUENCE_SCALE) {
            sortedAvatars.remove(i);
        } else {
            break;
        }
    }
    if (sortedAvatars.isEmpty()) {
        return;
    }
    center = sortedAvatars.last().accumulatedCenter;
    float radius = qMax(MIN_RADIUS, (CIRCUMFERENCE_PER_MEMBER * (sortedAvatars.size() + 1)) / PI_TIMES_TWO);

    // compute the average up vector
    glm::vec3 up = getWorldAlignedOrientation() * IDENTITY_UP;
    foreach (const SortedAvatar& sortedAvatar, sortedAvatars) {
        up += sortedAvatar.avatar->getWorldAlignedOrientation() * IDENTITY_UP;
    }
    up = glm::normalize(up);

    // find reasonable corresponding right/front vectors
    glm::vec3 front = glm::cross(up, IDENTITY_RIGHT);
    if (glm::length(front) < EPSILON) {
        front = glm::cross(up, IDENTITY_FRONT);
    }
    front = glm::normalize(front);
    glm::vec3 right = glm::cross(front, up);

    // find our angle and the angular distances to our closest neighbors
    glm::vec3 delta = _position - center;
    glm::vec3 projected = glm::vec3(glm::dot(right, delta), glm::dot(front, delta), 0.0f);
    float myAngle = glm::length(projected) > EPSILON ? atan2f(projected.y, projected.x) : 0.0f;
    float leftDistance = PI_TIMES_TWO;
    float rightDistance = PI_TIMES_TWO;
    foreach (const SortedAvatar& sortedAvatar, sortedAvatars) {
        delta = sortedAvatar.avatar->getPosition() - center;
        projected = glm::vec3(glm::dot(right, delta), glm::dot(front, delta), 0.0f);
        float angle = glm::length(projected) > EPSILON ? atan2f(projected.y, projected.x) : 0.0f;
        if (angle < myAngle) {
            leftDistance = min(myAngle - angle, leftDistance);
            rightDistance = min(PI_TIMES_TWO - (myAngle - angle), rightDistance);

        } else {
            leftDistance = min(PI_TIMES_TWO - (angle - myAngle), leftDistance);
            rightDistance = min(angle - myAngle, rightDistance);
        }
    }

    // if we're on top of a neighbor, we need to randomize so that they don't both go in the same direction
    if (rightDistance == 0.0f && randomBoolean()) {
        swap(leftDistance, rightDistance);
    }

    // split the difference between our neighbors
    float targetAngle = myAngle + (rightDistance - leftDistance) / 4.0f;
    glm::vec3 targetPosition = center + (front * sinf(targetAngle) + right * cosf(targetAngle)) * radius;

    // approach the target position
    const float APPROACH_RATE = 0.05f;
    _position = glm::mix(_position, targetPosition, APPROACH_RATE);
}

void MyAvatar::setGravity(glm::vec3 gravity) {
    _gravity = gravity;
    _head.setGravity(_gravity);

    // use the gravity to determine the new world up direction, if possible
    float gravityLength = glm::length(gravity);
    if (gravityLength > EPSILON) {
        _worldUpDirection = _gravity / -gravityLength;
    } else {
        _worldUpDirection = DEFAULT_UP_DIRECTION;
    }
}

void MyAvatar::setOrientation(const glm::quat& orientation) {
    glm::vec3 eulerAngles = safeEulerAngles(orientation);
    _bodyPitch = eulerAngles.x;
    _bodyYaw = eulerAngles.y;
    _bodyRoll = eulerAngles.z;
}

void MyAvatar::goHome() {
    qDebug("Going Home!");
    setPosition(START_LOCATION);
}

void MyAvatar::increaseSize() {
    if ((1.f + SCALING_RATIO) * _targetScale < MAX_AVATAR_SCALE) {
        _targetScale *= (1.f + SCALING_RATIO);
        qDebug("Changed scale to %f", _targetScale);
    }
}

void MyAvatar::decreaseSize() {
    if (MIN_AVATAR_SCALE < (1.f - SCALING_RATIO) * _targetScale) {
        _targetScale *= (1.f - SCALING_RATIO);
        qDebug("Changed scale to %f", _targetScale);
    }
}

void MyAvatar::resetSize() {
    _targetScale = 1.0f;
    qDebug("Reseted scale to %f", _targetScale);
}

