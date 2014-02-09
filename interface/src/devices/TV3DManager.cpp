//
//  TV3DManager.cpp
//  hifi
//
//  Created by Brad Hefta-Gaub on 12/24/13.
//  Copyright (c) 2013 High Fidelity, Inc. All rights reserved.
//

#include "InterfaceConfig.h"

#include <QOpenGLFramebufferObject>

#include <glm/glm.hpp>


#ifdef WIN32
#include <Systime.h>
#endif

#include "Application.h"

#include "TV3DManager.h"
#include "Menu.h"

int TV3DManager::_screenWidth = 1;
int TV3DManager::_screenHeight = 1;
double TV3DManager::_aspect = 1.0;
eyeFrustum TV3DManager::_leftEye;
eyeFrustum TV3DManager::_rightEye;


bool TV3DManager::isConnected() {
    const Menu* menu = Application::getInstance()->getMenu();
    if (menu) {
	return menu->isOptionChecked(MenuOption::Enable3DTVMode);
    }
    else return false;
}

void TV3DManager::connect() {
    Application* app = Application::getInstance();
    int width = app->getGLWidget()->width();
    int height = app->getGLWidget()->height();
    Camera& camera = *app->getCamera();

    configureCamera(camera, width, height);
}


// The basic strategy of this stereoscopic rendering is explained here:
//    http://www.orthostereo.com/geometryopengl.html
void TV3DManager::setFrustum(Camera& whichCamera) {
    const double DTR = 0.0174532925; // degree to radians
    const double IOD = 0.05; //intraocular distance
    double fovy = whichCamera.getFieldOfView(); // field of view in y-axis
    double nearZ = whichCamera.getNearClip(); // near clipping plane
    double screenZ = Application::getInstance()->getViewFrustum()->getFocalLength(); // screen projection plane

    double top = nearZ * tan(DTR * fovy / 2); //sets top of frustum based on fovy and near clipping plane
    double right = _aspect * top; // sets right of frustum based on aspect ratio
    double frustumshift = (IOD / 2) * nearZ / screenZ;
    
    _leftEye.top = top;
    _leftEye.bottom = -top;
    _leftEye.left = -right + frustumshift;
    _leftEye.right = right + frustumshift;
    _leftEye.modelTranslation = IOD / 2;
    
    _rightEye.top = top;
    _rightEye.bottom = -top;
    _rightEye.left = -right - frustumshift;
    _rightEye.right = right - frustumshift;
    _rightEye.modelTranslation = -IOD / 2;
}

void TV3DManager::configureCamera(Camera& whichCamera, int screenWidth, int screenHeight) {
    if (screenHeight == 0) {
        screenHeight = 1; // prevent divide by 0
    }
    _screenWidth = screenWidth;
    _screenHeight = screenHeight;
    _aspect= (double)_screenWidth / (double)_screenHeight;
    setFrustum(whichCamera);

    glViewport (0, 0, _screenWidth, _screenHeight); // sets drawing viewport
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
}

void TV3DManager::display(Camera& whichCamera) {
    double nearZ = whichCamera.getNearClip(); // near clipping plane
    double farZ = whichCamera.getFarClip(); // far clipping plane

    // left eye portal
    int portalX = 0;
    int portalY = 0;
    int portalW = Application::getInstance()->getGLWidget()->width() / 2;
    int portalH = Application::getInstance()->getGLWidget()->height();

    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glEnable(GL_SCISSOR_TEST);
    // render left side view
    glViewport(portalX, portalY, portalW, portalH);
    glScissor(portalX, portalY, portalW, portalH);

    glPushMatrix();
    {
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity(); // reset projection matrix
        glFrustum(_leftEye.left, _leftEye.right, _leftEye.bottom, _leftEye.top, nearZ, farZ); // set left view frustum
        glTranslatef(_leftEye.modelTranslation, 0.0, 0.0); // translate to cancel parallax
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        Application::getInstance()->displaySide(whichCamera);
    }
    glPopMatrix();
    glDisable(GL_SCISSOR_TEST);

    // render right side view
    portalX = Application::getInstance()->getGLWidget()->width() / 2;
    glEnable(GL_SCISSOR_TEST);
    // render left side view
    glViewport(portalX, portalY, portalW, portalH);
    glScissor(portalX, portalY, portalW, portalH);
    glPushMatrix();
    {
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity(); // reset projection matrix
        glFrustum(_rightEye.left, _rightEye.right, _rightEye.bottom, _rightEye.top, nearZ, farZ); // set left view frustum
        glTranslatef(_rightEye.modelTranslation, 0.0, 0.0); // translate to cancel parallax
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        Application::getInstance()->displaySide(whichCamera);
    }
    glPopMatrix();
    glDisable(GL_SCISSOR_TEST);

    // reset the viewport to how we started
    glViewport(0, 0, Application::getInstance()->getGLWidget()->width(), Application::getInstance()->getGLWidget()->height());
}
