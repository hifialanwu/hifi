//
//  Application.cpp
//  interface
//
//  Created by Andrzej Kapolka on 5/10/13.
//  Copyright (c) 2013 High Fidelity, Inc. All rights reserved.

#ifdef WIN32
#include <Systime.h>
#endif

#include <sstream>

#include <stdlib.h>
#include <cmath>
#include <math.h>


#include <glm/gtx/component_wise.hpp>
#include <glm/gtx/quaternion.hpp>
#include <glm/gtx/vector_angle.hpp>

// include this before QGLWidget, which includes an earlier version of OpenGL
#include "InterfaceConfig.h"

#include <QActionGroup>
#include <QColorDialog>
#include <QDesktopWidget>
#include <QCheckBox>
#include <QImage>
#include <QKeyEvent>
#include <QMainWindow>
#include <QMenuBar>
#include <QMouseEvent>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkDiskCache>
#include <QOpenGLFramebufferObject>
#include <QObject>
#include <QWheelEvent>
#include <QSettings>
#include <QShortcut>
#include <QTimer>
#include <QUrl>
#include <QtDebug>
#include <QFileDialog>
#include <QDesktopServices>
#include <QXmlStreamReader>
#include <QXmlStreamAttributes>
#include <QMediaPlayer>

#include <AudioInjector.h>
#include <Logging.h>
#include <OctalCode.h>
#include <PacketHeaders.h>
#include <PairingHandler.h>
#include <ParticlesScriptingInterface.h>
#include <PerfStat.h>
#include <UUID.h>
#include <VoxelSceneStats.h>

#include "Application.h"
#include "DataServerClient.h"
#include "InterfaceVersion.h"
#include "Menu.h"
#include "Swatch.h"
#include "Util.h"
#include "devices/OculusManager.h"
#include "devices/TV3DManager.h"
#include "renderer/ProgramObject.h"
#include "ui/TextRenderer.h"
#include "InfoView.h"
#include "ui/Snapshot.h"

using namespace std;

//  Starfield information
static unsigned STARFIELD_NUM_STARS = 50000;
static unsigned STARFIELD_SEED = 1;

static const int BANDWIDTH_METER_CLICK_MAX_DRAG_LENGTH = 6; // farther dragged clicks are ignored

const int IDLE_SIMULATE_MSECS = 16;              //  How often should call simulate and other stuff
                                                 //  in the idle loop?  (60 FPS is default)
static QTimer* idleTimer = NULL;

const int STARTUP_JITTER_SAMPLES = NETWORK_BUFFER_LENGTH_SAMPLES_PER_CHANNEL / 2;
//  Startup optimistically with small jitter buffer that
//  will start playback on the second received audio packet.

const int MIRROR_VIEW_TOP_PADDING = 5;
const int MIRROR_VIEW_LEFT_PADDING = 10;
const int MIRROR_VIEW_WIDTH = 265;
const int MIRROR_VIEW_HEIGHT = 215;
const float MIRROR_FULLSCREEN_DISTANCE = 0.35f;
const float MIRROR_REARVIEW_DISTANCE = 0.65f;
const float MIRROR_REARVIEW_BODY_DISTANCE = 2.3f;

const QString CHECK_VERSION_URL = "http://highfidelity.io/latestVersion.xml";
const QString SKIP_FILENAME = QStandardPaths::writableLocation(QStandardPaths::DataLocation) + "/hifi.skipversion";

const int STATS_PELS_PER_LINE = 20;

void messageHandler(QtMsgType type, const QMessageLogContext& context, const QString& message) {
    if (message.size() > 0) {
        QString messageWithNewLine = message + "\n";
        fprintf(stdout, "%s", messageWithNewLine.toLocal8Bit().constData());
        Application::getInstance()->getLogger()->addMessage(messageWithNewLine.toLocal8Bit().constData());
    }
}

Application::Application(int& argc, char** argv, timeval &startup_time) :
    QApplication(argc, argv),
    _window(new QMainWindow(desktop())),
    _glWidget(new GLCanvas()),
    _statsExpanded(false),
    _nodeThread(new QThread(this)),
    _datagramProcessor(),
    _frameCount(0),
    _fps(120.0f),
    _justStarted(true),
    _voxelImporter(NULL),
    _wantToKillLocalVoxels(false),
    _audioScope(256, 200, true),
    _myAvatar(),
    _profile(QString()),
    _mirrorViewRect(QRect(MIRROR_VIEW_LEFT_PADDING, MIRROR_VIEW_TOP_PADDING, MIRROR_VIEW_WIDTH, MIRROR_VIEW_HEIGHT)),
    _mouseX(0),
    _mouseY(0),
    _lastMouseMove(usecTimestampNow()),
    _mouseHidden(false),
    _seenMouseMove(false),
    _touchAvgX(0.0f),
    _touchAvgY(0.0f),
    _isTouchPressed(false),
    _mousePressed(false),
    _isHoverVoxel(false),
    _isHoverVoxelSounding(false),
    _mouseVoxelScale(1.0f / 1024.0f),
    _mouseVoxelScaleInitialized(false),
    _justEditedVoxel(false),
    _isHighlightVoxel(false),
    _nudgeStarted(false),
    _lookingAlongX(false),
    _lookingAwayFromOrigin(true),
    _chatEntryOn(false),
    _menu(new Menu()),
    _audio(&_audioScope, STARTUP_JITTER_SAMPLES),
    _enableProcessVoxelsThread(true),
    _voxelProcessor(),
    _voxelHideShowThread(&_voxels),
    _packetsPerSecond(0),
    _bytesPerSecond(0),
    _recentMaxPackets(0),
    _resetRecentMaxPacketsSoon(true),
    _swatch(NULL),
    _pasteMode(false),
    _logger(new FileLogger(this))
{
    _myAvatar = _avatarManager.getMyAvatar();

    _applicationStartupTime = startup_time;

    switchToResourcesParentIfRequired();
    QFontDatabase::addApplicationFont("resources/styles/Inconsolata.otf");
    _window->setWindowTitle("Interface");

    qInstallMessageHandler(messageHandler);

    _menu->init();
    _window->setMenuBar(_menu);

    unsigned int listenPort = 0; // bind to an ephemeral port by default
    const char** constArgv = const_cast<const char**>(argv);
    const char* portStr = getCmdOption(argc, constArgv, "--listenPort");
    if (portStr) {
        listenPort = atoi(portStr);
    }
    
    // start the nodeThread so its event loop is running
    _nodeThread->start();
    
    // make sure the node thread is given highest priority
    _nodeThread->setPriority(QThread::TimeCriticalPriority);
    
    // put the NodeList and datagram processing on the node thread
    NodeList* nodeList = NodeList::createInstance(NodeType::Agent, listenPort);
    
    nodeList->moveToThread(_nodeThread);
    _datagramProcessor.moveToThread(_nodeThread);
    
    // connect the DataProcessor processDatagrams slot to the QUDPSocket readyRead() signal
    connect(&nodeList->getNodeSocket(), SIGNAL(readyRead()), &_datagramProcessor, SLOT(processDatagrams()));

    // put the audio processing on a separate thread
    QThread* audioThread = new QThread(this);

    _audio.moveToThread(audioThread);
    connect(audioThread, SIGNAL(started()), &_audio, SLOT(start()));

    audioThread->start();

    connect(nodeList, SIGNAL(domainChanged(const QString&)), SLOT(domainChanged(const QString&)));

    connect(nodeList, SIGNAL(nodeKilled(SharedNodePointer)), SLOT(nodeKilled(SharedNodePointer)));
    connect(nodeList, SIGNAL(nodeAdded(SharedNodePointer)), &_voxels, SLOT(nodeAdded(SharedNodePointer)));
    connect(nodeList, SIGNAL(nodeKilled(SharedNodePointer)), &_voxels, SLOT(nodeKilled(SharedNodePointer)));
    
    // read the ApplicationInfo.ini file for Name/Version/Domain information
    QSettings applicationInfo("resources/info/ApplicationInfo.ini", QSettings::IniFormat);

    // set the associated application properties
    applicationInfo.beginGroup("INFO");

    setApplicationName(applicationInfo.value("name").toString());
    setApplicationVersion(BUILD_VERSION);
    setOrganizationName(applicationInfo.value("organizationName").toString());
    setOrganizationDomain(applicationInfo.value("organizationDomain").toString());
    
    qDebug() << "[VERSION] Build sequence: " << qPrintable(applicationVersion());

    _settings = new QSettings(this);

    // Check to see if the user passed in a command line option for loading a local
    // Voxel File.
    _voxelsFilename = getCmdOption(argc, constArgv, "-i");

#ifdef _WIN32
    WSADATA WsaData;
    int wsaresult = WSAStartup(MAKEWORD(2,2), &WsaData);
#endif

    // tell the NodeList instance who to tell the domain server we care about
    nodeList->addSetOfNodeTypesToNodeInterestSet(NodeSet() << NodeType::AudioMixer << NodeType::AvatarMixer
                                                 << NodeType::VoxelServer << NodeType::ParticleServer
                                                 << NodeType::MetavoxelServer);
    
    // connect to the packet sent signal of the _voxelEditSender and the _particleEditSender
    connect(&_voxelEditSender, &VoxelEditPacketSender::packetSent, this, &Application::packetSent);
    connect(&_particleEditSender, &ParticleEditPacketSender::packetSent, this, &Application::packetSent);

    // move the silentNodeTimer to the _nodeThread
    QTimer* silentNodeTimer = new QTimer();
    connect(silentNodeTimer, SIGNAL(timeout()), nodeList, SLOT(removeSilentNodes()));
    silentNodeTimer->moveToThread(_nodeThread);
    silentNodeTimer->start(NODE_SILENCE_THRESHOLD_USECS / 1000);

    QString cachePath = QStandardPaths::writableLocation(QStandardPaths::DataLocation);

    _networkAccessManager = new QNetworkAccessManager(this);
    QNetworkDiskCache* cache = new QNetworkDiskCache(_networkAccessManager);
    cache->setCacheDirectory(!cachePath.isEmpty() ? cachePath : "interfaceCache");
    _networkAccessManager->setCache(cache);

    _window->setCentralWidget(_glWidget);

    restoreSizeAndPosition();

    QFontDatabase fontDatabase; 
    fontDatabase.addApplicationFont("resources/styles/Inconsolata.otf");

    _window->setVisible(true);
    _glWidget->setFocusPolicy(Qt::StrongFocus);
    _glWidget->setFocus();

    // enable mouse tracking; otherwise, we only get drag events
    _glWidget->setMouseTracking(true);

    // initialization continues in initializeGL when OpenGL context is ready

    // Tell our voxel edit sender about our known jurisdictions
    _voxelEditSender.setVoxelServerJurisdictions(&_voxelServerJurisdictions);
    _particleEditSender.setServerJurisdictions(&_particleServerJurisdictions);

    Particle::setVoxelEditPacketSender(&_voxelEditSender);
    Particle::setParticleEditPacketSender(&_particleEditSender);

    // For now we're going to set the PPS for outbound packets to be super high, this is
    // probably not the right long term solution. But for now, we're going to do this to
    // allow you to move a particle around in your hand
    _particleEditSender.setPacketsPerSecond(3000); // super high!!

    // Set the sixense filtering
    _sixenseManager.setFilter(_menu->isOptionChecked(MenuOption::FilterSixense));

    
    checkVersion();

    // do this as late as possible so that all required subsystems are inialized
    loadScripts();
}

Application::~Application() {

    qInstallMessageHandler(NULL);
    
    // make sure we don't call the idle timer any more
    delete idleTimer;
    if(_menu){
	_menu->saveSettings();
    }
    
    _rearMirrorTools->saveSettings(_settings);
    _settings->sync();
    
    // let the avatar mixer know we're out
    MyAvatar::sendKillAvatar();
    
    // ask the datagram processing thread to quit and wait until it is done
    _nodeThread->quit();
    _nodeThread->wait();
    
    // ask the audio thread to quit and wait until it is done
    _audio.thread()->quit();
    _audio.thread()->wait();
    
    _voxelProcessor.terminate();
    _voxelHideShowThread.terminate();
    _voxelEditSender.terminate();
    _particleEditSender.terminate();

    storeSizeAndPosition();
    saveScripts();
    _sharedVoxelSystem.changeTree(new VoxelTree);

    VoxelTreeElement::removeDeleteHook(&_voxels); // we don't need to do this processing on shutdown
    _menu->deleteLater();

    _myAvatar = NULL;
    
    delete _glWidget;
}

void Application::restoreSizeAndPosition() {
    QSettings* settings = new QSettings(this);
    QRect available = desktop()->availableGeometry();

    settings->beginGroup("Window");

    int x = (int)loadSetting(settings, "x", 0);
    int y = (int)loadSetting(settings, "y", 0);
    _window->move(x, y);

    int width = (int)loadSetting(settings, "width", available.width());
    int height = (int)loadSetting(settings, "height", available.height());
    _window->resize(width, height);

    settings->endGroup();
}

void Application::storeSizeAndPosition() {
    QSettings* settings = new QSettings(this);

    settings->beginGroup("Window");

    settings->setValue("width", _window->rect().width());
    settings->setValue("height", _window->rect().height());

    settings->setValue("x", _window->pos().x());
    settings->setValue("y", _window->pos().y());

    settings->endGroup();
}

void Application::initializeGL() {
    qDebug( "Created Display Window.");

    // initialize glut for shape drawing; Qt apparently initializes it on OS X
#ifndef __APPLE__
    int argc = 0;
    glutInit(&argc, 0);
#endif

#ifdef WIN32
    GLenum err = glewInit();
    if (GLEW_OK != err) {
	/* Problem: glewInit failed, something is seriously wrong. */
	qDebug("Error: %s\n", glewGetErrorString(err));
    }
    qDebug("Status: Using GLEW %s\n", glewGetString(GLEW_VERSION));
#endif


    // Before we render anything, let's set up our viewFrustumOffsetCamera with a sufficiently large
    // field of view and near and far clip to make it interesting.
    //viewFrustumOffsetCamera.setFieldOfView(90.0);
    _viewFrustumOffsetCamera.setNearClip(0.1f);
    _viewFrustumOffsetCamera.setFarClip(500.0f * TREE_SCALE);

    initDisplay();
    qDebug( "Initialized Display.");

    init();
    qDebug( "init() complete.");

    // create thread for parsing of voxel data independent of the main network and rendering threads
    _voxelProcessor.initialize(_enableProcessVoxelsThread);
    _voxelEditSender.initialize(_enableProcessVoxelsThread);
    _voxelHideShowThread.initialize(_enableProcessVoxelsThread);
    _particleEditSender.initialize(_enableProcessVoxelsThread);
    if (_enableProcessVoxelsThread) {
        qDebug("Voxel parsing thread created.");
    }

    // call our timer function every second
    QTimer* timer = new QTimer(this);
    connect(timer, SIGNAL(timeout()), SLOT(timer()));
    timer->start(1000);

    // call our idle function whenever we can
    idleTimer = new QTimer(this);
    connect(idleTimer, SIGNAL(timeout()), SLOT(idle()));
    idleTimer->start(0);
    _idleLoopStdev.reset();

    if (_justStarted) {
        float startupTime = (usecTimestampNow() - usecTimestamp(&_applicationStartupTime)) / 1000000.0;
        _justStarted = false;
        qDebug("Startup time: %4.2f seconds.", startupTime);
        const char LOGSTASH_INTERFACE_START_TIME_KEY[] = "interface-start-time";

        // ask the Logstash class to record the startup time
        Logging::stashValue(STAT_TYPE_TIMER, LOGSTASH_INTERFACE_START_TIME_KEY, startupTime);
    }

    // update before the first render
    update(0.0f);

    InfoView::showFirstTime();
}

void Application::paintGL() {
    PerformanceWarning::setSuppressShortTimings(_menu->isOptionChecked(MenuOption::SuppressShortTimings));

    PerformanceWarning warn(_menu->isOptionChecked(MenuOption::PipelineWarnings), "Application::paintGL()");

    glEnable(GL_LINE_SMOOTH);

    if (OculusManager::isConnected()) {
        _myCamera.setUpShift       (0.0f);
        _myCamera.setDistance      (0.0f);
        _myCamera.setTightness     (0.0f);     //  Camera is directly connected to head without smoothing
        _myCamera.setTargetPosition(_myAvatar->getHead().calculateAverageEyePosition());
        _myCamera.setTargetRotation(_myAvatar->getHead().getOrientation());

    } else if (_myCamera.getMode() == CAMERA_MODE_FIRST_PERSON) {
        _myCamera.setTightness(0.0f);  //  In first person, camera follows head exactly without delay
        _myCamera.setTargetPosition(_myAvatar->getHead().calculateAverageEyePosition());
        _myCamera.setTargetRotation(_myAvatar->getHead().getCameraOrientation());

    } else if (_myCamera.getMode() == CAMERA_MODE_THIRD_PERSON) {
        _myCamera.setTightness     (0.0f);     //  Camera is directly connected to head without smoothing
        _myCamera.setTargetPosition(_myAvatar->getUprightHeadPosition());
        _myCamera.setTargetRotation(_myAvatar->getHead().getCameraOrientation());

    } else if (_myCamera.getMode() == CAMERA_MODE_MIRROR) {
        _myCamera.setTightness(0.0f);
        float headHeight = _myAvatar->getHead().calculateAverageEyePosition().y - _myAvatar->getPosition().y;
        _myCamera.setDistance(MIRROR_FULLSCREEN_DISTANCE * _myAvatar->getScale());
        _myCamera.setTargetPosition(_myAvatar->getPosition() + glm::vec3(0, headHeight, 0));
        _myCamera.setTargetRotation(_myAvatar->getWorldAlignedOrientation() * glm::quat(glm::vec3(0.0f, PIf, 0.0f)));
    }

    // Update camera position
    _myCamera.update( 1.f/_fps );


    // Note: whichCamera is used to pick between the normal camera myCamera for our
    // main camera, vs, an alternate camera. The alternate camera we support right now
    // is the viewFrustumOffsetCamera. But theoretically, we could use this same mechanism
    // to add other cameras.
    //
    // Why have two cameras? Well, one reason is that because in the case of the renderViewFrustum()
    // code, we want to keep the state of "myCamera" intact, so we can render what the view frustum of
    // myCamera is. But we also want to do meaningful camera transforms on OpenGL for the offset camera
    Camera whichCamera = _myCamera;
    if (_menu->isOptionChecked(MenuOption::DisplayFrustum)) {

	ViewFrustumOffset viewFrustumOffset = _menu->getViewFrustumOffset();

	// set the camera to third-person view but offset so we can see the frustum
	_viewFrustumOffsetCamera.setTargetPosition(_myCamera.getTargetPosition());
	_viewFrustumOffsetCamera.setTargetRotation(_myCamera.getTargetRotation() * glm::quat(glm::radians(glm::vec3(
														    viewFrustumOffset.pitch, viewFrustumOffset.yaw, viewFrustumOffset.roll))));
	_viewFrustumOffsetCamera.setUpShift(viewFrustumOffset.up);
	_viewFrustumOffsetCamera.setDistance(viewFrustumOffset.distance);
	_viewFrustumOffsetCamera.initialize(); // force immediate snap to ideal position and orientation
	_viewFrustumOffsetCamera.update(1.f/_fps);
	whichCamera = _viewFrustumOffsetCamera;
    }

    if (_menu->isOptionChecked(MenuOption::Shadows)) {
        updateShadowMap();
    }

    if (OculusManager::isConnected()) {
	OculusManager::display(whichCamera);
    } else if (TV3DManager::isConnected()) {
	_glowEffect.prepare();
	TV3DManager::display(whichCamera);
	_glowEffect.render();
    } else {
	_glowEffect.prepare();

	glMatrixMode(GL_MODELVIEW);
	glPushMatrix();
	glLoadIdentity();
	displaySide(whichCamera);
	glPopMatrix();

	_glowEffect.render();

	if (_menu->isOptionChecked(MenuOption::Mirror)) {

	    bool eyeRelativeCamera = false;
	    if (_rearMirrorTools->getZoomLevel() == BODY) {
		_mirrorCamera.setDistance(MIRROR_REARVIEW_BODY_DISTANCE * _myAvatar->getScale());
		_mirrorCamera.setTargetPosition(_myAvatar->getChestPosition());
	    } else { // HEAD zoom level
		_mirrorCamera.setDistance(MIRROR_REARVIEW_DISTANCE * _myAvatar->getScale());
		if (_myAvatar->getSkeletonModel().isActive() && _myAvatar->getHead().getFaceModel().isActive()) {
		    // as a hack until we have a better way of dealing with coordinate precision issues, reposition the
		    // face/body so that the average eye position lies at the origin
		    eyeRelativeCamera = true;
		    _mirrorCamera.setTargetPosition(glm::vec3());

		} else {
		    _mirrorCamera.setTargetPosition(_myAvatar->getHead().calculateAverageEyePosition());
		}
	    }

	    _mirrorCamera.setTargetRotation(_myAvatar->getWorldAlignedOrientation() * glm::quat(glm::vec3(0.0f, PIf, 0.0f)));
	    _mirrorCamera.update(1.0f/_fps);

	    // set the bounds of rear mirror view
	    glViewport(_mirrorViewRect.x(), _glWidget->height() - _mirrorViewRect.y() - _mirrorViewRect.height(),
		       _mirrorViewRect.width(), _mirrorViewRect.height());
	    glScissor(_mirrorViewRect.x(), _glWidget->height() - _mirrorViewRect.y() - _mirrorViewRect.height(),
		      _mirrorViewRect.width(), _mirrorViewRect.height());
	    bool updateViewFrustum = false;
	    updateProjectionMatrix(_mirrorCamera, updateViewFrustum);
	    glEnable(GL_SCISSOR_TEST);
	    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	    // render rear mirror view
	    glPushMatrix();
	    if (eyeRelativeCamera) {
		// save absolute translations
		glm::vec3 absoluteSkeletonTranslation = _myAvatar->getSkeletonModel().getTranslation();
		glm::vec3 absoluteFaceTranslation = _myAvatar->getHead().getFaceModel().getTranslation();

		// get the eye positions relative to the neck and use them to set the face translation
		glm::vec3 leftEyePosition, rightEyePosition;
		_myAvatar->getHead().getFaceModel().setTranslation(glm::vec3());
		_myAvatar->getHead().getFaceModel().getEyePositions(leftEyePosition, rightEyePosition);
		_myAvatar->getHead().getFaceModel().setTranslation((leftEyePosition + rightEyePosition) * -0.5f);

		// get the neck position relative to the body and use it to set the skeleton translation
		glm::vec3 neckPosition;
		_myAvatar->getSkeletonModel().setTranslation(glm::vec3());
		_myAvatar->getSkeletonModel().getNeckPosition(neckPosition);
		_myAvatar->getSkeletonModel().setTranslation(_myAvatar->getHead().getFaceModel().getTranslation() -
							     neckPosition);

		displaySide(_mirrorCamera, true);

		// restore absolute translations
		_myAvatar->getSkeletonModel().setTranslation(absoluteSkeletonTranslation);
		_myAvatar->getHead().getFaceModel().setTranslation(absoluteFaceTranslation);
	    } else {
		displaySide(_mirrorCamera, true);
	    }
	    glPopMatrix();

	    _rearMirrorTools->render(false);

	    // reset Viewport and projection matrix
	    glViewport(0, 0, _glWidget->width(), _glWidget->height());
	    glDisable(GL_SCISSOR_TEST);
	    updateProjectionMatrix(_myCamera, updateViewFrustum);
	} else if (_menu->isOptionChecked(MenuOption::FullscreenMirror)) {
	    _rearMirrorTools->render(true);
	}

	displayOverlay();
    }

    _frameCount++;
}

void Application::resetCamerasOnResizeGL(Camera& camera, int width, int height) {
    if (OculusManager::isConnected()) {
        OculusManager::configureCamera(camera, width, height);
    } else if (TV3DManager::isConnected()) {
        TV3DManager::configureCamera(camera, width, height);
    } else {
        camera.setAspectRatio((float)width / height);
	camera.setFieldOfView(_menu->getFieldOfView());
    }
}

void Application::resizeGL(int width, int height) {
    resetCamerasOnResizeGL(_viewFrustumOffsetCamera, width, height);
    resetCamerasOnResizeGL(_myCamera, width, height);

    glViewport(0, 0, width, height); // shouldn't this account for the menu???

    updateProjectionMatrix();
    glLoadIdentity();
}

void Application::updateProjectionMatrix() {
    updateProjectionMatrix(_myCamera);
}

void Application::updateProjectionMatrix(Camera& camera, bool updateViewFrustum) {
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();

    float left, right, bottom, top, nearVal, farVal;
    glm::vec4 nearClipPlane, farClipPlane;

    // Tell our viewFrustum about this change, using the application camera
    if (updateViewFrustum) {
        loadViewFrustum(camera, _viewFrustum);
        computeOffAxisFrustum(left, right, bottom, top, nearVal, farVal, nearClipPlane, farClipPlane);

        // If we're in Display Frustum mode, then we want to use the slightly adjust near/far clip values of the
        // _viewFrustumOffsetCamera, so that we can see more of the application content in the application's frustum
	if (_menu->isOptionChecked(MenuOption::DisplayFrustum)) {
	    nearVal = _viewFrustumOffsetCamera.getNearClip();
	    farVal = _viewFrustumOffsetCamera.getFarClip();
	}
    } else {
        ViewFrustum tempViewFrustum;
        loadViewFrustum(camera, tempViewFrustum);
        tempViewFrustum.computeOffAxisFrustum(left, right, bottom, top, nearVal, farVal, nearClipPlane, farClipPlane);
    }
    glFrustum(left, right, bottom, top, nearVal, farVal);

    glMatrixMode(GL_MODELVIEW);
}

void Application::resetProfile(const QString& username) {
    // call the destructor on the old profile and construct a new one
    (&_profile)->~Profile();
    new (&_profile) Profile(username);
    updateWindowTitle();
}

void Application::controlledBroadcastToNodes(const QByteArray& packet, const NodeSet& destinationNodeTypes) {
    foreach(NodeType_t type, destinationNodeTypes) {
        // Intercept data to voxel server when voxels are disabled
        if (type == NodeType::VoxelServer && !_menu->isOptionChecked(MenuOption::Voxels)) {
            continue;
        }
        
        // Perform the broadcast for one type
        int nReceivingNodes = NodeList::getInstance()->broadcastToNodes(packet, NodeSet() << type);
        
        // Feed number of bytes to corresponding channel of the bandwidth meter, if any (done otherwise)
        BandwidthMeter::ChannelIndex channel;
        switch (type) {
	case NodeType::Agent:
	case NodeType::AvatarMixer:
	    channel = BandwidthMeter::AVATARS;
	    break;
	case NodeType::VoxelServer:
	    channel = BandwidthMeter::VOXELS;
	    break;
	default:
	    continue;
        }
        _bandwidthMeter.outputStream(channel).updateValue(nReceivingNodes * packet.size());
    }
}

void Application::keyPressEvent(QKeyEvent* event) {

    _controllerScriptingInterface.emitKeyPressEvent(event); // send events to any registered scripts
    
    // if one of our scripts have asked to capture this event, then stop processing it
    if (_controllerScriptingInterface.isKeyCaptured(event)) {
        return;
    }

    if (activeWindow() == _window) {
        if (_chatEntryOn) {
            if (_chatEntry.keyPressEvent(event)) {
                _myAvatar->setKeyState(event->key() == Qt::Key_Backspace || event->key() == Qt::Key_Delete ?
				       DELETE_KEY_DOWN : INSERT_KEY_DOWN);
                _myAvatar->setChatMessage(string(_chatEntry.getContents().size(), SOLID_BLOCK_CHAR));

            } else {
                _myAvatar->setChatMessage(_chatEntry.getContents());
                _chatEntry.clear();
                _chatEntryOn = false;
                setMenuShortcutsEnabled(true);
            }
            return;
        }

        bool isShifted = event->modifiers().testFlag(Qt::ShiftModifier);
        bool isMeta = event->modifiers().testFlag(Qt::ControlModifier);
        switch (event->key()) {
	    break;
	case Qt::Key_Shift:
	    if (_menu->isOptionChecked(MenuOption::VoxelSelectMode)) {
		_pasteMode = true;
	    }
	    break;
	case Qt::Key_BracketLeft:
	case Qt::Key_BracketRight:
	case Qt::Key_BraceLeft:
	case Qt::Key_BraceRight:
	case Qt::Key_ParenLeft:
	case Qt::Key_ParenRight:
	case Qt::Key_Less:
	case Qt::Key_Greater:
	case Qt::Key_Comma:
	case Qt::Key_Period:
	    _menu->handleViewFrustumOffsetKeyModifier(event->key());
	    break;
	case Qt::Key_Apostrophe:
	    _audioScope.inputPaused = !_audioScope.inputPaused;
	    break;
	case Qt::Key_L:
	    if (isShifted) {
		_menu->triggerOption(MenuOption::LodTools);
	    } else if (isMeta) {
		_menu->triggerOption(MenuOption::Log);
	    }
	    break;

	case Qt::Key_E:
	    if (_nudgeStarted) {
		_nudgeGuidePosition.y += _mouseVoxel.s;
	    } else {
		if (!_myAvatar->getDriveKeys(UP)) {
		    _myAvatar->jump();
		}
		_myAvatar->setDriveKeys(UP, 1);
	    }
	    break;

	case Qt::Key_Asterisk:
	    _menu->triggerOption(MenuOption::Stars);
	    break;

	case Qt::Key_C:
	    if (_nudgeStarted) {
		_nudgeGuidePosition.y -= _mouseVoxel.s;
	    } else {
		_myAvatar->setDriveKeys(DOWN, 1);
	    }
	    break;

	case Qt::Key_W:
	    if (_nudgeStarted) {
		if (_lookingAlongX) {
		    if (_lookingAwayFromOrigin) {
			_nudgeGuidePosition.x += _mouseVoxel.s;
		    } else {
			_nudgeGuidePosition.x -= _mouseVoxel.s;
		    }
		} else {
		    if (_lookingAwayFromOrigin) {
			_nudgeGuidePosition.z += _mouseVoxel.s;
		    } else {
			_nudgeGuidePosition.z -= _mouseVoxel.s;
		    }
		}
	    } else {
		_myAvatar->setDriveKeys(FWD, 1);
	    }
	    break;

	case Qt::Key_S:
	    if (isShifted && !isMeta)  {
		_voxels.collectStatsForTreesAndVBOs();
	    } else if (isShifted && isMeta)  {
		_menu->triggerOption(MenuOption::SuppressShortTimings);
	    } else if (!isShifted && isMeta)  {
		takeSnapshot();
	    } else if (_nudgeStarted) {
		if (_lookingAlongX) {
		    if (_lookingAwayFromOrigin) {
			_nudgeGuidePosition.x -= _mouseVoxel.s;
		    } else {
			_nudgeGuidePosition.x += _mouseVoxel.s;
		    }
		} else {
		    if (_lookingAwayFromOrigin) {
			_nudgeGuidePosition.z -= _mouseVoxel.s;
		    } else {
			_nudgeGuidePosition.z += _mouseVoxel.s;
		    }
		}
	    } else {
		_myAvatar->setDriveKeys(BACK, 1);
	    }
	    break;

	case Qt::Key_Space:
	    resetSensors();
	    break;

	case Qt::Key_G:
	    if (isShifted) {
		_menu->triggerOption(MenuOption::Gravity);
	    } else {
		_menu->triggerOption(MenuOption::VoxelGetColorMode);
	    }
	    break;

	case Qt::Key_A:
	    if (isShifted) {
		_menu->triggerOption(MenuOption::Atmosphere);
	    } else if (_nudgeStarted) {
		if (_lookingAlongX) {
		    if (_lookingAwayFromOrigin) {
			_nudgeGuidePosition.z -= _mouseVoxel.s;
		    } else {
			_nudgeGuidePosition.z += _mouseVoxel.s;
		    }
		} else {
		    if (_lookingAwayFromOrigin) {
			_nudgeGuidePosition.x += _mouseVoxel.s;
		    } else {
			_nudgeGuidePosition.x -= _mouseVoxel.s;
		    }
		}
	    } else {
		_myAvatar->setDriveKeys(ROT_LEFT, 1);
	    }
	    break;

	case Qt::Key_D:
	    if (_nudgeStarted) {
		if (_lookingAlongX) {
		    if (_lookingAwayFromOrigin) {
			_nudgeGuidePosition.z += _mouseVoxel.s;
		    } else {
			_nudgeGuidePosition.z -= _mouseVoxel.s;
		    }
		} else {
		    if (_lookingAwayFromOrigin) {
			_nudgeGuidePosition.x -= _mouseVoxel.s;
		    } else {
			_nudgeGuidePosition.x += _mouseVoxel.s;
		    }
		}
	    } else {
		_myAvatar->setDriveKeys(ROT_RIGHT, 1);
	    }
	    break;

	case Qt::Key_Return:
	case Qt::Key_Enter:
	    if (_nudgeStarted) {
		nudgeVoxels();
	    } else {
		_chatEntryOn = true;
		_myAvatar->setKeyState(NO_KEY_DOWN);
		_myAvatar->setChatMessage(string());
		setMenuShortcutsEnabled(false);
	    }
	    break;

	case Qt::Key_Up:
	    if (_nudgeStarted && !isShifted) {
		if (_lookingAlongX) {
		    if (_lookingAwayFromOrigin) {
			_nudgeGuidePosition.x += _mouseVoxel.s;
		    } else {
			_nudgeGuidePosition.x -= _mouseVoxel.s;
		    }
		} else {
		    if (_lookingAwayFromOrigin) {
			_nudgeGuidePosition.z += _mouseVoxel.s;
		    } else {
			_nudgeGuidePosition.z -= _mouseVoxel.s;
		    }
		}
	    } else if (_nudgeStarted && isShifted) {
		_nudgeGuidePosition.y += _mouseVoxel.s;
	    } else {
		_myAvatar->setDriveKeys(isShifted ? UP : FWD, 1);
	    }
	    break;

	case Qt::Key_Down:
	    if (_nudgeStarted && !isShifted) {
		if (_lookingAlongX) {
		    if (_lookingAwayFromOrigin) {
			_nudgeGuidePosition.x -= _mouseVoxel.s;
		    } else {
			_nudgeGuidePosition.x += _mouseVoxel.s;
		    }
		} else {
		    if (_lookingAwayFromOrigin) {
			_nudgeGuidePosition.z -= _mouseVoxel.s;
		    } else {
			_nudgeGuidePosition.z += _mouseVoxel.s;
		    }
		}
	    } else if (_nudgeStarted && isShifted) {
		_nudgeGuidePosition.y -= _mouseVoxel.s;
	    } else {
		_myAvatar->setDriveKeys(isShifted ? DOWN : BACK, 1);
	    }
	    break;

	case Qt::Key_Left:
	    if (_nudgeStarted) {
		if (_lookingAlongX) {
		    if (_lookingAwayFromOrigin) {
			_nudgeGuidePosition.z -= _mouseVoxel.s;
		    } else {
			_nudgeGuidePosition.z += _mouseVoxel.s;
		    }
		} else {
		    if (_lookingAwayFromOrigin) {
			_nudgeGuidePosition.x += _mouseVoxel.s;
		    } else {
			_nudgeGuidePosition.x -= _mouseVoxel.s;
		    }
		}
	    } else {
		_myAvatar->setDriveKeys(isShifted ? LEFT : ROT_LEFT, 1);
	    }
	    break;

	case Qt::Key_Right:
	    if (_nudgeStarted) {
		if (_lookingAlongX) {
		    if (_lookingAwayFromOrigin) {
			_nudgeGuidePosition.z += _mouseVoxel.s;
		    } else {
			_nudgeGuidePosition.z -= _mouseVoxel.s;
		    }
		} else {
		    if (_lookingAwayFromOrigin) {
			_nudgeGuidePosition.x -= _mouseVoxel.s;
		    } else {
			_nudgeGuidePosition.x += _mouseVoxel.s;
		    }
		}
	    } else {
		_myAvatar->setDriveKeys(isShifted ? RIGHT : ROT_RIGHT, 1);
	    }
	    break;

	case Qt::Key_I:
	    if (isShifted) {
		_myCamera.setEyeOffsetOrientation(glm::normalize(
								 glm::quat(glm::vec3(0.002f, 0, 0)) * _myCamera.getEyeOffsetOrientation()));
	    } else {
		_myCamera.setEyeOffsetPosition(_myCamera.getEyeOffsetPosition() + glm::vec3(0, 0.001, 0));
	    }
	    updateProjectionMatrix();
	    break;

	case Qt::Key_K:
	    if (isShifted) {
		_myCamera.setEyeOffsetOrientation(glm::normalize(
								 glm::quat(glm::vec3(-0.002f, 0, 0)) * _myCamera.getEyeOffsetOrientation()));
	    } else {
		_myCamera.setEyeOffsetPosition(_myCamera.getEyeOffsetPosition() + glm::vec3(0, -0.001, 0));
	    }
	    updateProjectionMatrix();
	    break;

	case Qt::Key_J:
	    if (isShifted) {
		_viewFrustum.setFocalLength(_viewFrustum.getFocalLength() - 0.1f);
		if (TV3DManager::isConnected()) {
		    TV3DManager::configureCamera(_myCamera, _glWidget->width(),_glWidget->height());
		}
	    } else {
		_myCamera.setEyeOffsetPosition(_myCamera.getEyeOffsetPosition() + glm::vec3(-0.001, 0, 0));
	    }
	    updateProjectionMatrix();
	    break;

	case Qt::Key_M:
	    if (isShifted) {
		_viewFrustum.setFocalLength(_viewFrustum.getFocalLength() + 0.1f);
		if (TV3DManager::isConnected()) {
		    TV3DManager::configureCamera(_myCamera, _glWidget->width(),_glWidget->height());
		}

	    } else {
		_myCamera.setEyeOffsetPosition(_myCamera.getEyeOffsetPosition() + glm::vec3(0.001, 0, 0));
	    }
	    updateProjectionMatrix();
	    break;

	case Qt::Key_U:
	    if (isShifted) {
		_myCamera.setEyeOffsetOrientation(glm::normalize(
								 glm::quat(glm::vec3(0, 0, -0.002f)) * _myCamera.getEyeOffsetOrientation()));
	    } else {
		_myCamera.setEyeOffsetPosition(_myCamera.getEyeOffsetPosition() + glm::vec3(0, 0, -0.001));
	    }
	    updateProjectionMatrix();
	    break;

	case Qt::Key_Y:
	    if (isShifted) {
		_myCamera.setEyeOffsetOrientation(glm::normalize(
								 glm::quat(glm::vec3(0, 0, 0.002f)) * _myCamera.getEyeOffsetOrientation()));
	    } else {
		_myCamera.setEyeOffsetPosition(_myCamera.getEyeOffsetPosition() + glm::vec3(0, 0, 0.001));
	    }
	    updateProjectionMatrix();
	    break;
	case Qt::Key_H:
	    if (isShifted) {
		_menu->triggerOption(MenuOption::Mirror);
	    } else {
		_menu->triggerOption(MenuOption::FullscreenMirror);
	    }
	    break;
	case Qt::Key_F:
	    if (isShifted)  {
		_menu->triggerOption(MenuOption::DisplayFrustum);
	    }
	    break;
	case Qt::Key_V:
	    if (isShifted) {
		_menu->triggerOption(MenuOption::Voxels);
	    } else {
		_menu->triggerOption(MenuOption::VoxelAddMode);
		_nudgeStarted = false;
	    }
	    break;
	case Qt::Key_P:
	    _menu->triggerOption(MenuOption::FirstPerson);
	    break;
	case Qt::Key_R:
	    if (isShifted)  {
		_menu->triggerOption(MenuOption::FrustumRenderMode);
	    } else {
		_menu->triggerOption(MenuOption::VoxelDeleteMode);
		_nudgeStarted = false;
	    }
	    break;
	case Qt::Key_B:
	    _menu->triggerOption(MenuOption::VoxelColorMode);
	    _nudgeStarted = false;
	    break;
	case Qt::Key_O:
	    _menu->triggerOption(MenuOption::VoxelSelectMode);
	    _nudgeStarted = false;
	    break;
	case Qt::Key_Slash:
	    _menu->triggerOption(MenuOption::Stats);
	    break;
	case Qt::Key_Backspace:
	case Qt::Key_Delete:
	    if (_menu->isOptionChecked(MenuOption::VoxelDeleteMode) ||
		_menu->isOptionChecked(MenuOption::VoxelSelectMode)) {
		deleteVoxelUnderCursor();
	    }
	    break;
	case Qt::Key_Plus:
	    _myAvatar->increaseSize();
	    break;
	case Qt::Key_Minus:
	    _myAvatar->decreaseSize();
	    break;
	case Qt::Key_Equal:
	    _myAvatar->resetSize();
	    break;

	case Qt::Key_1:
	case Qt::Key_2:
	case Qt::Key_3:
	case Qt::Key_4:
	case Qt::Key_5:
	case Qt::Key_6:
	case Qt::Key_7:
	case Qt::Key_8:
	    _swatch.handleEvent(event->key(), _menu->isOptionChecked(MenuOption::VoxelGetColorMode));
	    break;
	case Qt::Key_At:
	    _menu->goTo();
	    break;
	default:
	    event->ignore();
	    break;
        }
    }
}

void Application::keyReleaseEvent(QKeyEvent* event) {

    _controllerScriptingInterface.emitKeyReleaseEvent(event); // send events to any registered scripts

    // if one of our scripts have asked to capture this event, then stop processing it
    if (_controllerScriptingInterface.isKeyCaptured(event)) {
        return;
    }


    if (activeWindow() == _window) {
        if (_chatEntryOn) {
            _myAvatar->setKeyState(NO_KEY_DOWN);
            return;
        }

        switch (event->key()) {
	case Qt::Key_Shift:
	    _pasteMode = false;
	    break;
	case Qt::Key_E:
	    _myAvatar->setDriveKeys(UP, 0);
	    break;

	case Qt::Key_C:
	    _myAvatar->setDriveKeys(DOWN, 0);
	    break;

	case Qt::Key_W:
	    _myAvatar->setDriveKeys(FWD, 0);
	    break;

	case Qt::Key_S:
	    _myAvatar->setDriveKeys(BACK, 0);
	    break;

	case Qt::Key_A:
	    _myAvatar->setDriveKeys(ROT_LEFT, 0);
	    break;

	case Qt::Key_D:
	    _myAvatar->setDriveKeys(ROT_RIGHT, 0);
	    break;

	case Qt::Key_Up:
	    _myAvatar->setDriveKeys(FWD, 0);
	    _myAvatar->setDriveKeys(UP, 0);
	    break;

	case Qt::Key_Down:
	    _myAvatar->setDriveKeys(BACK, 0);
	    _myAvatar->setDriveKeys(DOWN, 0);
	    break;

	case Qt::Key_Left:
	    _myAvatar->setDriveKeys(LEFT, 0);
	    _myAvatar->setDriveKeys(ROT_LEFT, 0);
	    break;

	case Qt::Key_Right:
	    _myAvatar->setDriveKeys(RIGHT, 0);
	    _myAvatar->setDriveKeys(ROT_RIGHT, 0);
	    break;

	default:
	    event->ignore();
	    break;
        }
    }
}

void Application::mouseMoveEvent(QMouseEvent* event) {
    _controllerScriptingInterface.emitMouseMoveEvent(event); // send events to any registered scripts

    // if one of our scripts have asked to capture this event, then stop processing it
    if (_controllerScriptingInterface.isMouseCaptured()) {
        return;
    }


    _lastMouseMove = usecTimestampNow();
    if (_mouseHidden) {
        getGLWidget()->setCursor(Qt::ArrowCursor);
        _mouseHidden = false;
        _seenMouseMove = true;
    }

    int deltaX = event->x() - _mouseX;
    int deltaY = event->y() - _mouseY;
    _mouseX = event->x();
    _mouseY = event->y();
        
    if (activeWindow() == _window) {
        // orbit behavior
	if (_mousePressed && !_menu->isVoxelModeActionChecked()) {
	    if (_myAvatar->getLookAtTargetAvatar()) {
		_myAvatar->orbit(_myAvatar->getLookAtTargetAvatar()->getPosition(), deltaX, deltaY);
		return;
	    }
	    if (_isHoverVoxel) {
		_myAvatar->orbit(getMouseVoxelWorldCoordinates(_hoverVoxel), deltaX, deltaY);
		return;
	    }
	}
        // detect drag
        glm::vec3 mouseVoxelPos(_mouseVoxel.x, _mouseVoxel.y, _mouseVoxel.z);
        if (!_justEditedVoxel && mouseVoxelPos != _lastMouseVoxelPos) {
	    if (event->buttons().testFlag(Qt::LeftButton)) {
		maybeEditVoxelUnderCursor();
		
	    } else if (event->buttons().testFlag(Qt::RightButton) && _menu->isVoxelModeActionChecked()) {
		deleteVoxelUnderCursor();
	    }
        }

        _pieMenu.mouseMoveEvent(_mouseX, _mouseY);
    }
}

const bool MAKE_SOUND_ON_VOXEL_HOVER = false;
const bool MAKE_SOUND_ON_VOXEL_CLICK = true;
const float HOVER_VOXEL_FREQUENCY = 7040.f;
const float HOVER_VOXEL_DECAY = 0.999f;

void Application::mousePressEvent(QMouseEvent* event) {
    _controllerScriptingInterface.emitMousePressEvent(event); // send events to any registered scripts

    // if one of our scripts have asked to capture this event, then stop processing it
    if (_controllerScriptingInterface.isMouseCaptured()) {
        return;
    }


    if (activeWindow() == _window) {
        if (event->button() == Qt::LeftButton) {
            _mouseX = event->x();
            _mouseY = event->y();
            _mouseDragStartedX = _mouseX;
            _mouseDragStartedY = _mouseY;
            _mouseVoxelDragging = _mouseVoxel;
            _mousePressed = true;

            maybeEditVoxelUnderCursor();

            if (_audio.mousePressEvent(_mouseX, _mouseY)) {
                // stop propagation
                return;
            }

            if (_rearMirrorTools->mousePressEvent(_mouseX, _mouseY)) {
                // stop propagation
                return;
            }

            if (!_palette.isActive() && (!_isHoverVoxel || _myAvatar->getLookAtTargetAvatar())) {
                // disable for now
                // _pieMenu.mousePressEvent(_mouseX, _mouseY);
            }
	    if (_menu->isOptionChecked(MenuOption::VoxelSelectMode) && _pasteMode) {
		pasteVoxels();
	    }

	    if (_menu->isOptionChecked(MenuOption::VoxelDeleteMode) && 
		MAKE_SOUND_ON_VOXEL_CLICK && _isHoverVoxel && !_isHoverVoxelSounding) {
		_hoverVoxelOriginalColor[0] = _hoverVoxel.red;
		_hoverVoxelOriginalColor[1] = _hoverVoxel.green;
		_hoverVoxelOriginalColor[2] = _hoverVoxel.blue;
		_hoverVoxelOriginalColor[3] = 1;
		const float RED_CLICK_FREQUENCY = 1000.f;
		const float GREEN_CLICK_FREQUENCY = 1250.f;
		const float BLUE_CLICK_FREQUENCY = 1330.f;
		const float MIDDLE_A_FREQUENCY = 440.f;
		float frequency = MIDDLE_A_FREQUENCY +
		    (_hoverVoxel.red / 255.f * RED_CLICK_FREQUENCY +
		     _hoverVoxel.green / 255.f * GREEN_CLICK_FREQUENCY +
		     _hoverVoxel.blue / 255.f * BLUE_CLICK_FREQUENCY) / 3.f;

		_audio.startCollisionSound(1.0, frequency, 0.0, HOVER_VOXEL_DECAY, false);
		_isHoverVoxelSounding = true;

		const float PERCENTAGE_TO_MOVE_TOWARD = 0.90f;
		glm::vec3 newTarget = getMouseVoxelWorldCoordinates(_hoverVoxel);
		glm::vec3 myPosition = _myAvatar->getPosition();

		// If there is not an action tool set (add, delete, color), move to this voxel
		if (_menu->isOptionChecked(MenuOption::ClickToFly) &&
		    !(_menu->isOptionChecked(MenuOption::VoxelAddMode) ||
		      _menu->isOptionChecked(MenuOption::VoxelDeleteMode) ||
		      _menu->isOptionChecked(MenuOption::VoxelColorMode))) {
		    _myAvatar->setMoveTarget(myPosition + (newTarget - myPosition) * PERCENTAGE_TO_MOVE_TOWARD);

		}

	    } else if (event->button() == Qt::RightButton && _menu->isVoxelModeActionChecked()) {
		deleteVoxelUnderCursor();

	    }
	}

    }
}


void Application::mouseReleaseEvent(QMouseEvent* event) {
    _controllerScriptingInterface.emitMouseReleaseEvent(event); // send events to any registered scripts

    // if one of our scripts have asked to capture this event, then stop processing it
    if (_controllerScriptingInterface.isMouseCaptured()) {
        return;
    }

    if (activeWindow() == _window) {
        if (event->button() == Qt::LeftButton) {
            _mouseX = event->x();
            _mouseY = event->y();
            _mousePressed = false;
            checkBandwidthMeterClick();
	    if (_menu->isOptionChecked(MenuOption::Stats)) {
                checkStatsClick();
	    }            
            _pieMenu.mouseReleaseEvent(_mouseX, _mouseY);
        }
    }
}

void Application::touchUpdateEvent(QTouchEvent* event) {
    _controllerScriptingInterface.emitTouchUpdateEvent(event); // send events to any registered scripts

    // if one of our scripts have asked to capture this event, then stop processing it
    if (_controllerScriptingInterface.isTouchCaptured()) {
        return;
    }

    bool validTouch = false;
    if (activeWindow() == _window) {
        const QList<QTouchEvent::TouchPoint>& tPoints = event->touchPoints();
        _touchAvgX = 0.0f;
        _touchAvgY = 0.0f;
        int numTouches = tPoints.count();
        if (numTouches > 1) {
            for (int i = 0; i < numTouches; ++i) {
                _touchAvgX += tPoints[i].pos().x();
                _touchAvgY += tPoints[i].pos().y();
            }
            _touchAvgX /= (float)(numTouches);
            _touchAvgY /= (float)(numTouches);
            validTouch = true;
        }
    }
    if (!_isTouchPressed) {
        _touchDragStartedAvgX = _touchAvgX;
        _touchDragStartedAvgY = _touchAvgY;
    }
    _isTouchPressed = validTouch;
}

void Application::touchBeginEvent(QTouchEvent* event) {
    _controllerScriptingInterface.emitTouchBeginEvent(event); // send events to any registered scripts

    touchUpdateEvent(event);

    // if one of our scripts have asked to capture this event, then stop processing it
    if (_controllerScriptingInterface.isTouchCaptured()) {
        return;
    }
    
    // put any application specific touch behavior below here..
    _lastTouchAvgX = _touchAvgX;
    _lastTouchAvgY = _touchAvgY;

}

void Application::touchEndEvent(QTouchEvent* event) {
    _controllerScriptingInterface.emitTouchEndEvent(event); // send events to any registered scripts

    // if one of our scripts have asked to capture this event, then stop processing it
    if (_controllerScriptingInterface.isTouchCaptured()) {
        return;
    }

    // put any application specific touch behavior below here..
    _touchDragStartedAvgX = _touchAvgX;
    _touchDragStartedAvgY = _touchAvgY;
    _isTouchPressed = false;

}

const bool USE_MOUSEWHEEL = false;
void Application::wheelEvent(QWheelEvent* event) {

    _controllerScriptingInterface.emitWheelEvent(event); // send events to any registered scripts

    // if one of our scripts have asked to capture this event, then stop processing it
    if (_controllerScriptingInterface.isWheelCaptured()) {
        return;
    }
    
    //  Wheel Events disabled for now because they are also activated by touch look pitch up/down.
    if (USE_MOUSEWHEEL && (activeWindow() == _window)) {
        if (!_menu->isVoxelModeActionChecked()) {
            event->ignore();
            return;
        }
        if (event->delta() > 0) {
            increaseVoxelSize();
        } else {
            decreaseVoxelSize();
        }
    }
}

void Application::sendPingPackets() {
    QByteArray pingPacket = NodeList::getInstance()->constructPingPacket();
    controlledBroadcastToNodes(pingPacket, NodeSet() << NodeType::VoxelServer
                               << NodeType::ParticleServer
                               << NodeType::AudioMixer << NodeType::AvatarMixer
                               << NodeType::MetavoxelServer);
}

//  Every second, check the frame rates and other stuff
void Application::timer() {
    gettimeofday(&_timerEnd, NULL);
    
    if (_menu->isOptionChecked(MenuOption::TestPing)) {
        sendPingPackets();
    }

    _fps = (float)_frameCount / ((float)diffclock(&_timerStart, &_timerEnd) / 1000.f);
    
    _packetsPerSecond = (float) _datagramProcessor.getPacketCount() / ((float)diffclock(&_timerStart, &_timerEnd) / 1000.f);
    _bytesPerSecond = (float) _datagramProcessor.getByteCount() / ((float)diffclock(&_timerStart, &_timerEnd) / 1000.f);
    _frameCount = 0;
    
    _datagramProcessor.resetCounters();

    gettimeofday(&_timerStart, NULL);

    // ask the node list to check in with the domain server
    NodeList::getInstance()->sendDomainServerCheckIn();
    
    // send unmatched DataServerClient packets
    DataServerClient::resendUnmatchedPackets();

    // give the MyAvatar object position, orientation to the Profile so it can propagate to the data-server
    if(_myAvatar){
	_profile.updatePosition(_myAvatar->getPosition());
	_profile.updateOrientation(_myAvatar->getOrientation());
    }
}

static glm::vec3 getFaceVector(BoxFace face) {
    switch (face) {
    case MIN_X_FACE:
	return glm::vec3(-1, 0, 0);

    case MAX_X_FACE:
	return glm::vec3(1, 0, 0);

    case MIN_Y_FACE:
	return glm::vec3(0, -1, 0);

    case MAX_Y_FACE:
	return glm::vec3(0, 1, 0);

    case MIN_Z_FACE:
	return glm::vec3(0, 0, -1);

    default: // quiet windows warnings
    case MAX_Z_FACE:
	return glm::vec3(0, 0, 1);
    }
}

void Application::idle() {
    // Normally we check PipelineWarnings, but since idle will often take more than 10ms we only show these idle timing
    // details if we're in ExtraDebugging mode. However, the ::update() and it's subcomponents will show their timing
    // details normally.
    bool showWarnings = getLogger()->extraDebugging();
    PerformanceWarning warn(showWarnings, "Application::idle()");

    timeval check;
    gettimeofday(&check, NULL);

    //  Only run simulation code if more than IDLE_SIMULATE_MSECS have passed since last time we ran

    double timeSinceLastUpdate = diffclock(&_lastTimeUpdated, &check);
    if (timeSinceLastUpdate > IDLE_SIMULATE_MSECS) {
        {
            PerformanceWarning warn(showWarnings, "Application::idle()... update()");
            const float BIGGEST_DELTA_TIME_SECS = 0.25f;
            update(glm::clamp((float)timeSinceLastUpdate / 1000.f, 0.f, BIGGEST_DELTA_TIME_SECS));
        }
        {
            PerformanceWarning warn(showWarnings, "Application::idle()... updateGL()");
            _glWidget->updateGL();
        }
        {
            PerformanceWarning warn(showWarnings, "Application::idle()... rest of it");
            _lastTimeUpdated = check;
            _idleLoopStdev.addValue(timeSinceLastUpdate);

            //  Record standard deviation and reset counter if needed
            const int STDEV_SAMPLES = 500;
            if (_idleLoopStdev.getSamples() > STDEV_SAMPLES) {
                _idleLoopMeasuredJitter = _idleLoopStdev.getStDev();
                _idleLoopStdev.reset();
            }

            // After finishing all of the above work, restart the idle timer, allowing 2ms to process events.
            idleTimer->start(2);
        }
    }
}

void Application::checkBandwidthMeterClick() {
    // ... to be called upon button release
    if (_menu->isOptionChecked(MenuOption::Bandwidth) &&
        glm::compMax(glm::abs(glm::ivec2(_mouseX - _mouseDragStartedX, _mouseY - _mouseDragStartedY)))
	<= BANDWIDTH_METER_CLICK_MAX_DRAG_LENGTH
	&& _bandwidthMeter.isWithinArea(_mouseX, _mouseY, _glWidget->width(), _glWidget->height())) {

        // The bandwidth meter is visible, the click didn't get dragged too far and
        // we actually hit the bandwidth meter
        _menu->bandwidthDetails();
    }
}

void Application::setFullscreen(bool fullscreen) {
    _window->setWindowState(fullscreen ? (_window->windowState() | Qt::WindowFullScreen) :
			    (_window->windowState() & ~Qt::WindowFullScreen));
}

void Application::setEnable3DTVMode(bool enable3DTVMode) {
    resizeGL(_glWidget->width(),_glWidget->height());
}


void Application::setRenderVoxels(bool voxelRender) {
    _voxelEditSender.setShouldSend(voxelRender);
    if (!voxelRender) {
        doKillLocalVoxels();
    }
}

void Application::doKillLocalVoxels() {
    _wantToKillLocalVoxels = true;
}

void Application::removeVoxel(glm::vec3 position,
                              float scale) {
    VoxelDetail voxel;
    voxel.x = position.x / TREE_SCALE;
    voxel.y = position.y / TREE_SCALE;
    voxel.z = position.z / TREE_SCALE;
    voxel.s = scale / TREE_SCALE;
    _voxelEditSender.sendVoxelEditMessage(PacketTypeVoxelErase, voxel);

    // delete it locally to see the effect immediately (and in case no voxel server is present)
    _voxels.deleteVoxelAt(voxel.x, voxel.y, voxel.z, voxel.s);
}


void Application::makeVoxel(glm::vec3 position,
                            float scale,
                            unsigned char red,
                            unsigned char green,
                            unsigned char blue,
                            bool isDestructive) {
    VoxelDetail voxel;
    voxel.x = position.x / TREE_SCALE;
    voxel.y = position.y / TREE_SCALE;
    voxel.z = position.z / TREE_SCALE;
    voxel.s = scale / TREE_SCALE;
    voxel.red = red;
    voxel.green = green;
    voxel.blue = blue;
    PacketType message = isDestructive ? PacketTypeVoxelSetDestructive : PacketTypeVoxelSet;
    _voxelEditSender.sendVoxelEditMessage(message, voxel);

    // create the voxel locally so it appears immediately

    _voxels.createVoxel(voxel.x, voxel.y, voxel.z, voxel.s,
                        voxel.red, voxel.green, voxel.blue,
                        isDestructive);
}

glm::vec3 Application::getMouseVoxelWorldCoordinates(const VoxelDetail& mouseVoxel) {
    return glm::vec3((mouseVoxel.x + mouseVoxel.s / 2.f) * TREE_SCALE, (mouseVoxel.y + mouseVoxel.s / 2.f) * TREE_SCALE,
		     (mouseVoxel.z + mouseVoxel.s / 2.f) * TREE_SCALE);
}

const float NUDGE_PRECISION_MIN = 1 / pow(2.0, 12.0);

void Application::decreaseVoxelSize() {
    if (_nudgeStarted) {
        if (_mouseVoxelScale >= NUDGE_PRECISION_MIN) {
            _mouseVoxelScale /= 2;
        }
    } else {
        _mouseVoxelScale /= 2;
    }
}

void Application::increaseVoxelSize() {
    if (_nudgeStarted) {
        if (_mouseVoxelScale < _nudgeVoxel.s) {
            _mouseVoxelScale *= 2;
        }
    } else {
        _mouseVoxelScale *= 2;
    }
}

const int MAXIMUM_EDIT_VOXEL_MESSAGE_SIZE = 1500;
struct SendVoxelsOperationArgs {
    const unsigned char*  newBaseOctCode;
};

bool Application::sendVoxelsOperation(OctreeElement* element, void* extraData) {
    VoxelTreeElement* voxel = (VoxelTreeElement*)element;
    SendVoxelsOperationArgs* args = (SendVoxelsOperationArgs*)extraData;
    if (voxel->isColored()) {
        const unsigned char* nodeOctalCode = voxel->getOctalCode();
        unsigned char* codeColorBuffer = NULL;
        int codeLength  = 0;
        int bytesInCode = 0;
        int codeAndColorLength;

        // If the newBase is NULL, then don't rebase
        if (args->newBaseOctCode) {
            codeColorBuffer = rebaseOctalCode(nodeOctalCode, args->newBaseOctCode, true);
            codeLength  = numberOfThreeBitSectionsInCode(codeColorBuffer);
            bytesInCode = bytesRequiredForCodeLength(codeLength);
            codeAndColorLength = bytesInCode + SIZE_OF_COLOR_DATA;
        } else {
            codeLength  = numberOfThreeBitSectionsInCode(nodeOctalCode);
            bytesInCode = bytesRequiredForCodeLength(codeLength);
            codeAndColorLength = bytesInCode + SIZE_OF_COLOR_DATA;
            codeColorBuffer = new unsigned char[codeAndColorLength];
            memcpy(codeColorBuffer, nodeOctalCode, bytesInCode);
        }

        // copy the colors over
        codeColorBuffer[bytesInCode + RED_INDEX] = voxel->getColor()[RED_INDEX];
        codeColorBuffer[bytesInCode + GREEN_INDEX] = voxel->getColor()[GREEN_INDEX];
        codeColorBuffer[bytesInCode + BLUE_INDEX] = voxel->getColor()[BLUE_INDEX];
        getInstance()->_voxelEditSender.queueVoxelEditMessage(PacketTypeVoxelSetDestructive,
							      codeColorBuffer, codeAndColorLength);

        delete[] codeColorBuffer;
    }
    return true; // keep going
}

void Application::exportVoxels() {
    QString desktopLocation = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
    QString suggestedName = desktopLocation.append("/voxels.svo");

    QString fileNameString = QFileDialog::getSaveFileName(_glWidget, tr("Export Voxels"), suggestedName,
                                                          tr("Sparse Voxel Octree Files (*.svo)"));
    QByteArray fileNameAscii = fileNameString.toLocal8Bit();
    const char* fileName = fileNameAscii.data();
    VoxelTreeElement* selectedNode = _voxels.getVoxelAt(_mouseVoxel.x, _mouseVoxel.y, _mouseVoxel.z, _mouseVoxel.s);
    if (selectedNode) {
        VoxelTree exportTree;
        _voxels.copySubTreeIntoNewTree(selectedNode, &exportTree, true);
        exportTree.writeToSVOFile(fileName);
    }

    // restore the main window's active state
    _window->activateWindow();
}

void Application::importVoxels() {
    if (!_voxelImporter) {
        _voxelImporter = new VoxelImporter(_window);
        _voxelImporter->init(_settings);
    }
    
    if (_voxelImporter->exec()) {
        qDebug("[DEBUG] Import succeeded.");
    } else {
        qDebug("[DEBUG] Import failed.");
    }

    // restore the main window's active state
    _window->activateWindow();
}

void Application::cutVoxels() {
    copyVoxels();
    deleteVoxelUnderCursor();
}

void Application::copyVoxels() {
    // switch to and clear the clipboard first...
    _sharedVoxelSystem.killLocalVoxels();
    if (_sharedVoxelSystem.getTree() != &_clipboard) {
        _clipboard.eraseAllOctreeElements();
        _sharedVoxelSystem.changeTree(&_clipboard);
    }

    // then copy onto it if there is something to copy
    VoxelTreeElement* selectedNode = _voxels.getVoxelAt(_mouseVoxel.x, _mouseVoxel.y, _mouseVoxel.z, _mouseVoxel.s);
    if (selectedNode) {
        _voxels.copySubTreeIntoNewTree(selectedNode, &_sharedVoxelSystem, true);
    }
}

void Application::pasteVoxelsToOctalCode(const unsigned char* octalCodeDestination) {
    // Recurse the clipboard tree, where everything is root relative, and send all the colored voxels to
    // the server as an set voxel message, this will also rebase the voxels to the new location
    SendVoxelsOperationArgs args;
    args.newBaseOctCode = octalCodeDestination;
    _sharedVoxelSystem.getTree()->recurseTreeWithOperation(sendVoxelsOperation, &args);

    if (_sharedVoxelSystem.getTree() != &_clipboard) {
        _sharedVoxelSystem.killLocalVoxels();
        _sharedVoxelSystem.changeTree(&_clipboard);
    }

    _voxelEditSender.releaseQueuedMessages();
}

void Application::pasteVoxels() {
    unsigned char* calculatedOctCode = NULL;
    VoxelTreeElement* selectedNode = _voxels.getVoxelAt(_mouseVoxel.x, _mouseVoxel.y, _mouseVoxel.z, _mouseVoxel.s);

    // we only need the selected voxel to get the newBaseOctCode, which we can actually calculate from the
    // voxel size/position details. If we don't have an actual selectedNode then use the mouseVoxel to create a
    // target octalCode for where the user is pointing.
    const unsigned char* octalCodeDestination;
    if (selectedNode) {
        octalCodeDestination = selectedNode->getOctalCode();
    } else {
        octalCodeDestination = calculatedOctCode = pointToVoxel(_mouseVoxel.x, _mouseVoxel.y, _mouseVoxel.z, _mouseVoxel.s);
    }

    pasteVoxelsToOctalCode(octalCodeDestination);

    if (calculatedOctCode) {
        delete[] calculatedOctCode;
    }
}

void Application::findAxisAlignment() {
    glm::vec3 direction = _myAvatar->getMouseRayDirection();
    if (fabs(direction.z) > fabs(direction.x)) {
        _lookingAlongX = false;
        if (direction.z < 0) {
            _lookingAwayFromOrigin = false;
        } else {
            _lookingAwayFromOrigin = true;
        }
    } else {
        _lookingAlongX = true;
        if (direction.x < 0) {
            _lookingAwayFromOrigin = false;
        } else {
            _lookingAwayFromOrigin = true;
        }
    }
}

void Application::nudgeVoxels() {
    VoxelTreeElement* selectedNode = _voxels.getVoxelAt(_mouseVoxel.x, _mouseVoxel.y, _mouseVoxel.z, _mouseVoxel.s);
    if (!_menu->isOptionChecked(MenuOption::VoxelSelectMode) && selectedNode) {
        _menu->triggerOption(MenuOption::VoxelSelectMode);
    }

    if (!_nudgeStarted && selectedNode) {
        _nudgeVoxel = _mouseVoxel;
        _nudgeStarted = true;
        _nudgeGuidePosition = glm::vec3(_nudgeVoxel.x, _nudgeVoxel.y, _nudgeVoxel.z);
        findAxisAlignment();
    } else {
        // calculate nudgeVec
        glm::vec3 nudgeVec(_nudgeGuidePosition.x - _nudgeVoxel.x, _nudgeGuidePosition.y - _nudgeVoxel.y, _nudgeGuidePosition.z - _nudgeVoxel.z);

        VoxelTreeElement* nodeToNudge = _voxels.getVoxelAt(_nudgeVoxel.x, _nudgeVoxel.y, _nudgeVoxel.z, _nudgeVoxel.s);

        if (nodeToNudge) {
            _voxels.getTree()->nudgeSubTree(nodeToNudge, nudgeVec, _voxelEditSender);
            _nudgeStarted = false;
        }
    }
}

void Application::deleteVoxels() {
    deleteVoxelUnderCursor();
}

void Application::initDisplay() {
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_CONSTANT_ALPHA, GL_ONE);
    glShadeModel(GL_SMOOTH);
    glEnable(GL_LIGHTING);
    glEnable(GL_LIGHT0);
    glEnable(GL_DEPTH_TEST);
}

void Application::init() {
    _sharedVoxelSystemViewFrustum.setPosition(glm::vec3(TREE_SCALE / 2.0f,
                                                        TREE_SCALE / 2.0f,
                                                        3.0f * TREE_SCALE / 2.0f));
    _sharedVoxelSystemViewFrustum.setNearClip(TREE_SCALE / 2.0f);
    _sharedVoxelSystemViewFrustum.setFarClip(3.0f * TREE_SCALE / 2.0f);
    _sharedVoxelSystemViewFrustum.setFieldOfView(90);
    _sharedVoxelSystemViewFrustum.setOrientation(glm::quat());
    _sharedVoxelSystemViewFrustum.calculate();
    _sharedVoxelSystem.setViewFrustum(&_sharedVoxelSystemViewFrustum);

    VoxelTreeElement::removeUpdateHook(&_sharedVoxelSystem);

    _sharedVoxelSystem.init();
    VoxelTree* tmpTree = _sharedVoxelSystem.getTree();
    _sharedVoxelSystem.changeTree(&_clipboard);
    delete tmpTree;

    _environment.init();

    _glowEffect.init();
    _ambientOcclusionEffect.init();
    _voxelShader.init();
    _pointShader.init();

    _mouseX = _glWidget->width() / 2;
    _mouseY = _glWidget->height() / 2;
    QCursor::setPos(_mouseX, _mouseY);

    // TODO: move _myAvatar out of Application. Move relevant code to MyAvataar or AvatarManager
    _avatarManager.init();
    _myCamera.setMode(CAMERA_MODE_FIRST_PERSON);
    _myCamera.setModeShiftRate(1.0f);

    _mirrorCamera.setMode(CAMERA_MODE_MIRROR);
    _mirrorCamera.setAspectRatio((float)MIRROR_VIEW_WIDTH / (float)MIRROR_VIEW_HEIGHT);
    _mirrorCamera.setFieldOfView(30);

    OculusManager::connect();
    if (OculusManager::isConnected()) {
        QMetaObject::invokeMethod(_menu->getActionForOption(MenuOption::Fullscreen),
                                  "trigger",
                                  Qt::QueuedConnection);
    }

    TV3DManager::connect();
    if (TV3DManager::isConnected()) {
        QMetaObject::invokeMethod(_menu->getActionForOption(MenuOption::Fullscreen),
                                  "trigger",
                                  Qt::QueuedConnection);
    }

    gettimeofday(&_timerStart, NULL);
    gettimeofday(&_lastTimeUpdated, NULL);

    _menu->loadSettings();
    if (_menu->getAudioJitterBufferSamples() != 0) {
        _audio.setJitterBufferSamples(_menu->getAudioJitterBufferSamples());
    }

    qDebug("Loaded settings");

    if (!_profile.getUsername().isEmpty()) {
        // we have a username for this avatar, ask the data-server for the mesh URL for this avatar
        DataServerClient::getValueForKeyAndUserString(DataServerKey::FaceMeshURL, _profile.getUserString(), &_profile);
        DataServerClient::getValueForKeyAndUserString(DataServerKey::SkeletonURL, _profile.getUserString(), &_profile);
    }

    // Set up VoxelSystem after loading preferences so we can get the desired max voxel count
    _voxels.setMaxVoxels(_menu->getMaxVoxels());
    _voxels.setUseVoxelShader(_menu->isOptionChecked(MenuOption::UseVoxelShader));
    _voxels.setVoxelsAsPoints(_menu->isOptionChecked(MenuOption::VoxelsAsPoints));
    _voxels.setDisableFastVoxelPipeline(false);
    _voxels.init();

    _particles.init();
    _particles.setViewFrustum(getViewFrustum());

    _metavoxels.init();

    _particleCollisionSystem.init(&_particleEditSender, _particles.getTree(), _voxels.getTree(), &_audio, &_avatarManager);

    // connect the _particleCollisionSystem to our script engine's ParticleScriptingInterface
    connect(&_particleCollisionSystem, 
            SIGNAL(particleCollisionWithVoxel(const ParticleID&, const VoxelDetail&)),
            ScriptEngine::getParticlesScriptingInterface(), 
            SLOT(forwardParticleCollisionWithVoxel(const ParticleID&, const VoxelDetail&)));

    connect(&_particleCollisionSystem, 
            SIGNAL(particleCollisionWithParticle(const ParticleID&, const ParticleID&)),
            ScriptEngine::getParticlesScriptingInterface(), 
            SLOT(forwardParticleCollisionWithParticle(const ParticleID&, const ParticleID&)));

    _palette.init(_glWidget->width(), _glWidget->height());
    _palette.addAction(_menu->getActionForOption(MenuOption::VoxelAddMode), 0, 0);
    _palette.addAction(_menu->getActionForOption(MenuOption::VoxelDeleteMode), 0, 1);
    _palette.addTool(&_swatch);
    _palette.addAction(_menu->getActionForOption(MenuOption::VoxelColorMode), 0, 2);
    _palette.addAction(_menu->getActionForOption(MenuOption::VoxelGetColorMode), 0, 3);
    _palette.addAction(_menu->getActionForOption(MenuOption::VoxelSelectMode), 0, 4);

    _pieMenu.init("./resources/images/hifi-interface-tools-v2-pie.svg",
                  _glWidget->width(),
                  _glWidget->height());

    _audio.init(_glWidget);

    _rearMirrorTools = new RearMirrorTools(_glWidget, _mirrorViewRect, _settings);
    connect(_rearMirrorTools, SIGNAL(closeView()), SLOT(closeMirrorView()));
    connect(_rearMirrorTools, SIGNAL(restoreView()), SLOT(restoreMirrorView()));
    connect(_rearMirrorTools, SIGNAL(shrinkView()), SLOT(shrinkMirrorView()));
    connect(_rearMirrorTools, SIGNAL(resetView()), SLOT(resetSensors()));
}

void Application::closeMirrorView() {
    if (_menu->isOptionChecked(MenuOption::Mirror)) {
        _menu->triggerOption(MenuOption::Mirror);;
    }
}

void Application::restoreMirrorView() {
    if (_menu->isOptionChecked(MenuOption::Mirror)) {
        _menu->triggerOption(MenuOption::Mirror);;
    }

    if (!_menu->isOptionChecked(MenuOption::FullscreenMirror)) {
        _menu->triggerOption(MenuOption::FullscreenMirror);
    }
}

void Application::shrinkMirrorView() {
    if (!_menu->isOptionChecked(MenuOption::Mirror)) {
        _menu->triggerOption(MenuOption::Mirror);;
    }

    if (_menu->isOptionChecked(MenuOption::FullscreenMirror)) {
        _menu->triggerOption(MenuOption::FullscreenMirror);
    }
}

const float MAX_AVATAR_EDIT_VELOCITY = 1.0f;
const float MAX_VOXEL_EDIT_DISTANCE = 50.0f;
const float HEAD_SPHERE_RADIUS = 0.07f;

bool Application::isLookingAtMyAvatar(Avatar* avatar) {
    glm::vec3 theirLookat = avatar->getHead().getLookAtPosition();
    glm::vec3 myHeadPosition = _myAvatar->getHead().getPosition();

    if (pointInSphere(theirLookat, myHeadPosition, HEAD_SPHERE_RADIUS * _myAvatar->getScale())) {
        return true;
    }
    return false;
}

void Application::renderHighlightVoxel(VoxelDetail voxel) {
    glDisable(GL_LIGHTING);
    glPushMatrix();
    glScalef(TREE_SCALE, TREE_SCALE, TREE_SCALE);
    const float EDGE_EXPAND = 1.02f;
    glColor3ub(voxel.red + 128, voxel.green + 128, voxel.blue + 128);
    glTranslatef(voxel.x + voxel.s * 0.5f,
                 voxel.y + voxel.s * 0.5f,
                 voxel.z + voxel.s * 0.5f);
    glLineWidth(2.0f);
    glutWireCube(voxel.s * EDGE_EXPAND);
    glPopMatrix();
}

void Application::updateMouseRay() {

    PerformanceWarning warn(_menu->isOptionChecked(MenuOption::PipelineWarnings), "Application::updateMouseRay()");

    _viewFrustum.computePickRay(_mouseX / (float)_glWidget->width(), _mouseY / (float)_glWidget->height(),
				_mouseRayOrigin, _mouseRayDirection);

    // adjust for mirroring
    if (_myCamera.getMode() == CAMERA_MODE_MIRROR) {
        glm::vec3 mouseRayOffset = _mouseRayOrigin - _viewFrustum.getPosition();
        _mouseRayOrigin -= 2.0f * (_viewFrustum.getDirection() * glm::dot(_viewFrustum.getDirection(), mouseRayOffset) +
				   _viewFrustum.getRight() * glm::dot(_viewFrustum.getRight(), mouseRayOffset));
        _mouseRayDirection -= 2.0f * (_viewFrustum.getDirection() * glm::dot(_viewFrustum.getDirection(), _mouseRayDirection) +
				      _viewFrustum.getRight() * glm::dot(_viewFrustum.getRight(), _mouseRayDirection));
    }

    // tell my avatar if the mouse is being pressed...
    _myAvatar->setMousePressed(_mousePressed);

    // tell my avatar the posiion and direction of the ray projected ino the world based on the mouse position
    _myAvatar->setMouseRay(_mouseRayOrigin, _mouseRayDirection);
}

void Application::updateFaceshift() {

    PerformanceWarning warn(_menu->isOptionChecked(MenuOption::PipelineWarnings), "Application::updateFaceshift()");

    //  Update faceshift
    _faceshift.update();

    //  Copy angular velocity if measured by faceshift, to the head
    if (_faceshift.isActive()) {
        _myAvatar->getHead().setAngularVelocity(_faceshift.getHeadAngularVelocity());
    }
}

void Application::updateMyAvatarLookAtPosition(glm::vec3& lookAtSpot) {

    PerformanceWarning warn(_menu->isOptionChecked(MenuOption::PipelineWarnings), "Application::updateMyAvatarLookAtPosition()");

    const float FAR_AWAY_STARE = TREE_SCALE;
    if (_myCamera.getMode() == CAMERA_MODE_MIRROR) {
        lookAtSpot = _myCamera.getPosition();

    } else if (_mouseHidden) {
        // if the mouse cursor is hidden, just look straight ahead
        glm::vec3 rayOrigin, rayDirection;
        _viewFrustum.computePickRay(0.5f, 0.5f, rayOrigin, rayDirection);
        lookAtSpot = rayOrigin + rayDirection * FAR_AWAY_STARE;
    } else {
        // just look in direction of the mouse ray
        lookAtSpot = _mouseRayOrigin + _mouseRayDirection * FAR_AWAY_STARE;
    }
    if (_faceshift.isActive()) {
        // deflect using Faceshift gaze data
        glm::vec3 origin = _myAvatar->getHead().calculateAverageEyePosition();
        float pitchSign = (_myCamera.getMode() == CAMERA_MODE_MIRROR) ? -1.0f : 1.0f;
        float deflection = _menu->getFaceshiftEyeDeflection();
        lookAtSpot = origin + _myCamera.getRotation() * glm::quat(glm::radians(glm::vec3(
											 _faceshift.getEstimatedEyePitch() * pitchSign * deflection, _faceshift.getEstimatedEyeYaw() * deflection, 0.0f))) *
	    glm::inverse(_myCamera.getRotation()) * (lookAtSpot - origin);
    }
    _myAvatar->getHead().setLookAtPosition(lookAtSpot);
}

void Application::updateHoverVoxels(float deltaTime, float& distance, BoxFace& face) {

    PerformanceWarning warn(_menu->isOptionChecked(MenuOption::PipelineWarnings), "Application::updateHoverVoxels()");

    //  If we have clicked on a voxel, update it's color
    if (_isHoverVoxelSounding) {
        VoxelTreeElement* hoveredNode = _voxels.getVoxelAt(_hoverVoxel.x, _hoverVoxel.y, _hoverVoxel.z, _hoverVoxel.s);
        if (hoveredNode) {
            float bright = _audio.getCollisionSoundMagnitude();
            nodeColor clickColor = { 255 * bright + _hoverVoxelOriginalColor[0] * (1.f - bright),
				     _hoverVoxelOriginalColor[1] * (1.f - bright),
				     _hoverVoxelOriginalColor[2] * (1.f - bright), 1 };
            hoveredNode->setColor(clickColor);
            if (bright < 0.01f) {
                hoveredNode->setColor(_hoverVoxelOriginalColor);
                _isHoverVoxelSounding = false;
            }
        } else {
            //  Voxel is not found, clear all
            _isHoverVoxelSounding = false;
            _isHoverVoxel = false;
        }
    } else {
        //  Check for a new hover voxel
        glm::vec4 oldVoxel(_hoverVoxel.x, _hoverVoxel.y, _hoverVoxel.z, _hoverVoxel.s);
        // only do this work if MAKE_SOUND_ON_VOXEL_HOVER or MAKE_SOUND_ON_VOXEL_CLICK is enabled,
        // and make sure the tree is not already busy... because otherwise you'll have to wait.
        if (!_mousePressed) {
            {
		PerformanceWarning warn(_menu->isOptionChecked(MenuOption::PipelineWarnings), "Application::updateHoverVoxels() _voxels.findRayIntersection()");
                _isHoverVoxel = _voxels.findRayIntersection(_mouseRayOrigin, _mouseRayDirection, _hoverVoxel, distance, face);
            }
            if (MAKE_SOUND_ON_VOXEL_HOVER && _isHoverVoxel &&
		glm::vec4(_hoverVoxel.x, _hoverVoxel.y, _hoverVoxel.z, _hoverVoxel.s) != oldVoxel) {

                _hoverVoxelOriginalColor[0] = _hoverVoxel.red;
                _hoverVoxelOriginalColor[1] = _hoverVoxel.green;
                _hoverVoxelOriginalColor[2] = _hoverVoxel.blue;
                _hoverVoxelOriginalColor[3] = 1;
                _audio.startCollisionSound(1.0, HOVER_VOXEL_FREQUENCY * _hoverVoxel.s * TREE_SCALE, 0.0, HOVER_VOXEL_DECAY, false);
                _isHoverVoxelSounding = true;
            }
        }
    }
}

void Application::updateMouseVoxels(float deltaTime, float& distance, BoxFace& face) {

    PerformanceWarning warn(_menu->isOptionChecked(MenuOption::PipelineWarnings), "Application::updateMouseVoxels()");

    _mouseVoxel.s = 0.0f;
    bool wasInitialized = _mouseVoxelScaleInitialized;
    if (_menu->isVoxelModeActionChecked() &&
	(fabs(_myAvatar->getVelocity().x) +
	 fabs(_myAvatar->getVelocity().y) +
	 fabs(_myAvatar->getVelocity().z)) / 3 < MAX_AVATAR_EDIT_VELOCITY) {

        if (_voxels.findRayIntersection(_mouseRayOrigin, _mouseRayDirection, _mouseVoxel, distance, face)) {
	    if (distance < MAX_VOXEL_EDIT_DISTANCE) {
		// set the voxel scale to that of the first moused-over voxel
		if (!wasInitialized) {
		    _mouseVoxelScale = _mouseVoxel.s;
		}
		_mouseVoxelScaleInitialized = true;

		// find the nearest voxel with the desired scale
		if (_mouseVoxelScale > _mouseVoxel.s) {
		    // choose the larger voxel that encompasses the one selected
		    _mouseVoxel.x = _mouseVoxelScale * floorf(_mouseVoxel.x / _mouseVoxelScale);
		    _mouseVoxel.y = _mouseVoxelScale * floorf(_mouseVoxel.y / _mouseVoxelScale);
		    _mouseVoxel.z = _mouseVoxelScale * floorf(_mouseVoxel.z / _mouseVoxelScale);
		    _mouseVoxel.s = _mouseVoxelScale;

		} else {
		    glm::vec3 faceVector = getFaceVector(face);
		    if (_mouseVoxelScale < _mouseVoxel.s) {
			// find the closest contained voxel
			glm::vec3 pt = (_mouseRayOrigin + _mouseRayDirection * distance) / (float)TREE_SCALE -
			    faceVector * (_mouseVoxelScale * 0.5f);
			_mouseVoxel.x = _mouseVoxelScale * floorf(pt.x / _mouseVoxelScale);
			_mouseVoxel.y = _mouseVoxelScale * floorf(pt.y / _mouseVoxelScale);
			_mouseVoxel.z = _mouseVoxelScale * floorf(pt.z / _mouseVoxelScale);
			_mouseVoxel.s = _mouseVoxelScale;
		    }
		    if (_menu->isOptionChecked(MenuOption::VoxelAddMode)) {
			// use the face to determine the side on which to create a neighbor
			_mouseVoxel.x += faceVector.x * _mouseVoxel.s;
			_mouseVoxel.y += faceVector.y * _mouseVoxel.s;
			_mouseVoxel.z += faceVector.z * _mouseVoxel.s;
		    }
		}
	    } else {
		_mouseVoxel.s = 0.0f;
	    }
        } else if (_menu->isOptionChecked(MenuOption::VoxelAddMode)
                   || _menu->isOptionChecked(MenuOption::VoxelSelectMode)) {
	    // place the voxel a fixed distance away
	    float worldMouseVoxelScale = _mouseVoxelScale * TREE_SCALE;
	    glm::vec3 pt = _mouseRayOrigin + _mouseRayDirection * (2.0f + worldMouseVoxelScale * 0.5f);
	    _mouseVoxel.x = _mouseVoxelScale * floorf(pt.x / worldMouseVoxelScale);
	    _mouseVoxel.y = _mouseVoxelScale * floorf(pt.y / worldMouseVoxelScale);
	    _mouseVoxel.z = _mouseVoxelScale * floorf(pt.z / worldMouseVoxelScale);
	    _mouseVoxel.s = _mouseVoxelScale;
        }

        if (_menu->isOptionChecked(MenuOption::VoxelDeleteMode)) {
	    // red indicates deletion
	    _mouseVoxel.red = 255;
	    _mouseVoxel.green = _mouseVoxel.blue = 0;
        } else if (_menu->isOptionChecked(MenuOption::VoxelSelectMode)) {
	    if (_nudgeStarted) {
		_mouseVoxel.red = _mouseVoxel.green = _mouseVoxel.blue = 255;
	    } else {
		// yellow indicates selection
		_mouseVoxel.red = _mouseVoxel.green = 255;
		_mouseVoxel.blue = 0;
	    }
        } else { // _addVoxelMode->isChecked() || _colorVoxelMode->isChecked()
	    QColor paintColor = _menu->getActionForOption(MenuOption::VoxelPaintColor)->data().value<QColor>();
	    _mouseVoxel.red = paintColor.red();
	    _mouseVoxel.green = paintColor.green();
	    _mouseVoxel.blue = paintColor.blue();
        }

        // if we just edited, use the currently selected voxel as the "last" for drag detection
        if (_justEditedVoxel) {
	    _lastMouseVoxelPos = glm::vec3(_mouseVoxel.x, _mouseVoxel.y, _mouseVoxel.z);
	    _justEditedVoxel = false;
        }
    }
}



void Application::updateHandAndTouch(float deltaTime) {
    PerformanceWarning warn(_menu->isOptionChecked(MenuOption::PipelineWarnings), "Application::updateHandAndTouch()");

    //  Update from Touch
    if (_isTouchPressed) {
        _lastTouchAvgX = _touchAvgX;
        _lastTouchAvgY = _touchAvgY;
    }
}

void Application::updateLeap(float deltaTime) {
    PerformanceWarning warn(_menu->isOptionChecked(MenuOption::PipelineWarnings), "Application::updateLeap()");
}

void Application::updateSixense(float deltaTime) {
    PerformanceWarning warn(_menu->isOptionChecked(MenuOption::PipelineWarnings), "Application::updateSixense()");

    _sixenseManager.update(deltaTime);
}

void Application::updateSerialDevices(float deltaTime) {
    PerformanceWarning warn(_menu->isOptionChecked(MenuOption::PipelineWarnings), "Application::updateSerialDevices()");
}

void Application::updateThreads(float deltaTime) {
    PerformanceWarning warn(_menu->isOptionChecked(MenuOption::PipelineWarnings), "Application::updateThreads()");

    // parse voxel packets
    if (!_enableProcessVoxelsThread) {
        _voxelProcessor.threadRoutine();
        _voxelHideShowThread.threadRoutine();
        _voxelEditSender.threadRoutine();
        _particleEditSender.threadRoutine();
    }
}

void Application::updateParticles(float deltaTime) {
    PerformanceWarning warn(_menu->isOptionChecked(MenuOption::PipelineWarnings), "Application::updateParticles()");
    if (_menu->isOptionChecked(MenuOption::ParticleCloud)) {
        _cloud.simulate(deltaTime);
    }
}

void Application::updateMetavoxels(float deltaTime) {
    PerformanceWarning warn(_menu->isOptionChecked(MenuOption::PipelineWarnings), "Application::updateMetavoxels()");
    if (_menu->isOptionChecked(MenuOption::Metavoxels)) {
	if (_menu->isOptionChecked(MenuOption::Metavoxels)) {
	    _metavoxels.simulate(deltaTime);
	}
    }
}

void Application::updateCamera(float deltaTime) {
    PerformanceWarning warn(_menu->isOptionChecked(MenuOption::PipelineWarnings), "Application::updateCamera()");

    if (!OculusManager::isConnected() && !TV3DManager::isConnected()) {
        if (_menu->isOptionChecked(MenuOption::FullscreenMirror)) {
            if (_myCamera.getMode() != CAMERA_MODE_MIRROR) {
                _myCamera.setMode(CAMERA_MODE_MIRROR);
                _myCamera.setModeShiftRate(100.0f);
            }
        } else if (_menu->isOptionChecked(MenuOption::FirstPerson)) {
            if (_myCamera.getMode() != CAMERA_MODE_FIRST_PERSON) {
                _myCamera.setMode(CAMERA_MODE_FIRST_PERSON);
                _myCamera.setModeShiftRate(1.0f);
            }
        } else {
            if (_myCamera.getMode() != CAMERA_MODE_THIRD_PERSON) {
                _myCamera.setMode(CAMERA_MODE_THIRD_PERSON);
                _myCamera.setModeShiftRate(1.0f);
            }
        }

        if (_menu->isOptionChecked(MenuOption::OffAxisProjection)) {
            float xSign = _myCamera.getMode() == CAMERA_MODE_MIRROR ? 1.0f : -1.0f;
            if (_faceshift.isActive()) {
                const float EYE_OFFSET_SCALE = 0.025f;
                glm::vec3 position = _faceshift.getHeadTranslation() * EYE_OFFSET_SCALE;
                _myCamera.setEyeOffsetPosition(glm::vec3(position.x * xSign, position.y, -position.z));
                updateProjectionMatrix();
            }
        }
    }
}


void Application::updateDialogs(float deltaTime) {
    PerformanceWarning warn(_menu->isOptionChecked(MenuOption::PipelineWarnings), "Application::updateDialogs()");

    // Update bandwidth dialog, if any
    BandwidthDialog* bandwidthDialog = _menu->getBandwidthDialog();
    if (bandwidthDialog) {
        bandwidthDialog->update();
    }
    VoxelStatsDialog* voxelStatsDialog = _menu->getVoxelStatsDialog();
    if (voxelStatsDialog) {
        voxelStatsDialog->update();
    }
}

void Application::updateAudio(float deltaTime) {
    PerformanceWarning warn(_menu->isOptionChecked(MenuOption::PipelineWarnings), "Application::updateAudio()");

    //  Update audio stats for procedural sounds
    _audio.setLastAcceleration(_myAvatar->getThrust());
    _audio.setLastVelocity(_myAvatar->getVelocity());
}

void Application::updateCursor(float deltaTime) {
    PerformanceWarning warn(_menu->isOptionChecked(MenuOption::PipelineWarnings), "Application::updateCursor()");

    // watch mouse position, if it hasn't moved, hide the cursor
    bool underMouse = _glWidget->underMouse();
    if (!_mouseHidden) {
        quint64 now = usecTimestampNow();
        int elapsed = now - _lastMouseMove;
        const int HIDE_CURSOR_TIMEOUT = 1 * 1000 * 1000; // 1 second
        if (elapsed > HIDE_CURSOR_TIMEOUT && (underMouse || !_seenMouseMove)) {
            getGLWidget()->setCursor(Qt::BlankCursor);
            _mouseHidden = true;
        }
    } else {
        // if the mouse is hidden, but we're not inside our window, then consider ourselves to be moving
        if (!underMouse && _seenMouseMove) {
            _lastMouseMove = usecTimestampNow();
            getGLWidget()->setCursor(Qt::ArrowCursor);
            _mouseHidden = false;
        }
    }
}

void Application::update(float deltaTime) {
    PerformanceWarning warn(_menu->isOptionChecked(MenuOption::PipelineWarnings), "Application::update()");

    // check what's under the mouse and update the mouse voxel
    updateMouseRay();

    // Set where I am looking based on my mouse ray (so that other people can see)
    glm::vec3 lookAtSpot;

    updateFaceshift();
    _myAvatar->updateLookAtTargetAvatar(lookAtSpot);
    updateMyAvatarLookAtPosition(lookAtSpot);

    //  Find the voxel we are hovering over, and respond if clicked
    float distance;
    BoxFace face;

    updateHoverVoxels(deltaTime, distance, face); // clicking on voxels and making sounds
    updateMouseVoxels(deltaTime, distance, face); // UI/UX related to voxels
    updateHandAndTouch(deltaTime); // Update state for touch sensors
    updateLeap(deltaTime); // Leap finger-sensing device
    updateSixense(deltaTime); // Razer Hydra controllers
    updateSerialDevices(deltaTime); // Read serial port interface devices
    updateMyAvatar(deltaTime); // Sample hardware, update view frustum if needed, and send avatar data to mixer/nodes
    updateThreads(deltaTime); // If running non-threaded, then give the threads some time to process...
    _avatarManager.updateOtherAvatars(deltaTime); //loop through all the other avatars and simulate them...
    updateParticles(deltaTime); // Simulate particle cloud movements
    updateMetavoxels(deltaTime); // update metavoxels
    updateCamera(deltaTime); // handle various camera tweaks like off axis projection
    updateDialogs(deltaTime); // update various stats dialogs if present
    updateAudio(deltaTime); // Update audio stats for procedural sounds
    updateCursor(deltaTime); // Handle cursor updates

    _particles.update(); // update the particles...
    _particleCollisionSystem.update(); // collide the particles...
}

void Application::updateMyAvatar(float deltaTime) {
    PerformanceWarning warn(_menu->isOptionChecked(MenuOption::PipelineWarnings), "Application::updateMyAvatar()");

    _myAvatar->update(deltaTime);

    // send head/hand data to the avatar mixer and voxel server
    QByteArray packet = byteArrayWithPopluatedHeader(PacketTypeAvatarData);
    packet.append(_myAvatar->toByteArray());

    controlledBroadcastToNodes(packet, NodeSet() << NodeType::AvatarMixer);

    // Update _viewFrustum with latest camera and view frustum data...
    // NOTE: we get this from the view frustum, to make it simpler, since the
    // loadViewFrumstum() method will get the correct details from the camera
    // We could optimize this to not actually load the viewFrustum, since we don't
    // actually need to calculate the view frustum planes to send these details
    // to the server.
    loadViewFrustum(_myCamera, _viewFrustum);

    // Update my voxel servers with my current voxel query...
    queryOctree(NodeType::VoxelServer, PacketTypeVoxelQuery, _voxelServerJurisdictions);
    queryOctree(NodeType::ParticleServer, PacketTypeParticleQuery, _particleServerJurisdictions);
}

void Application::queryOctree(NodeType_t serverType, PacketType packetType, NodeToJurisdictionMap& jurisdictions) {

    // if voxels are disabled, then don't send this at all...
    if (!_menu->isOptionChecked(MenuOption::Voxels)) {
        return;
    }

    bool wantExtraDebugging = getLogger()->extraDebugging();

    // These will be the same for all servers, so we can set them up once and then reuse for each server we send to.
    _voxelQuery.setWantLowResMoving(!_menu->isOptionChecked(MenuOption::DisableLowRes));
    _voxelQuery.setWantColor(!_menu->isOptionChecked(MenuOption::DisableColorVoxels));
    _voxelQuery.setWantDelta(!_menu->isOptionChecked(MenuOption::DisableDeltaSending));
    _voxelQuery.setWantOcclusionCulling(_menu->isOptionChecked(MenuOption::EnableOcclusionCulling));
    _voxelQuery.setWantCompression(_menu->isOptionChecked(MenuOption::EnableVoxelPacketCompression));

    _voxelQuery.setCameraPosition(_viewFrustum.getPosition());
    _voxelQuery.setCameraOrientation(_viewFrustum.getOrientation());
    _voxelQuery.setCameraFov(_viewFrustum.getFieldOfView());
    _voxelQuery.setCameraAspectRatio(_viewFrustum.getAspectRatio());
    _voxelQuery.setCameraNearClip(_viewFrustum.getNearClip());
    _voxelQuery.setCameraFarClip(_viewFrustum.getFarClip());
    _voxelQuery.setCameraEyeOffsetPosition(_viewFrustum.getEyeOffsetPosition());
    _voxelQuery.setOctreeSizeScale(_menu->getVoxelSizeScale());
    _voxelQuery.setBoundaryLevelAdjust(_menu->getBoundaryLevelAdjust());

    unsigned char voxelQueryPacket[MAX_PACKET_SIZE];

    // Iterate all of the nodes, and get a count of how many voxel servers we have...
    int totalServers = 0;
    int inViewServers = 0;
    int unknownJurisdictionServers = 0;

    foreach (const SharedNodePointer& node, NodeList::getInstance()->getNodeHash()) {
        // only send to the NodeTypes that are serverType
        if (node->getActiveSocket() != NULL && node->getType() == serverType) {
            totalServers++;

            // get the server bounds for this server
            QUuid nodeUUID = node->getUUID();

            // if we haven't heard from this voxel server, go ahead and send it a query, so we
            // can get the jurisdiction...
            if (jurisdictions.find(nodeUUID) == jurisdictions.end()) {
                unknownJurisdictionServers++;
            } else {
                const JurisdictionMap& map = (jurisdictions)[nodeUUID];

                unsigned char* rootCode = map.getRootOctalCode();

                if (rootCode) {
                    VoxelPositionSize rootDetails;
                    voxelDetailsForCode(rootCode, rootDetails);
                    AABox serverBounds(glm::vec3(rootDetails.x, rootDetails.y, rootDetails.z), rootDetails.s);
                    serverBounds.scale(TREE_SCALE);

                    ViewFrustum::location serverFrustumLocation = _viewFrustum.boxInFrustum(serverBounds);

                    if (serverFrustumLocation != ViewFrustum::OUTSIDE) {
                        inViewServers++;
                    }
                }
            }
        }
    }

    if (wantExtraDebugging && unknownJurisdictionServers > 0) {
        qDebug("Servers: total %d, in view %d, unknown jurisdiction %d",
	       totalServers, inViewServers, unknownJurisdictionServers);
    }

    int perServerPPS = 0;
    const int SMALL_BUDGET = 10;
    int perUnknownServer = SMALL_BUDGET;
    int totalPPS = 0;
    totalPPS = _menu->getMaxVoxelPacketsPerSecond();

    // determine PPS based on number of servers
    if (inViewServers >= 1) {
        // set our preferred PPS to be exactly evenly divided among all of the voxel servers... and allocate 1 PPS
        // for each unknown jurisdiction server
        perServerPPS = (totalPPS / inViewServers) - (unknownJurisdictionServers * perUnknownServer);
    } else {
        if (unknownJurisdictionServers > 0) {
            perUnknownServer = (totalPPS / unknownJurisdictionServers);
        }
    }

    if (wantExtraDebugging && unknownJurisdictionServers > 0) {
        qDebug("perServerPPS: %d perUnknownServer: %d", perServerPPS, perUnknownServer);
    }

    NodeList* nodeList = NodeList::getInstance();

    foreach (const SharedNodePointer& node, nodeList->getNodeHash()) {
        // only send to the NodeTypes that are serverType
        if (node->getActiveSocket() != NULL && node->getType() == serverType) {


            // get the server bounds for this server
            QUuid nodeUUID = node->getUUID();

            bool inView = false;
            bool unknownView = false;

            // if we haven't heard from this voxel server, go ahead and send it a query, so we
            // can get the jurisdiction...
            if (jurisdictions.find(nodeUUID) == jurisdictions.end()) {
                unknownView = true; // assume it's in view
                if (wantExtraDebugging) {
                    qDebug() << "no known jurisdiction for node " << *node << ", assume it's visible.";
                }
            } else {
                const JurisdictionMap& map = (jurisdictions)[nodeUUID];

                unsigned char* rootCode = map.getRootOctalCode();

                if (rootCode) {
                    VoxelPositionSize rootDetails;
                    voxelDetailsForCode(rootCode, rootDetails);
                    AABox serverBounds(glm::vec3(rootDetails.x, rootDetails.y, rootDetails.z), rootDetails.s);
                    serverBounds.scale(TREE_SCALE);

                    ViewFrustum::location serverFrustumLocation = _viewFrustum.boxInFrustum(serverBounds);
                    if (serverFrustumLocation != ViewFrustum::OUTSIDE) {
                        inView = true;
                    } else {
                        inView = false;
                    }
                } else {
                    if (wantExtraDebugging) {
                        qDebug() << "Jurisdiction without RootCode for node " << *node << ". That's unusual!";
                    }
                }
            }

            if (inView) {
                _voxelQuery.setMaxOctreePacketsPerSecond(perServerPPS);
            } else if (unknownView) {
                if (wantExtraDebugging) {
                    qDebug() << "no known jurisdiction for node " << *node << ", give it budget of "
			     << perUnknownServer << " to send us jurisdiction.";
                }

                // set the query's position/orientation to be degenerate in a manner that will get the scene quickly
                // If there's only one server, then don't do this, and just let the normal voxel query pass through
                // as expected... this way, we will actually get a valid scene if there is one to be seen
                if (totalServers > 1) {
                    _voxelQuery.setCameraPosition(glm::vec3(-0.1,-0.1,-0.1));
                    const glm::quat OFF_IN_NEGATIVE_SPACE = glm::quat(-0.5, 0, -0.5, 1.0);
                    _voxelQuery.setCameraOrientation(OFF_IN_NEGATIVE_SPACE);
                    _voxelQuery.setCameraNearClip(0.1f);
                    _voxelQuery.setCameraFarClip(0.1f);
                    if (wantExtraDebugging) {
                        qDebug() << "Using 'minimal' camera position for node" << *node;
                    }
                } else {
                    if (wantExtraDebugging) {
                        qDebug() << "Using regular camera position for node" << *node;
                    }
                }
                _voxelQuery.setMaxOctreePacketsPerSecond(perUnknownServer);
            } else {
                _voxelQuery.setMaxOctreePacketsPerSecond(0);
            }
            // set up the packet for sending...
            unsigned char* endOfVoxelQueryPacket = voxelQueryPacket;

            // insert packet type/version and node UUID
            endOfVoxelQueryPacket += populatePacketHeader(reinterpret_cast<char*>(endOfVoxelQueryPacket), packetType);

            // encode the query data...
            endOfVoxelQueryPacket += _voxelQuery.getBroadcastData(endOfVoxelQueryPacket);

            int packetLength = endOfVoxelQueryPacket - voxelQueryPacket;

            // make sure we still have an active socket
            if (node->getActiveSocket()) {
                nodeList->getNodeSocket().writeDatagram((char*) voxelQueryPacket, packetLength,
                                                        node->getActiveSocket()->getAddress(), node->getActiveSocket()->getPort());
            }

            // Feed number of bytes to corresponding channel of the bandwidth meter
            _bandwidthMeter.outputStream(BandwidthMeter::VOXELS).updateValue(packetLength);
        }
    }
}

/////////////////////////////////////////////////////////////////////////////////////
// loadViewFrustum()
//
// Description: this will load the view frustum bounds for EITHER the head
//                 or the "myCamera".
//
void Application::loadViewFrustum(Camera& camera, ViewFrustum& viewFrustum) {
    // We will use these below, from either the camera or head vectors calculated above
    glm::vec3 position(camera.getPosition());
    float fov         = camera.getFieldOfView();
    float nearClip    = camera.getNearClip();
    float farClip     = camera.getFarClip();
    float aspectRatio = camera.getAspectRatio();

    glm::quat rotation = camera.getRotation();

    // Set the viewFrustum up with the correct position and orientation of the camera
    viewFrustum.setPosition(position);
    viewFrustum.setOrientation(rotation);

    // Also make sure it's got the correct lens details from the camera
    viewFrustum.setAspectRatio(aspectRatio);
    viewFrustum.setFieldOfView(fov);
    viewFrustum.setNearClip(nearClip);
    viewFrustum.setFarClip(farClip);
    viewFrustum.setEyeOffsetPosition(camera.getEyeOffsetPosition());
    viewFrustum.setEyeOffsetOrientation(camera.getEyeOffsetOrientation());

    // Ask the ViewFrustum class to calculate our corners
    viewFrustum.calculate();
}

glm::vec3 Application::getSunDirection() {
    return glm::normalize(_environment.getClosestData(_myCamera.getPosition()).getSunLocation() - _myCamera.getPosition());
}

void Application::updateShadowMap() {
    QOpenGLFramebufferObject* fbo = _textureCache.getShadowFramebufferObject();
    fbo->bind();
    glEnable(GL_DEPTH_TEST);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glViewport(0, 0, fbo->width(), fbo->height());

    glm::vec3 lightDirection = -getSunDirection();
    glm::quat rotation = glm::inverse(rotationBetween(IDENTITY_FRONT, lightDirection));
    glm::vec3 translation = glm::vec3();
    float nearScale = 0.0f;
    const float MAX_SHADOW_DISTANCE = 2.0f;
    float farScale = (MAX_SHADOW_DISTANCE - _viewFrustum.getNearClip()) / (_viewFrustum.getFarClip() - _viewFrustum.getNearClip());
    loadViewFrustum(_myCamera, _viewFrustum);
    glm::vec3 points[] = {
        rotation * (glm::mix(_viewFrustum.getNearTopLeft(), _viewFrustum.getFarTopLeft(), nearScale) + translation),
        rotation * (glm::mix(_viewFrustum.getNearTopRight(), _viewFrustum.getFarTopRight(), nearScale) + translation),
        rotation * (glm::mix(_viewFrustum.getNearBottomLeft(), _viewFrustum.getFarBottomLeft(), nearScale) + translation),
        rotation * (glm::mix(_viewFrustum.getNearBottomRight(), _viewFrustum.getFarBottomRight(), nearScale) + translation),
        rotation * (glm::mix(_viewFrustum.getNearTopLeft(), _viewFrustum.getFarTopLeft(), farScale) + translation),
        rotation * (glm::mix(_viewFrustum.getNearTopRight(), _viewFrustum.getFarTopRight(), farScale) + translation),
        rotation * (glm::mix(_viewFrustum.getNearBottomLeft(), _viewFrustum.getFarBottomLeft(), farScale) + translation),
        rotation * (glm::mix(_viewFrustum.getNearBottomRight(), _viewFrustum.getFarBottomRight(), farScale) + translation) };
    glm::vec3 minima(FLT_MAX, FLT_MAX, FLT_MAX), maxima(-FLT_MAX, -FLT_MAX, -FLT_MAX);
    for (int i = 0; i < sizeof(points) / sizeof(points[0]); i++) {
        minima = glm::min(minima, points[i]);
        maxima = glm::max(maxima, points[i]);
    }

    // stretch out our extents in z so that we get all of the avatars
    minima.z -= _viewFrustum.getFarClip() * 0.5f;
    maxima.z += _viewFrustum.getFarClip() * 0.5f;

    // save the combined matrix for rendering
    _shadowMatrix = glm::transpose(glm::translate(0.5f, 0.5f, 0.5f) * glm::scale(0.5f, 0.5f, 0.5f) *
				   glm::ortho(minima.x, maxima.x, minima.y, maxima.y, -maxima.z, -minima.z) *
				   glm::mat4_cast(rotation) * glm::translate(translation));

    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrtho(minima.x, maxima.x, minima.y, maxima.y, -maxima.z, -minima.z);

    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();
    glm::vec3 axis = glm::axis(rotation);
    glRotatef(glm::angle(rotation), axis.x, axis.y, axis.z);

    // store view matrix without translation, which we'll use for precision-sensitive objects
    glGetFloatv(GL_MODELVIEW_MATRIX, (GLfloat*)&_untranslatedViewMatrix);
    _viewMatrixTranslation = translation;

    glTranslatef(translation.x, translation.y, translation.z);

    _avatarManager.renderAvatars(true);
    _particles.render();

    glPopMatrix();

    glMatrixMode(GL_PROJECTION);
    glPopMatrix();

    glMatrixMode(GL_MODELVIEW);

    fbo->release();

    glViewport(0, 0, _glWidget->width(), _glWidget->height());
}

const GLfloat WHITE_SPECULAR_COLOR[] = { 1.0f, 1.0f, 1.0f, 1.0f };
const GLfloat NO_SPECULAR_COLOR[] = { 0.0f, 0.0f, 0.0f, 1.0f };

void Application::setupWorldLight() {

    //  Setup 3D lights (after the camera transform, so that they are positioned in world space)
    glEnable(GL_COLOR_MATERIAL);
    glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);

    glm::vec3 sunDirection = getSunDirection();
    GLfloat light_position0[] = { sunDirection.x, sunDirection.y, sunDirection.z, 0.0 };
    glLightfv(GL_LIGHT0, GL_POSITION, light_position0);
    GLfloat ambient_color[] = { 0.7f, 0.7f, 0.8f };
    glLightfv(GL_LIGHT0, GL_AMBIENT, ambient_color);
    GLfloat diffuse_color[] = { 0.8f, 0.7f, 0.7f };
    glLightfv(GL_LIGHT0, GL_DIFFUSE, diffuse_color);

    glLightfv(GL_LIGHT0, GL_SPECULAR, WHITE_SPECULAR_COLOR);
    glMaterialfv(GL_FRONT, GL_SPECULAR, WHITE_SPECULAR_COLOR);
    glMateriali(GL_FRONT, GL_SHININESS, 96);
}

void Application::displaySide(Camera& whichCamera, bool selfAvatarOnly) {
    PerformanceWarning warn(_menu->isOptionChecked(MenuOption::PipelineWarnings), "Application::displaySide()");
    // transform by eye offset

    // flip x if in mirror mode (also requires reversing winding order for backface culling)
    if (whichCamera.getMode() == CAMERA_MODE_MIRROR) {
        glScalef(-1.0f, 1.0f, 1.0f);
        glFrontFace(GL_CW);

    } else {
        glFrontFace(GL_CCW);
    }

    glm::vec3 eyeOffsetPos = whichCamera.getEyeOffsetPosition();
    glm::quat eyeOffsetOrient = whichCamera.getEyeOffsetOrientation();
    glm::vec3 eyeOffsetAxis = glm::axis(eyeOffsetOrient);
    glRotatef(-glm::angle(eyeOffsetOrient), eyeOffsetAxis.x, eyeOffsetAxis.y, eyeOffsetAxis.z);
    glTranslatef(-eyeOffsetPos.x, -eyeOffsetPos.y, -eyeOffsetPos.z);

    // transform view according to whichCamera
    // could be myCamera (if in normal mode)
    // or could be viewFrustumOffsetCamera if in offset mode

    glm::quat rotation = whichCamera.getRotation();
    glm::vec3 axis = glm::axis(rotation);
    glRotatef(-glm::angle(rotation), axis.x, axis.y, axis.z);

    // store view matrix without translation, which we'll use for precision-sensitive objects
    glGetFloatv(GL_MODELVIEW_MATRIX, (GLfloat*)&_untranslatedViewMatrix);
    _viewMatrixTranslation = -whichCamera.getPosition();

    glTranslatef(_viewMatrixTranslation.x, _viewMatrixTranslation.y, _viewMatrixTranslation.z);

    //  Setup 3D lights (after the camera transform, so that they are positioned in world space)
    setupWorldLight();
    if (!selfAvatarOnly && _menu->isOptionChecked(MenuOption::Stars)) {
	PerformanceWarning warn(_menu->isOptionChecked(MenuOption::PipelineWarnings),
				"Application::displaySide() ... stars...");
	if (!_stars.isStarsLoaded()) {
	    _stars.generate(STARFIELD_NUM_STARS, STARFIELD_SEED);
	}
	// should be the first rendering pass - w/o depth buffer / lighting

	// compute starfield alpha based on distance from atmosphere
	float alpha = 1.0f;
	if (_menu->isOptionChecked(MenuOption::Atmosphere)) {
	    const EnvironmentData& closestData = _environment.getClosestData(whichCamera.getPosition());
	    float height = glm::distance(whichCamera.getPosition(), closestData.getAtmosphereCenter());
	    if (height < closestData.getAtmosphereInnerRadius()) {
		alpha = 0.0f;

	    } else if (height < closestData.getAtmosphereOuterRadius()) {
		alpha = (height - closestData.getAtmosphereInnerRadius()) /
		    (closestData.getAtmosphereOuterRadius() - closestData.getAtmosphereInnerRadius());
	    }
	}

	// finally render the starfield
	_stars.render(whichCamera.getFieldOfView(), whichCamera.getAspectRatio(), whichCamera.getNearClip(), alpha);
    }

    // draw the sky dome
    if (!selfAvatarOnly && _menu->isOptionChecked(MenuOption::Atmosphere)) {
	PerformanceWarning warn(_menu->isOptionChecked(MenuOption::PipelineWarnings),
				"Application::displaySide() ... atmosphere...");
	_environment.renderAtmospheres(whichCamera);
    }

    glEnable(GL_LIGHTING);
    glEnable(GL_DEPTH_TEST);

    //  Enable to show line from me to the voxel I am touching
    //renderThrustAtVoxel(_voxelThrust);

    if (!selfAvatarOnly) {
        // draw a red sphere
        float originSphereRadius = 0.05f;
        glColor3f(1,0,0);
        glPushMatrix();
	glutSolidSphere(originSphereRadius, 15, 15);
        glPopMatrix();

        // disable specular lighting for ground and voxels
        glMaterialfv(GL_FRONT, GL_SPECULAR, NO_SPECULAR_COLOR);

        //  Draw Cloud Particles
	if(_menu){
	    if (_menu->isOptionChecked(MenuOption::ParticleCloud)) {
		_cloud.render();
	    }
	    //  Draw voxels
	    if (_menu->isOptionChecked(MenuOption::Voxels)) {
		PerformanceWarning warn(_menu->isOptionChecked(MenuOption::PipelineWarnings),
					"Application::displaySide() ... voxels...");
		if (!_menu->isOptionChecked(MenuOption::DontRenderVoxels)) {
		    _voxels.render(_menu->isOptionChecked(MenuOption::VoxelTextures));
		}
	    }

	    // also, metavoxels
	    if (_menu->isOptionChecked(MenuOption::Metavoxels)) {
		PerformanceWarning warn(_menu->isOptionChecked(MenuOption::PipelineWarnings),
					"Application::displaySide() ... metavoxels...");
		_metavoxels.render();
	    }
	}

        // render particles...
        _particles.render();

	// render the ambient occlusion effect if enabled
	if (_menu->isOptionChecked(MenuOption::AmbientOcclusion)) {
            PerformanceWarning warn(_menu->isOptionChecked(MenuOption::PipelineWarnings),
				    "Application::displaySide() ... AmbientOcclusion...");
            _ambientOcclusionEffect.render();
	}

        // restore default, white specular
        glMaterialfv(GL_FRONT, GL_SPECULAR, WHITE_SPECULAR_COLOR);

        //  Render the highlighted voxel
        if (_isHighlightVoxel) {
            renderHighlightVoxel(_highlightVoxel);
        }

        // indicate what we'll be adding/removing in mouse mode, if anything
        if (_mouseVoxel.s != 0 && whichCamera.getMode() != CAMERA_MODE_MIRROR) {
            PerformanceWarning warn(_menu->isOptionChecked(MenuOption::PipelineWarnings),
				    "Application::displaySide() ... voxels TOOLS UX...");

            glDisable(GL_LIGHTING);
            glPushMatrix();
            glScalef(TREE_SCALE, TREE_SCALE, TREE_SCALE);
            const float CUBE_EXPANSION = 1.01f;
            if (_nudgeStarted) {
                renderNudgeGuide(_nudgeGuidePosition.x, _nudgeGuidePosition.y, _nudgeGuidePosition.z, _nudgeVoxel.s);
                renderNudgeGrid(_nudgeVoxel.x, _nudgeVoxel.y, _nudgeVoxel.z, _nudgeVoxel.s, _mouseVoxel.s);
                glPushMatrix();
                glTranslatef(_nudgeVoxel.x + _nudgeVoxel.s * 0.5f,
			     _nudgeVoxel.y + _nudgeVoxel.s * 0.5f,
			     _nudgeVoxel.z + _nudgeVoxel.s * 0.5f);
                glColor3ub(255, 255, 255);
                glLineWidth(4.0f);
                glutWireCube(_nudgeVoxel.s * CUBE_EXPANSION);
                glPopMatrix();
            } else {
                renderMouseVoxelGrid(_mouseVoxel.x, _mouseVoxel.y, _mouseVoxel.z, _mouseVoxel.s);
            }
	    if (_menu->isOptionChecked(MenuOption::VoxelAddMode)) {
		// use a contrasting color so that we can see what we're doing
		glColor3ub(_mouseVoxel.red + 128, _mouseVoxel.green + 128, _mouseVoxel.blue + 128);
	    } else {
		glColor3ub(_mouseVoxel.red, _mouseVoxel.green, _mouseVoxel.blue);
	    }

            if (_nudgeStarted) {
                // render nudge guide cube
                glTranslatef(_nudgeGuidePosition.x + _nudgeVoxel.s*0.5f,
			     _nudgeGuidePosition.y + _nudgeVoxel.s*0.5f,
			     _nudgeGuidePosition.z + _nudgeVoxel.s*0.5f);
                glLineWidth(4.0f);
                glutWireCube(_nudgeVoxel.s * CUBE_EXPANSION);
            } else {
                glTranslatef(_mouseVoxel.x + _mouseVoxel.s*0.5f,
			     _mouseVoxel.y + _mouseVoxel.s*0.5f,
			     _mouseVoxel.z + _mouseVoxel.s*0.5f);
                glLineWidth(4.0f);
                glutWireCube(_mouseVoxel.s * CUBE_EXPANSION);
            }
            glLineWidth(1.0f);
            glPopMatrix();
            glEnable(GL_LIGHTING);
        }

	if (_menu->isOptionChecked(MenuOption::VoxelSelectMode) && _pasteMode && whichCamera.getMode() != CAMERA_MODE_MIRROR) {
	    PerformanceWarning warn(_menu->isOptionChecked(MenuOption::PipelineWarnings),
				    "Application::displaySide() ... PASTE Preview...");

	    glPushMatrix();
	    glTranslatef(_mouseVoxel.x * TREE_SCALE,
			 _mouseVoxel.y * TREE_SCALE,
			 _mouseVoxel.z * TREE_SCALE);
	    glScalef(_mouseVoxel.s,
		     _mouseVoxel.s,
		     _mouseVoxel.s);

	    _sharedVoxelSystem.render(true);
	    glPopMatrix();
	}
    }

    bool renderMyHead = (whichCamera.getInterpolatedMode() != CAMERA_MODE_FIRST_PERSON);
    _avatarManager.renderAvatars(renderMyHead, selfAvatarOnly);

    if (!selfAvatarOnly) {
	if(_menu){
	    //  Render the world box
	    if (whichCamera.getMode() != CAMERA_MODE_MIRROR && _menu->isOptionChecked(MenuOption::Stats)) {
		renderWorldBox();
	    }

	    // brad's frustum for debugging
	    if (_menu->isOptionChecked(MenuOption::DisplayFrustum) && whichCamera.getMode() != CAMERA_MODE_MIRROR) {
		PerformanceWarning warn(_menu->isOptionChecked(MenuOption::PipelineWarnings),
					"Application::displaySide() ... renderViewFrustum...");
		renderViewFrustum(_viewFrustum);
	    }
	}

        // render voxel fades if they exist
        if (_voxelFades.size() > 0) {
            PerformanceWarning warn(_menu->isOptionChecked(MenuOption::PipelineWarnings),
				    "Application::displaySide() ... voxel fades...");
            for(std::vector<VoxelFade>::iterator fade = _voxelFades.begin(); fade != _voxelFades.end();) {
                fade->render();
                if(fade->isDone()) {
                    fade = _voxelFades.erase(fade);
                } else {
                    ++fade;
                }
            }
        }

        // render transmitter pick ray, if non-empty
        _myAvatar->renderTransmitterPickRay();

        // give external parties a change to hook in
        emit renderingInWorldInterface();
    }
}

void Application::loadTranslatedViewMatrix(const glm::vec3& translation) {
    glLoadMatrixf((const GLfloat*)&_untranslatedViewMatrix);
    glTranslatef(translation.x + _viewMatrixTranslation.x, translation.y + _viewMatrixTranslation.y,
		 translation.z + _viewMatrixTranslation.z);
}

void Application::computeOffAxisFrustum(float& left, float& right, float& bottom, float& top, float& nearVal,
					float& farVal, glm::vec4& nearClipPlane, glm::vec4& farClipPlane) const {

    _viewFrustum.computeOffAxisFrustum(left, right, bottom, top, nearVal, farVal, nearClipPlane, farClipPlane);
}

void Application::displayOverlay() {
    PerformanceWarning warn(_menu->isOptionChecked(MenuOption::PipelineWarnings), "Application::displayOverlay()");

    //  Render 2D overlay:  I/O level bar graphs and text
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();

    glLoadIdentity();
    gluOrtho2D(0, _glWidget->width(), _glWidget->height(), 0);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_LIGHTING);

    //  Display a single screen-size quad to create an alpha blended 'collision' flash
    if (_audio.getCollisionFlashesScreen()) {
        float collisionSoundMagnitude = _audio.getCollisionSoundMagnitude();
        const float VISIBLE_COLLISION_SOUND_MAGNITUDE = 0.5f;
        if (collisionSoundMagnitude > VISIBLE_COLLISION_SOUND_MAGNITUDE) {
	    renderCollisionOverlay(_glWidget->width(), _glWidget->height(), _audio.getCollisionSoundMagnitude());
        }
    }

    if (_menu->isOptionChecked(MenuOption::Stats)) {
        displayStatsBackground(0x33333399, 0, _glWidget->height() - 68, 296, 68);
        _audio.render(_glWidget->width(), _glWidget->height());
        if (_menu->isOptionChecked(MenuOption::Oscilloscope)) {
	    int oscilloscopeTop = _menu->isOptionChecked(MenuOption::Mirror) ? 130 : 25;
	    _audioScope.render(25, oscilloscopeTop);
        }
    }

    //noiseTest(_glWidget->width(), _glWidget->height());

    if (_menu->isOptionChecked(MenuOption::HeadMouse)) {
        _myAvatar->renderHeadMouse();
    }


    _myAvatar->renderTransmitterLevels(_glWidget->width(), _glWidget->height());

    //  Display stats and log text onscreen
    glLineWidth(1.0f);
    glPointSize(1.0f);

    if (_menu->isOptionChecked(MenuOption::Stats)) {
	//  Onscreen text about position, servers, etc
	displayStats();
	//  Bandwidth meter
	if (_menu->isOptionChecked(MenuOption::Bandwidth)) {
	    displayStatsBackground(0x33333399, _glWidget->width() - 296, _glWidget->height() - 68, 296, 68);
	    _bandwidthMeter.render(_glWidget->width(), _glWidget->height());
	}
    }

    // testing rendering coverage map
    if (_menu->isOptionChecked(MenuOption::CoverageMapV2)) {
	renderCoverageMapV2();
    }

    if (_menu->isOptionChecked(MenuOption::CoverageMap)) {
	renderCoverageMap();
    }

    //  Show chat entry field
    if (_chatEntryOn) {
	_chatEntry.render(_glWidget->width(), _glWidget->height());
    }

    //  Show on-screen msec timer
    if (_menu->isOptionChecked(MenuOption::FrameTimer)) {
	char frameTimer[10];
	quint64 mSecsNow = floor(usecTimestampNow() / 1000.0 + 0.5);
	sprintf(frameTimer, "%d\n", (int)(mSecsNow % 1000));
	int timerBottom = 
	    (_menu->isOptionChecked(MenuOption::Stats) && 
	     _menu->isOptionChecked(MenuOption::Bandwidth))
	    ? 80 : 20;
	drawtext(_glWidget->width() - 100, _glWidget->height() - timerBottom, 0.30f, 0, 1.0f, 0, frameTimer, 0, 0, 0);
	drawtext(_glWidget->width() - 102, _glWidget->height() - timerBottom - 2, 0.30f, 0, 1.0f, 0, frameTimer, 1, 1, 1);
    }

    _palette.render(_glWidget->width(), _glWidget->height());

    QAction* paintColorAction = NULL;
    if (_menu->isOptionChecked(MenuOption::VoxelGetColorMode)
	&& (paintColorAction = _menu->getActionForOption(MenuOption::VoxelPaintColor))->data().value<QColor>()
	!= _swatch.getColor()) {
	QColor color = paintColorAction->data().value<QColor>();
	TextRenderer textRenderer(SANS_FONT_FAMILY, 11, 50);
	const char line1[] = "Assign this color to a swatch";
	const char line2[] = "by choosing a key from 1 to 8.";

	int left = (_glWidget->width() - POPUP_WIDTH - 2 * POPUP_MARGIN) / 2;
	int top = _glWidget->height() / 40;

	glBegin(GL_POLYGON);
	glColor3f(0.0f, 0.0f, 0.0f);
	for (double a = M_PI; a < 1.5f * M_PI; a += POPUP_STEP) {
	    glVertex2f(left + POPUP_MARGIN * cos(a)              , top + POPUP_MARGIN * sin(a));
	}
	for (double a = 1.5f * M_PI; a < 2.0f * M_PI; a += POPUP_STEP) {
	    glVertex2f(left + POPUP_WIDTH + POPUP_MARGIN * cos(a), top + POPUP_MARGIN * sin(a));
	}
	for (double a = 0.0f; a < 0.5f * M_PI; a += POPUP_STEP) {
	    glVertex2f(left + POPUP_WIDTH + POPUP_MARGIN * cos(a), top + POPUP_HEIGHT + POPUP_MARGIN * sin(a));
	}
	for (double a = 0.5f * M_PI; a < 1.0f * M_PI; a += POPUP_STEP) {
	    glVertex2f(left + POPUP_MARGIN * cos(a)              , top + POPUP_HEIGHT + POPUP_MARGIN * sin(a));
	}
	glEnd();

	glBegin(GL_QUADS);
	glColor3f(color.redF(),
		  color.greenF(),
		  color.blueF());
	glVertex2f(left               , top);
	glVertex2f(left + SWATCH_WIDTH, top);
	glVertex2f(left + SWATCH_WIDTH, top + SWATCH_HEIGHT);
	glVertex2f(left               , top + SWATCH_HEIGHT);
	glEnd();

	glColor3f(1.0f, 1.0f, 1.0f);
	textRenderer.draw(left + SWATCH_WIDTH + POPUP_MARGIN, top + FIRST_LINE_OFFSET , line1);
	textRenderer.draw(left + SWATCH_WIDTH + POPUP_MARGIN, top + SECOND_LINE_OFFSET, line2);
    }
    else {
	_swatch.checkColor();
    }

    if (_pieMenu.isDisplayed()) {
	_pieMenu.render();
    }

    glPopMatrix();
}

// translucent background box that makes stats more readable
void Application::displayStatsBackground(unsigned int rgba, int x, int y, int width, int height) {
    glBegin(GL_QUADS);
    glColor4f(((rgba >> 24) & 0xff) / 255.0f,
              ((rgba >> 16) & 0xff) / 255.0f, 
              ((rgba >> 8) & 0xff)  / 255.0f,
              (rgba & 0xff) / 255.0f);
    glVertex3f(x, y, 0);
    glVertex3f(x + width, y, 0);
    glVertex3f(x + width, y + height, 0);
    glVertex3f(x , y + height, 0);
    glEnd();
    glColor4f(1, 1, 1, 1); 
}

// display expanded or contracted stats
void Application::displayStats() {
    unsigned int backgroundColor = 0x33333399;
    int verticalOffset = 0, horizontalOffset = 0, lines = 0;
    bool mirrorEnabled = _menu->isOptionChecked(MenuOption::Mirror);

    QLocale locale(QLocale::English);
    std::stringstream voxelStats;

    glPointSize(1.0f);

    // we need to take one avatar out so we don't include ourselves
    int totalAvatars = _avatarManager.size() - 1;
    int totalServers = NodeList::getInstance()->size();

    if (mirrorEnabled) {
        horizontalOffset += MIRROR_VIEW_WIDTH + MIRROR_VIEW_LEFT_PADDING * 2;
    }

    lines = _statsExpanded ? 5 : 3;
    displayStatsBackground(backgroundColor, horizontalOffset, 0, 165, lines * STATS_PELS_PER_LINE + 10);
    horizontalOffset += 5;

    char serverNodes[30];
    sprintf(serverNodes, "Servers: %d", totalServers);
    char avatarNodes[30];
    sprintf(avatarNodes, "Avatars: %d", totalAvatars);
    char framesPerSecond[30];
    sprintf(framesPerSecond, "Framerate: %3.0f FPS", _fps);

    verticalOffset += STATS_PELS_PER_LINE;
    drawtext(horizontalOffset, verticalOffset, 0.10f, 0, 1.0f, 2, serverNodes, .93f, .93f, .93f);
    verticalOffset += STATS_PELS_PER_LINE;
    drawtext(horizontalOffset, verticalOffset, 0.10f, 0, 1.0f, 2, avatarNodes, .93f, .93f, .93f);
    verticalOffset += STATS_PELS_PER_LINE;
    drawtext(horizontalOffset, verticalOffset, 0.10f, 0, 1.0, 2, framesPerSecond, .93f, .93f, .93f);

    if (_statsExpanded) {
        char packetsPerSecond[30];
        sprintf(packetsPerSecond, "Pkts/sec: %d", _packetsPerSecond);
        char averageMegabitsPerSecond[30];
        sprintf(averageMegabitsPerSecond, "Avg Mbps: %3.2f", (float)_bytesPerSecond * 8.f / 1000000.f);        

        verticalOffset += STATS_PELS_PER_LINE;
        drawtext(horizontalOffset, verticalOffset, 0.10f, 0, 1.0, 2, packetsPerSecond, .93f, .93f, .93f);
        verticalOffset += STATS_PELS_PER_LINE;
        drawtext(horizontalOffset, verticalOffset, 0.10f, 0, 1.0, 2, averageMegabitsPerSecond, .93f, .93f, .93f);
    }

    verticalOffset = 0;
    horizontalOffset += 161;

    if (_menu->isOptionChecked(MenuOption::TestPing)) {
        int pingAudio = 0, pingAvatar = 0, pingVoxel = 0, pingVoxelMax = 0;

        NodeList* nodeList = NodeList::getInstance();
        SharedNodePointer audioMixerNode = nodeList->soloNodeOfType(NodeType::AudioMixer);
        SharedNodePointer avatarMixerNode = nodeList->soloNodeOfType(NodeType::AvatarMixer);

        pingAudio = audioMixerNode ? audioMixerNode->getPingMs() : 0;
        pingAvatar = avatarMixerNode ? avatarMixerNode->getPingMs() : 0;

        // Now handle voxel servers, since there could be more than one, we average their ping times
        unsigned long totalPingVoxel = 0;
        int voxelServerCount = 0;

        foreach (const SharedNodePointer& node, nodeList->getNodeHash()) {
            if (node->getType() == NodeType::VoxelServer) {
                totalPingVoxel += node->getPingMs();
                voxelServerCount++;
                if (pingVoxelMax < node->getPingMs()) {
                    pingVoxelMax = node->getPingMs();
                }
            }
        }

        if (voxelServerCount) {
            pingVoxel = totalPingVoxel/voxelServerCount;
        }

        lines = _statsExpanded ? 4 : 3;
        displayStatsBackground(backgroundColor, horizontalOffset, 0, 175, lines * STATS_PELS_PER_LINE + 10);
        horizontalOffset += 5;

        char audioPing[30];
        sprintf(audioPing, "Audio ping: %d", pingAudio);
        char avatarPing[30];
        sprintf(avatarPing, "Avatar ping: %d", pingAvatar);
        char voxelAvgPing[30];
        sprintf(voxelAvgPing, "Voxel avg ping: %d", pingVoxel);

        verticalOffset += STATS_PELS_PER_LINE;
        drawtext(horizontalOffset, verticalOffset, 0.10f, 0, 1.0, 2, audioPing, .93f, .93f, .93f);
        verticalOffset += STATS_PELS_PER_LINE;
        drawtext(horizontalOffset, verticalOffset, 0.10f, 0, 1.0, 2, avatarPing, .93f, .93f, .93f);
        verticalOffset += STATS_PELS_PER_LINE;
        drawtext(horizontalOffset, verticalOffset, 0.10f, 0, 1.0, 2, voxelAvgPing, .93f, .93f, .93f);

        if (_statsExpanded) {
            char voxelMaxPing[30];
            sprintf(voxelMaxPing, "Voxel max ping: %d", pingVoxelMax);

            verticalOffset += STATS_PELS_PER_LINE;
            drawtext(horizontalOffset, verticalOffset, 0.10f, 0, 1.0, 2, voxelMaxPing, .93f, .93f, .93f);
        }

        verticalOffset = 0;
        horizontalOffset += 171;
    }

    glm::vec3 avatarPos = _myAvatar->getPosition();

    lines = _statsExpanded ? 4 : 3;
    displayStatsBackground(backgroundColor, horizontalOffset, 0, _glWidget->width() - (mirrorEnabled ? 301 : 411) - horizontalOffset, lines * STATS_PELS_PER_LINE + 10);
    horizontalOffset += 5;

    char avatarPosition[200];
    if (mirrorEnabled) {
        // shorthand formatting
        sprintf(avatarPosition, "Pos: %.0f,%.0f,%.0f", avatarPos.x, avatarPos.y, avatarPos.z);
    } else {
        // longhand way
        sprintf(avatarPosition, "Position: %.3f, %.3f, %.3f", avatarPos.x, avatarPos.y, avatarPos.z);
    }    
    char avatarVelocity[30];
    sprintf(avatarVelocity, "Velocity: %.1f", glm::length(_myAvatar->getVelocity()));
    char avatarBodyYaw[30];
    sprintf(avatarBodyYaw, "Yaw: %.2f", _myAvatar->getBodyYaw());
    char avatarMixerStats[200];

    verticalOffset += STATS_PELS_PER_LINE;
    drawtext(horizontalOffset, verticalOffset, 0.10f, 0, 1.0, 2, avatarPosition, .93f, .93f, .93f);
    verticalOffset += STATS_PELS_PER_LINE;
    drawtext(horizontalOffset, verticalOffset, 0.10f, 0, 1.0, 2, avatarVelocity, .93f, .93f, .93f);
    verticalOffset += STATS_PELS_PER_LINE;
    drawtext(horizontalOffset, verticalOffset, 0.10f, 0, 1.0, 2, avatarBodyYaw, .93f, .93f, .93f);

    if (_statsExpanded) {
        SharedNodePointer avatarMixer = NodeList::getInstance()->soloNodeOfType(NodeType::AvatarMixer);
        if (avatarMixer) {
            sprintf(avatarMixerStats, "Avatar Mixer: %.f kbps, %.f pps",
                    roundf(avatarMixer->getAverageKilobitsPerSecond()),
                    roundf(avatarMixer->getAveragePacketsPerSecond()));
        } else {
            sprintf(avatarMixerStats, "No Avatar Mixer");
        }

        verticalOffset += STATS_PELS_PER_LINE;
        drawtext(horizontalOffset, verticalOffset, 0.10f, 0, 1.0, 2, avatarMixerStats, .93f, .93f, .93f);
    }

    verticalOffset = 0;
    horizontalOffset = _glWidget->width() - (mirrorEnabled ? 300 : 410);

    lines = _statsExpanded ? 11 : 3;
    displayStatsBackground(backgroundColor, horizontalOffset, 0, _glWidget->width() - horizontalOffset, lines * STATS_PELS_PER_LINE + 10);
    horizontalOffset += 5;

    if (_statsExpanded) {
        // Local Voxel Memory Usage
        voxelStats.str("");
        voxelStats << "Voxels Memory Nodes: " << VoxelTreeElement::getTotalMemoryUsage() / 1000000.f << "MB";
        verticalOffset += STATS_PELS_PER_LINE;
        drawtext(horizontalOffset, verticalOffset, 0.10f, 0, 1.0, 2, (char*)voxelStats.str().c_str(), .93f, .93f, .93f);

        voxelStats.str("");
        voxelStats << 
	    "Geometry RAM: " << _voxels.getVoxelMemoryUsageRAM() / 1000000.f << "MB / " <<
	    "VBO: " << _voxels.getVoxelMemoryUsageVBO() / 1000000.f << "MB";
        if (_voxels.hasVoxelMemoryUsageGPU()) {
            voxelStats << " / GPU: " << _voxels.getVoxelMemoryUsageGPU() / 1000000.f << "MB";
        }
        verticalOffset += STATS_PELS_PER_LINE;
        drawtext(horizontalOffset, verticalOffset, 0.10f, 0, 1.0, 2, (char*)voxelStats.str().c_str(), .93f, .93f, .93f);

        // Voxel Rendering
        voxelStats.str("");
        voxelStats.precision(4);
        voxelStats << "Voxel Rendering Slots Max: " << _voxels.getMaxVoxels() / 1000.f << "K";
        verticalOffset += STATS_PELS_PER_LINE;
        drawtext(horizontalOffset, verticalOffset, 0.10f, 0, 1.0, 2, (char*)voxelStats.str().c_str(), .93f, .93f, .93f);        
    }

    voxelStats.str("");
    voxelStats.precision(4);
    voxelStats << "Drawn: " << _voxels.getVoxelsWritten() / 1000.f << "K " <<
        "Abandoned: " << _voxels.getAbandonedVoxels() / 1000.f << "K ";
    verticalOffset += STATS_PELS_PER_LINE;
    drawtext(horizontalOffset, verticalOffset, 0.10f, 0, 1.0, 2, (char*)voxelStats.str().c_str(), .93f, .93f, .93f);

    // iterate all the current voxel stats, and list their sending modes, and total voxel counts
    std::stringstream sendingMode("");
    sendingMode << "Octree Sending Mode: [";
    int serverCount = 0;
    int movingServerCount = 0;
    unsigned long totalNodes = 0;
    unsigned long totalInternal = 0;
    unsigned long totalLeaves = 0;
    for(NodeToVoxelSceneStatsIterator i = _octreeServerSceneStats.begin(); i != _octreeServerSceneStats.end(); i++) {
        //const QUuid& uuid = i->first;
        VoxelSceneStats& stats = i->second;
        serverCount++;
        if (_statsExpanded) {
            if (serverCount > 1) {
                sendingMode << ",";
            }
            if (stats.isMoving()) {
                sendingMode << "M";
                movingServerCount++;
            } else {
                sendingMode << "S";
            }
        }

        // calculate server node totals
        totalNodes += stats.getTotalElements();
        if (_statsExpanded) {
            totalInternal += stats.getTotalInternal();
            totalLeaves += stats.getTotalLeaves();                
        }
    }
    if (_statsExpanded) {
        if (serverCount == 0) {
            sendingMode << "---";
        }
        sendingMode << "] " << serverCount << " servers";
        if (movingServerCount > 0) {
            sendingMode << " <SCENE NOT STABLE>";
        } else {
            sendingMode << " <SCENE STABLE>";
        }
        verticalOffset += STATS_PELS_PER_LINE;
        drawtext(horizontalOffset, verticalOffset, 0.10f, 0, 1.0, 2, (char*)sendingMode.str().c_str(), .93f, .93f, .93f);
    }

    // Incoming packets
    int voxelPacketsToProcess = _voxelProcessor.packetsToProcessCount();
    if (_statsExpanded) {
        voxelStats.str("");
        QString packetsString = locale.toString((int)voxelPacketsToProcess);
        QString maxString = locale.toString((int)_recentMaxPackets);
        voxelStats << "Voxel Packets to Process: " << packetsString.toLocal8Bit().constData()
		   << " [Recent Max: " << maxString.toLocal8Bit().constData() << "]";        
        verticalOffset += STATS_PELS_PER_LINE;
        drawtext(horizontalOffset, verticalOffset, 0.10f, 0, 1.0, 2, (char*)voxelStats.str().c_str(), .93f, .93f, .93f);
    }

    if (_resetRecentMaxPacketsSoon && voxelPacketsToProcess > 0) {
        _recentMaxPackets = 0;
        _resetRecentMaxPacketsSoon = false;
    }
    if (voxelPacketsToProcess == 0) {
        _resetRecentMaxPacketsSoon = true;
    } else {
        if (voxelPacketsToProcess > _recentMaxPackets) {
            _recentMaxPackets = voxelPacketsToProcess;
        }
    }

    verticalOffset += (_statsExpanded ? STATS_PELS_PER_LINE : 0);

    QString serversTotalString = locale.toString((uint)totalNodes); // consider adding: .rightJustified(10, ' ');

    // Server Voxels
    voxelStats.str("");
    voxelStats << "Server voxels: " << serversTotalString.toLocal8Bit().constData();
    verticalOffset += STATS_PELS_PER_LINE;
    drawtext(horizontalOffset, verticalOffset, 0.10f, 0, 1.0, 2, (char*)voxelStats.str().c_str(), .93f, .93f, .93f);

    if (_statsExpanded) {
        QString serversInternalString = locale.toString((uint)totalInternal);
        QString serversLeavesString = locale.toString((uint)totalLeaves);

        voxelStats.str("");
        voxelStats <<
            "Internal: " << serversInternalString.toLocal8Bit().constData() << "  " <<
            "Leaves: " << serversLeavesString.toLocal8Bit().constData() << "";
        verticalOffset += STATS_PELS_PER_LINE;
        drawtext(horizontalOffset, verticalOffset, 0.10f, 0, 1.0, 2, (char*)voxelStats.str().c_str(), .93f, .93f, .93f);        
    }

    unsigned long localTotal = VoxelTreeElement::getNodeCount();
    QString localTotalString = locale.toString((uint)localTotal); // consider adding: .rightJustified(10, ' ');

    // Local Voxels
    voxelStats.str("");
    voxelStats << "Local voxels: " << localTotalString.toLocal8Bit().constData();
    verticalOffset += STATS_PELS_PER_LINE;
    drawtext(horizontalOffset, verticalOffset, 0.10f, 0, 1.0, 2, (char*)voxelStats.str().c_str(), .93f, .93f, .93f);

    if (_statsExpanded) {
        unsigned long localInternal = VoxelTreeElement::getInternalNodeCount();
        unsigned long localLeaves = VoxelTreeElement::getLeafNodeCount();
        QString localInternalString = locale.toString((uint)localInternal);
        QString localLeavesString = locale.toString((uint)localLeaves);

        voxelStats.str("");
        voxelStats <<
            "Internal: " << localInternalString.toLocal8Bit().constData() << "  " <<
            "Leaves: " << localLeavesString.toLocal8Bit().constData() << "";
        verticalOffset += STATS_PELS_PER_LINE;
        drawtext(horizontalOffset, verticalOffset, 0.10f, 0, 1.0, 2, (char*)voxelStats.str().c_str(), .93f, .93f, .93f);
    }
}

// called on mouse click release
// check for clicks over stats  in order to expand or contract them
void Application::checkStatsClick() {
    if (0 != glm::compMax(glm::abs(glm::ivec2(_mouseX - _mouseDragStartedX, _mouseY - _mouseDragStartedY)))) {
        // not worried about dragging on stats
        return;
    }

    int statsHeight = 0, statsWidth = 0, statsX = 0, statsY = 0, lines = 0;

    if (_menu->isOptionChecked(MenuOption::Mirror)) {
        statsX += MIRROR_VIEW_WIDTH;
    }

    // top-left stats click
    lines = _statsExpanded ? 5 : 3;
    statsHeight = lines * STATS_PELS_PER_LINE + 10;
    statsWidth = 165;
    if (_mouseX > statsX && _mouseX < statsX + statsWidth  && _mouseY > statsY && _mouseY < statsY + statsHeight) {
        toggleStatsExpanded();
        return;
    }

    // ping stats click
    lines = _statsExpanded ? 4 : 3;
    statsX += statsWidth;
    statsHeight = lines * STATS_PELS_PER_LINE + 10;
    statsWidth = 175;
    if (_mouseX > statsX && _mouseX < statsX + statsWidth  && _mouseY > statsY && _mouseY < statsY + statsHeight) {
        toggleStatsExpanded();
        return;
    }

    // top-center stats panel click
    lines = _statsExpanded ? 4 : 3;
    statsX += statsWidth;
    statsHeight = lines * STATS_PELS_PER_LINE + 10;
    statsWidth = _glWidget->width() - 411 - statsX;
    if (_mouseX > statsX && _mouseX < statsX + statsWidth  && _mouseY > statsY && _mouseY < statsY + statsHeight) {
        toggleStatsExpanded();
        return;
    }

    // top-right stats click
    lines = _statsExpanded ? 11 : 3;
    statsX = _glWidget->width() - 410;
    statsHeight = lines * STATS_PELS_PER_LINE + 10;
    statsWidth = _glWidget->width() - statsX;
    if (_mouseX > statsX && _mouseX < statsX + statsWidth  && _mouseY > statsY && _mouseY < statsY + statsHeight) {
        toggleStatsExpanded();
        return;
    }
}

void Application::toggleStatsExpanded() {
    _statsExpanded = !_statsExpanded;
}

void Application::renderThrustAtVoxel(const glm::vec3& thrust) {
    if (_mousePressed) {
        glColor3f(1, 0, 0);
        glLineWidth(2.0f);
        glBegin(GL_LINES);
        glm::vec3 voxelTouched = getMouseVoxelWorldCoordinates(_mouseVoxelDragging);
        glVertex3f(voxelTouched.x, voxelTouched.y, voxelTouched.z);
        glVertex3f(voxelTouched.x + thrust.x, voxelTouched.y + thrust.y, voxelTouched.z + thrust.z);
        glEnd();
    }
}


glm::vec2 Application::getScaledScreenPoint(glm::vec2 projectedPoint) {
    float horizontalScale = _glWidget->width() / 2.0f;
    float verticalScale   = _glWidget->height() / 2.0f;

    // -1,-1 is 0,windowHeight
    // 1,1 is windowWidth,0

    // -1,1                    1,1
    // +-----------------------+
    // |           |           |
    // |           |           |
    // | -1,0      |           |
    // |-----------+-----------|
    // |          0,0          |
    // |           |           |
    // |           |           |
    // |           |           |
    // +-----------------------+
    // -1,-1                   1,-1

    glm::vec2 screenPoint((projectedPoint.x + 1.0) * horizontalScale,
			  ((projectedPoint.y + 1.0) * -verticalScale) + _glWidget->height());

    return screenPoint;
}

// render the coverage map on screen
void Application::renderCoverageMapV2() {
    glDisable(GL_LIGHTING);
    glLineWidth(2.0);
    glBegin(GL_LINES);
    glColor3f(0,1,1);

    renderCoverageMapsV2Recursively(&_voxels.myCoverageMapV2);

    glEnd();
    glEnable(GL_LIGHTING);
}

void Application::renderCoverageMapsV2Recursively(CoverageMapV2* map) {
    // render ourselves...
    if (map->isCovered()) {
        BoundingBox box = map->getBoundingBox();

        glm::vec2 firstPoint = getScaledScreenPoint(box.getVertex(0));
        glm::vec2 lastPoint(firstPoint);

        for (int i = 1; i < box.getVertexCount(); i++) {
            glm::vec2 thisPoint = getScaledScreenPoint(box.getVertex(i));

            glVertex2f(lastPoint.x, lastPoint.y);
            glVertex2f(thisPoint.x, thisPoint.y);
            lastPoint = thisPoint;
        }

        glVertex2f(lastPoint.x, lastPoint.y);
        glVertex2f(firstPoint.x, firstPoint.y);
    } else {
        // iterate our children and call render on them.
        for (int i = 0; i < CoverageMapV2::NUMBER_OF_CHILDREN; i++) {
            CoverageMapV2* childMap = map->getChild(i);
            if (childMap) {
                renderCoverageMapsV2Recursively(childMap);
            }
        }
    }
}

// render the coverage map on screen
void Application::renderCoverageMap() {

    glDisable(GL_LIGHTING);
    glLineWidth(2.0);
    glBegin(GL_LINES);
    glColor3f(0,0,1);

    renderCoverageMapsRecursively(&_voxels.myCoverageMap);

    glEnd();
    glEnable(GL_LIGHTING);
}

void Application::renderCoverageMapsRecursively(CoverageMap* map) {
    for (int i = 0; i < map->getPolygonCount(); i++) {

        OctreeProjectedPolygon* polygon = map->getPolygon(i);

        if (polygon->getProjectionType()        == (PROJECTION_RIGHT | PROJECTION_NEAR | PROJECTION_BOTTOM)) {
            glColor3f(.5,0,0); // dark red
        } else if (polygon->getProjectionType() == (PROJECTION_NEAR | PROJECTION_RIGHT)) {
            glColor3f(.5,.5,0); // dark yellow
        } else if (polygon->getProjectionType() == (PROJECTION_NEAR | PROJECTION_LEFT)) {
            glColor3f(.5,.5,.5); // gray
        } else if (polygon->getProjectionType() == (PROJECTION_NEAR | PROJECTION_LEFT | PROJECTION_BOTTOM)) {
            glColor3f(.5,0,.5); // dark magenta
        } else if (polygon->getProjectionType() == (PROJECTION_NEAR | PROJECTION_BOTTOM)) {
            glColor3f(.75,0,0); // red
        } else if (polygon->getProjectionType() == (PROJECTION_NEAR | PROJECTION_TOP)) {
            glColor3f(1,0,1); // magenta
        } else if (polygon->getProjectionType() == (PROJECTION_NEAR | PROJECTION_LEFT | PROJECTION_TOP)) {
            glColor3f(0,0,1); // Blue
        } else if (polygon->getProjectionType() == (PROJECTION_NEAR | PROJECTION_RIGHT | PROJECTION_TOP)) {
            glColor3f(0,1,0); // green
        } else if (polygon->getProjectionType() == (PROJECTION_NEAR)) {
            glColor3f(1,1,0); // yellow
        } else if (polygon->getProjectionType() == (PROJECTION_FAR | PROJECTION_RIGHT | PROJECTION_BOTTOM)) {
            glColor3f(0,.5,.5); // dark cyan
        } else {
            glColor3f(1,0,0);
        }

        glm::vec2 firstPoint = getScaledScreenPoint(polygon->getVertex(0));
        glm::vec2 lastPoint(firstPoint);

        for (int i = 1; i < polygon->getVertexCount(); i++) {
            glm::vec2 thisPoint = getScaledScreenPoint(polygon->getVertex(i));

            glVertex2f(lastPoint.x, lastPoint.y);
            glVertex2f(thisPoint.x, thisPoint.y);
            lastPoint = thisPoint;
        }

        glVertex2f(lastPoint.x, lastPoint.y);
        glVertex2f(firstPoint.x, firstPoint.y);
    }

    // iterate our children and call render on them.
    for (int i = 0; i < CoverageMapV2::NUMBER_OF_CHILDREN; i++) {
        CoverageMap* childMap = map->getChild(i);
        if (childMap) {
            renderCoverageMapsRecursively(childMap);
        }
    }
}

// renderViewFrustum()
//
// Description: this will render the view frustum bounds for EITHER the head
//                 or the "myCamera".
//
// Frustum rendering mode. For debug purposes, we allow drawing the frustum in a couple of different ways.
// We can draw it with each of these parts:
//    * Origin Direction/Up/Right vectors - these will be drawn at the point of the camera
//    * Near plane - this plane is drawn very close to the origin point.
//    * Right/Left planes - these two planes are drawn between the near and far planes.
//    * Far plane - the plane is drawn in the distance.
// Modes - the following modes, will draw the following parts.
//    * All - draws all the parts listed above
//    * Planes - draws the planes but not the origin vectors
//    * Origin Vectors - draws the origin vectors ONLY
//    * Near Plane - draws only the near plane
//    * Far Plane - draws only the far plane
void Application::renderViewFrustum(ViewFrustum& viewFrustum) {
    // Load it with the latest details!
    loadViewFrustum(_myCamera, viewFrustum);

    glm::vec3 position  = viewFrustum.getOffsetPosition();
    glm::vec3 direction = viewFrustum.getOffsetDirection();
    glm::vec3 up        = viewFrustum.getOffsetUp();
    glm::vec3 right     = viewFrustum.getOffsetRight();

    //  Get ready to draw some lines
    glDisable(GL_LIGHTING);
    glColor4f(1.0, 1.0, 1.0, 1.0);
    glLineWidth(1.0);
    glBegin(GL_LINES);

    if (_menu->getFrustumDrawMode() == FRUSTUM_DRAW_MODE_ALL
        || _menu->getFrustumDrawMode() == FRUSTUM_DRAW_MODE_VECTORS) {
        // Calculate the origin direction vectors
        glm::vec3 lookingAt      = position + (direction * 0.2f);
        glm::vec3 lookingAtUp    = position + (up * 0.2f);
        glm::vec3 lookingAtRight = position + (right * 0.2f);

        // Looking At = white
        glColor3f(1,1,1);
        glVertex3f(position.x, position.y, position.z);
        glVertex3f(lookingAt.x, lookingAt.y, lookingAt.z);

        // Looking At Up = purple
        glColor3f(1,0,1);
        glVertex3f(position.x, position.y, position.z);
        glVertex3f(lookingAtUp.x, lookingAtUp.y, lookingAtUp.z);

        // Looking At Right = cyan
        glColor3f(0,1,1);
        glVertex3f(position.x, position.y, position.z);
        glVertex3f(lookingAtRight.x, lookingAtRight.y, lookingAtRight.z);
    }

    if (_menu->getFrustumDrawMode() == FRUSTUM_DRAW_MODE_ALL
        || _menu->getFrustumDrawMode() == FRUSTUM_DRAW_MODE_PLANES
        || _menu->getFrustumDrawMode() == FRUSTUM_DRAW_MODE_NEAR_PLANE) {
        // Drawing the bounds of the frustum
        // viewFrustum.getNear plane - bottom edge
        glColor3f(1,0,0);
        glVertex3f(viewFrustum.getNearBottomLeft().x, viewFrustum.getNearBottomLeft().y, viewFrustum.getNearBottomLeft().z);
        glVertex3f(viewFrustum.getNearBottomRight().x, viewFrustum.getNearBottomRight().y, viewFrustum.getNearBottomRight().z);

        // viewFrustum.getNear plane - top edge
        glVertex3f(viewFrustum.getNearTopLeft().x, viewFrustum.getNearTopLeft().y, viewFrustum.getNearTopLeft().z);
        glVertex3f(viewFrustum.getNearTopRight().x, viewFrustum.getNearTopRight().y, viewFrustum.getNearTopRight().z);

        // viewFrustum.getNear plane - right edge
        glVertex3f(viewFrustum.getNearBottomRight().x, viewFrustum.getNearBottomRight().y, viewFrustum.getNearBottomRight().z);
        glVertex3f(viewFrustum.getNearTopRight().x, viewFrustum.getNearTopRight().y, viewFrustum.getNearTopRight().z);

        // viewFrustum.getNear plane - left edge
        glVertex3f(viewFrustum.getNearBottomLeft().x, viewFrustum.getNearBottomLeft().y, viewFrustum.getNearBottomLeft().z);
        glVertex3f(viewFrustum.getNearTopLeft().x, viewFrustum.getNearTopLeft().y, viewFrustum.getNearTopLeft().z);
    }

    if (_menu->getFrustumDrawMode() == FRUSTUM_DRAW_MODE_ALL
        || _menu->getFrustumDrawMode() == FRUSTUM_DRAW_MODE_PLANES
        || _menu->getFrustumDrawMode() == FRUSTUM_DRAW_MODE_FAR_PLANE) {
        // viewFrustum.getFar plane - bottom edge
        glColor3f(0,1,0);
        glVertex3f(viewFrustum.getFarBottomLeft().x, viewFrustum.getFarBottomLeft().y, viewFrustum.getFarBottomLeft().z);
        glVertex3f(viewFrustum.getFarBottomRight().x, viewFrustum.getFarBottomRight().y, viewFrustum.getFarBottomRight().z);

        // viewFrustum.getFar plane - top edge
        glVertex3f(viewFrustum.getFarTopLeft().x, viewFrustum.getFarTopLeft().y, viewFrustum.getFarTopLeft().z);
        glVertex3f(viewFrustum.getFarTopRight().x, viewFrustum.getFarTopRight().y, viewFrustum.getFarTopRight().z);

        // viewFrustum.getFar plane - right edge
        glVertex3f(viewFrustum.getFarBottomRight().x, viewFrustum.getFarBottomRight().y, viewFrustum.getFarBottomRight().z);
        glVertex3f(viewFrustum.getFarTopRight().x, viewFrustum.getFarTopRight().y, viewFrustum.getFarTopRight().z);

        // viewFrustum.getFar plane - left edge
        glVertex3f(viewFrustum.getFarBottomLeft().x, viewFrustum.getFarBottomLeft().y, viewFrustum.getFarBottomLeft().z);
        glVertex3f(viewFrustum.getFarTopLeft().x, viewFrustum.getFarTopLeft().y, viewFrustum.getFarTopLeft().z);
    }

    if (_menu->getFrustumDrawMode() == FRUSTUM_DRAW_MODE_ALL
        || _menu->getFrustumDrawMode() == FRUSTUM_DRAW_MODE_PLANES) {
        // RIGHT PLANE IS CYAN
        // right plane - bottom edge - viewFrustum.getNear to distant
        glColor3f(0,1,1);
        glVertex3f(viewFrustum.getNearBottomRight().x, viewFrustum.getNearBottomRight().y, viewFrustum.getNearBottomRight().z);
        glVertex3f(viewFrustum.getFarBottomRight().x, viewFrustum.getFarBottomRight().y, viewFrustum.getFarBottomRight().z);

        // right plane - top edge - viewFrustum.getNear to distant
        glVertex3f(viewFrustum.getNearTopRight().x, viewFrustum.getNearTopRight().y, viewFrustum.getNearTopRight().z);
        glVertex3f(viewFrustum.getFarTopRight().x, viewFrustum.getFarTopRight().y, viewFrustum.getFarTopRight().z);

        // LEFT PLANE IS BLUE
        // left plane - bottom edge - viewFrustum.getNear to distant
        glColor3f(0,0,1);
        glVertex3f(viewFrustum.getNearBottomLeft().x, viewFrustum.getNearBottomLeft().y, viewFrustum.getNearBottomLeft().z);
        glVertex3f(viewFrustum.getFarBottomLeft().x, viewFrustum.getFarBottomLeft().y, viewFrustum.getFarBottomLeft().z);

        // left plane - top edge - viewFrustum.getNear to distant
        glVertex3f(viewFrustum.getNearTopLeft().x, viewFrustum.getNearTopLeft().y, viewFrustum.getNearTopLeft().z);
        glVertex3f(viewFrustum.getFarTopLeft().x, viewFrustum.getFarTopLeft().y, viewFrustum.getFarTopLeft().z);

        // focal plane - bottom edge
        glColor3f(1.0f, 0.0f, 1.0f);
        float focalProportion = (viewFrustum.getFocalLength() - viewFrustum.getNearClip()) /
            (viewFrustum.getFarClip() - viewFrustum.getNearClip());
        glm::vec3 focalBottomLeft = glm::mix(viewFrustum.getNearBottomLeft(), viewFrustum.getFarBottomLeft(), focalProportion);
        glm::vec3 focalBottomRight = glm::mix(viewFrustum.getNearBottomRight(),
					      viewFrustum.getFarBottomRight(), focalProportion);
        glVertex3f(focalBottomLeft.x, focalBottomLeft.y, focalBottomLeft.z);
        glVertex3f(focalBottomRight.x, focalBottomRight.y, focalBottomRight.z);

        // focal plane - top edge
        glm::vec3 focalTopLeft = glm::mix(viewFrustum.getNearTopLeft(), viewFrustum.getFarTopLeft(), focalProportion);
        glm::vec3 focalTopRight = glm::mix(viewFrustum.getNearTopRight(), viewFrustum.getFarTopRight(), focalProportion);
        glVertex3f(focalTopLeft.x, focalTopLeft.y, focalTopLeft.z);
        glVertex3f(focalTopRight.x, focalTopRight.y, focalTopRight.z);

        // focal plane - left edge
        glVertex3f(focalBottomLeft.x, focalBottomLeft.y, focalBottomLeft.z);
        glVertex3f(focalTopLeft.x, focalTopLeft.y, focalTopLeft.z);

        // focal plane - right edge
        glVertex3f(focalBottomRight.x, focalBottomRight.y, focalBottomRight.z);
        glVertex3f(focalTopRight.x, focalTopRight.y, focalTopRight.z);
    }
    glEnd();
    glEnable(GL_LIGHTING);

    if (_menu->getFrustumDrawMode() == FRUSTUM_DRAW_MODE_ALL
        || _menu->getFrustumDrawMode() == FRUSTUM_DRAW_MODE_KEYHOLE) {
        // Draw the keyhole
        float keyholeRadius = viewFrustum.getKeyholeRadius();
        if (keyholeRadius > 0.0f) {
            glPushMatrix();
            glColor4f(1, 1, 0, 1);
            glTranslatef(position.x, position.y, position.z); // where we actually want it!
            glutWireSphere(keyholeRadius, 20, 20);
            glPopMatrix();
        }
    }
}

bool Application::maybeEditVoxelUnderCursor() {
    if (_menu->isOptionChecked(MenuOption::VoxelAddMode)
        || _menu->isOptionChecked(MenuOption::VoxelColorMode)) {
        if (_mouseVoxel.s != 0) {
            makeVoxel(glm::vec3(_mouseVoxel.x * TREE_SCALE,
				_mouseVoxel.y * TREE_SCALE,
				_mouseVoxel.z * TREE_SCALE),
                      _mouseVoxel.s * TREE_SCALE,
                      _mouseVoxel.red,
                      _mouseVoxel.green,
                      _mouseVoxel.blue,
                      _menu->isOptionChecked(MenuOption::DestructiveAddVoxel));

            // remember the position for drag detection
            _justEditedVoxel = true;

        }
    } else if (_menu->isOptionChecked(MenuOption::VoxelDeleteMode)) {
        deleteVoxelUnderCursor();
        VoxelFade fade(VoxelFade::FADE_OUT, 1.0f, 1.0f, 1.0f);
        const float VOXEL_BOUNDS_ADJUST = 0.01f;
        float slightlyBigger = _mouseVoxel.s * VOXEL_BOUNDS_ADJUST;
        fade.voxelDetails.x = _mouseVoxel.x - slightlyBigger;
        fade.voxelDetails.y = _mouseVoxel.y - slightlyBigger;
        fade.voxelDetails.z = _mouseVoxel.z - slightlyBigger;
        fade.voxelDetails.s = _mouseVoxel.s + slightlyBigger + slightlyBigger;
        _voxelFades.push_back(fade);

    } else if (_menu->isOptionChecked(MenuOption::VoxelGetColorMode)) {
        eyedropperVoxelUnderCursor();
    } else {
        return false;
    }
    return true;
}

void Application::deleteVoxelUnderCursor() {
    if (_mouseVoxel.s != 0) {
        // sending delete to the server is sufficient, server will send new version so we see updates soon enough
        _voxelEditSender.sendVoxelEditMessage(PacketTypeVoxelErase, _mouseVoxel);

        // delete it locally to see the effect immediately (and in case no voxel server is present)
        _voxels.deleteVoxelAt(_mouseVoxel.x, _mouseVoxel.y, _mouseVoxel.z, _mouseVoxel.s);

    }
    // remember the position for drag detection
    _justEditedVoxel = true;
}

void Application::eyedropperVoxelUnderCursor() {
    VoxelTreeElement* selectedNode = _voxels.getVoxelAt(_mouseVoxel.x, _mouseVoxel.y, _mouseVoxel.z, _mouseVoxel.s);
    if (selectedNode && selectedNode->isColored()) {
        QColor selectedColor(selectedNode->getColor()[RED_INDEX],
                             selectedNode->getColor()[GREEN_INDEX],
                             selectedNode->getColor()[BLUE_INDEX]);

        if (selectedColor.isValid()) {
            QAction* voxelPaintColorAction = _menu->getActionForOption(MenuOption::VoxelPaintColor);
            voxelPaintColorAction->setData(selectedColor);
            voxelPaintColorAction->setIcon(Swatch::createIcon(selectedColor));
        }
    }
}

void Application::resetSensors() {
    _mouseX = _glWidget->width() / 2;
    _mouseY = _glWidget->height() / 2;

    _faceshift.reset();

    if (OculusManager::isConnected()) {
        OculusManager::reset();
    }

    QCursor::setPos(_mouseX, _mouseY);
    _myAvatar->reset();

    QMetaObject::invokeMethod(&_audio, "reset", Qt::QueuedConnection);
}

static void setShortcutsEnabled(QWidget* widget, bool enabled) {
    foreach (QAction* action, widget->actions()) {
        QKeySequence shortcut = action->shortcut();
        if (!shortcut.isEmpty() && (shortcut[0] & (Qt::CTRL | Qt::ALT | Qt::META)) == 0) {
            // it's a shortcut that may coincide with a "regular" key, so switch its context
            action->setShortcutContext(enabled ? Qt::WindowShortcut : Qt::WidgetShortcut);
        }
    }
    foreach (QObject* child, widget->children()) {
        if (child->isWidgetType()) {
            setShortcutsEnabled(static_cast<QWidget*>(child), enabled);
        }
    }
}

void Application::setMenuShortcutsEnabled(bool enabled) {
    setShortcutsEnabled(_window->menuBar(), enabled);
}

void Application::updateWindowTitle(){
    
    QString buildVersion = " (build " + applicationVersion() + ")";
    NodeList* nodeList = NodeList::getInstance();
    
    QString title = QString() + _profile.getUsername() + " " + nodeList->getOwnerUUID().toString()
        + " @ " + nodeList->getDomainHostname() + buildVersion;

    qDebug("Application title set to: %s", title.toStdString().c_str());
    _window->setWindowTitle(title);
}

void Application::domainChanged(const QString& domainHostname) {
    // update the user's last domain in their Profile (which will propagate to data-server)
    _profile.updateDomain(domainHostname);

    updateWindowTitle();

    // reset the environment so that we don't erroneously end up with multiple
    _environment.resetToDefault();

    // reset our node to stats and node to jurisdiction maps... since these must be changing...
    _voxelServerJurisdictions.clear();
    _octreeServerSceneStats.clear();
    _particleServerJurisdictions.clear();
    
    // reset the particle renderer
    _particles.clear();
}

void Application::nodeKilled(SharedNodePointer node) {
    if (node->getType() == NodeType::VoxelServer) {
        QUuid nodeUUID = node->getUUID();
        // see if this is the first we've heard of this node...
        if (_voxelServerJurisdictions.find(nodeUUID) != _voxelServerJurisdictions.end()) {
            unsigned char* rootCode = _voxelServerJurisdictions[nodeUUID].getRootOctalCode();
            VoxelPositionSize rootDetails;
            voxelDetailsForCode(rootCode, rootDetails);

            printf("voxel server going away...... v[%f, %f, %f, %f]\n",
		   rootDetails.x, rootDetails.y, rootDetails.z, rootDetails.s);

            // Add the jurisditionDetails object to the list of "fade outs"
	    if (!_menu->isOptionChecked(MenuOption::DontFadeOnVoxelServerChanges)) {
                VoxelFade fade(VoxelFade::FADE_OUT, NODE_KILLED_RED, NODE_KILLED_GREEN, NODE_KILLED_BLUE);
                fade.voxelDetails = rootDetails;
                const float slightly_smaller = 0.99f;
                fade.voxelDetails.s = fade.voxelDetails.s * slightly_smaller;
                _voxelFades.push_back(fade);
	    }

	    // If the voxel server is going away, remove it from our jurisdiction map so we don't send voxels to a dead server
	    _voxelServerJurisdictions.erase(nodeUUID);
	}

	// also clean up scene stats for that server
	_voxelSceneStatsLock.lockForWrite();
	if (_octreeServerSceneStats.find(nodeUUID) != _octreeServerSceneStats.end()) {
	    _octreeServerSceneStats.erase(nodeUUID);
	}
	_voxelSceneStatsLock.unlock();

    } else if (node->getType() == NodeType::ParticleServer) {
	QUuid nodeUUID = node->getUUID();
	// see if this is the first we've heard of this node...
	if (_particleServerJurisdictions.find(nodeUUID) != _particleServerJurisdictions.end()) {
	    unsigned char* rootCode = _particleServerJurisdictions[nodeUUID].getRootOctalCode();
	    VoxelPositionSize rootDetails;
	    voxelDetailsForCode(rootCode, rootDetails);

	    printf("particle server going away...... v[%f, %f, %f, %f]\n",
		   rootDetails.x, rootDetails.y, rootDetails.z, rootDetails.s);

	    // Add the jurisditionDetails object to the list of "fade outs"
	    if (!_menu->isOptionChecked(MenuOption::DontFadeOnVoxelServerChanges)) {
		VoxelFade fade(VoxelFade::FADE_OUT, NODE_KILLED_RED, NODE_KILLED_GREEN, NODE_KILLED_BLUE);
		fade.voxelDetails = rootDetails;
		const float slightly_smaller = 0.99f;
		fade.voxelDetails.s = fade.voxelDetails.s * slightly_smaller;
		_voxelFades.push_back(fade);
	    }

	    // If the voxel server is going away, remove it from our jurisdiction map so we don't send voxels to a dead server
	    _particleServerJurisdictions.erase(nodeUUID);
	}

	// also clean up scene stats for that server
	_voxelSceneStatsLock.lockForWrite();
	if (_octreeServerSceneStats.find(nodeUUID) != _octreeServerSceneStats.end()) {
	    _octreeServerSceneStats.erase(nodeUUID);
	}
	_voxelSceneStatsLock.unlock();

    } else if (node->getType() == NodeType::AvatarMixer) {
	// our avatar mixer has gone away - clear the hash of avatars
	_avatarManager.clearOtherAvatars();
    }
}

void Application::trackIncomingVoxelPacket(const QByteArray& packet, const HifiSockAddr& senderSockAddr, bool wasStatsPacket) {

    // Attempt to identify the sender from it's address.
    SharedNodePointer serverNode = NodeList::getInstance()->nodeWithAddress(senderSockAddr);
    if (serverNode) {
        QUuid nodeUUID = serverNode->getUUID();

        // now that we know the node ID, let's add these stats to the stats for that node...
        _voxelSceneStatsLock.lockForWrite();
        if (_octreeServerSceneStats.find(nodeUUID) != _octreeServerSceneStats.end()) {
            VoxelSceneStats& stats = _octreeServerSceneStats[nodeUUID];
            stats.trackIncomingOctreePacket(packet, wasStatsPacket, serverNode->getClockSkewUsec());
        }
        _voxelSceneStatsLock.unlock();
    }
}

int Application::parseOctreeStats(const QByteArray& packet, const HifiSockAddr& senderSockAddr) {

    // But, also identify the sender, and keep track of the contained jurisdiction root for this server
    SharedNodePointer server = NodeList::getInstance()->nodeWithAddress(senderSockAddr);

    // parse the incoming stats datas stick it in a temporary object for now, while we
    // determine which server it belongs to
    VoxelSceneStats temp;
    int statsMessageLength = temp.unpackFromMessage(reinterpret_cast<const unsigned char*>(packet.data()), packet.size());

    // quick fix for crash... why would voxelServer be NULL?
    if (server) {
        QUuid nodeUUID = server->getUUID();

        // now that we know the node ID, let's add these stats to the stats for that node...
        _voxelSceneStatsLock.lockForWrite();
        if (_octreeServerSceneStats.find(nodeUUID) != _octreeServerSceneStats.end()) {
            _octreeServerSceneStats[nodeUUID].unpackFromMessage(reinterpret_cast<const unsigned char*>(packet.data()),
                                                                packet.size());
        } else {
            _octreeServerSceneStats[nodeUUID] = temp;
        }
        _voxelSceneStatsLock.unlock();

        VoxelPositionSize rootDetails;
        voxelDetailsForCode(temp.getJurisdictionRoot(), rootDetails);

        // see if this is the first we've heard of this node...
        NodeToJurisdictionMap* jurisdiction = NULL;
        if (server->getType() == NodeType::VoxelServer) {
            jurisdiction = &_voxelServerJurisdictions;
        } else {
            jurisdiction = &_particleServerJurisdictions;
        }


        if (jurisdiction->find(nodeUUID) == jurisdiction->end()) {
            printf("stats from new server... v[%f, %f, %f, %f]\n",
		   rootDetails.x, rootDetails.y, rootDetails.z, rootDetails.s);

            // Add the jurisditionDetails object to the list of "fade outs"
	    if (!_menu->isOptionChecked(MenuOption::DontFadeOnVoxelServerChanges)) {
                VoxelFade fade(VoxelFade::FADE_OUT, NODE_ADDED_RED, NODE_ADDED_GREEN, NODE_ADDED_BLUE);
                fade.voxelDetails = rootDetails;
                const float slightly_smaller = 0.99f;
                fade.voxelDetails.s = fade.voxelDetails.s * slightly_smaller;
                _voxelFades.push_back(fade);
	    }
	}
	// store jurisdiction details for later use
	// This is bit of fiddling is because JurisdictionMap assumes it is the owner of the values used to construct it
	// but VoxelSceneStats thinks it's just returning a reference to it's contents. So we need to make a copy of the
	// details from the VoxelSceneStats to construct the JurisdictionMap
	JurisdictionMap jurisdictionMap;
	jurisdictionMap.copyContents(temp.getJurisdictionRoot(), temp.getJurisdictionEndNodes());
	(*jurisdiction)[nodeUUID] = jurisdictionMap;
    }
    return statsMessageLength;
}

void Application::packetSent(quint64 length) {
    _bandwidthMeter.outputStream(BandwidthMeter::VOXELS).updateValue(length);
}

void Application::loadScripts() {
    // loads all saved scripts
    QSettings* settings = new QSettings(this);
    int size = settings->beginReadArray("Settings");
    
    for (int i = 0; i < size; ++i){
        settings->setArrayIndex(i);
        QString string = settings->value("script").toString();
        loadScript(string);
    }
    
    settings->endArray();
}

void Application::saveScripts() {
    // saves all current running scripts
    QSettings* settings = new QSettings(this);
    settings->beginWriteArray("Settings");
    for (int i = 0; i < _activeScripts.size(); ++i){
        settings->setArrayIndex(i);
        settings->setValue("script", _activeScripts.at(i));
    }
    
    settings->endArray();
}

void Application::removeScriptName(const QString& fileNameString) {
    _activeScripts.removeOne(fileNameString);
}

void Application::loadScript(const QString& fileNameString) {
    _activeScripts.append(fileNameString);
    QByteArray fileNameAscii = fileNameString.toLocal8Bit();
    const char* fileName = fileNameAscii.data();

    std::ifstream file(fileName, std::ios::in|std::ios::binary|std::ios::ate);
    if(!file.is_open()) {
        qDebug("Error loading file %s", fileName);
        return;
    }
    qDebug("Loading file %s...", fileName);

    // get file length....
    unsigned long fileLength = file.tellg();
    file.seekg( 0, std::ios::beg );

    // read the entire file into a buffer, WHAT!? Why not.
    char* entireFile = new char[fileLength+1];
    file.read((char*)entireFile, fileLength);
    file.close();

    entireFile[fileLength] = 0;// null terminate
    QString script(entireFile);
    delete[] entireFile;

    // start the script on a new thread...
    bool wantMenuItems = true; // tells the ScriptEngine object to add menu items for itself

    ScriptEngine* scriptEngine = new ScriptEngine(script, wantMenuItems, fileName, _menu,
                                                  &_controllerScriptingInterface);
    scriptEngine->setupMenuItems();

    // setup the packet senders and jurisdiction listeners of the script engine's scripting interfaces so
    // we can use the same ones from the application.
    scriptEngine->getVoxelsScriptingInterface()->setPacketSender(&_voxelEditSender);
    scriptEngine->getParticlesScriptingInterface()->setPacketSender(&_particleEditSender);
    scriptEngine->getParticlesScriptingInterface()->setParticleTree(_particles.getTree());
    
    // hook our avatar object into this script engine
    scriptEngine->setAvatarData( static_cast<Avatar*>(_myAvatar), "MyAvatar");

    QThread* workerThread = new QThread(this);

    // when the worker thread is started, call our engine's run..
    connect(workerThread, &QThread::started, scriptEngine, &ScriptEngine::run);

    // when the thread is terminated, add both scriptEngine and thread to the deleteLater queue
    connect(scriptEngine, SIGNAL(finished(const QString&)), scriptEngine, SLOT(deleteLater()));
    connect(workerThread, SIGNAL(finished()), workerThread, SLOT(deleteLater()));
    connect(scriptEngine, SIGNAL(finished(const QString&)), this, SLOT(removeScriptName(const QString&)));

    // when the application is about to quit, stop our script engine so it unwinds properly
    connect(this, SIGNAL(aboutToQuit()), scriptEngine, SLOT(stop()));

    scriptEngine->moveToThread(workerThread);

    // Starts an event loop, and emits workerThread->started()
    workerThread->start();

    // restore the main window's active state
    _window->activateWindow();
}

void Application::loadDialog() {
    // shut down and stop any existing script
    QString desktopLocation = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
    QString suggestedName = desktopLocation.append("/script.js");

    QString fileNameString = QFileDialog::getOpenFileName(_glWidget, tr("Open Script"), suggestedName, 
                                                          tr("JavaScript Files (*.js)"));
    
    loadScript(fileNameString);
}

void Application::toggleLogDialog() {
    if (! _logDialog) {
        _logDialog = new LogDialog(_glWidget, getLogger());
        _logDialog->show();
    } else {
        _logDialog->close();
    }
}

void Application::initAvatarAndViewFrustum() {
    updateMyAvatar(0.f);
}

void Application::checkVersion() {
    QNetworkRequest latestVersionRequest((QUrl(CHECK_VERSION_URL)));
    latestVersionRequest.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::PreferCache);
    connect(Application::getInstance()->getNetworkAccessManager()->get(latestVersionRequest), SIGNAL(finished()), SLOT(parseVersionXml()));
}

void Application::parseVersionXml() {
    
#ifdef Q_OS_WIN32
    QString operatingSystem("win");
#endif
    
#ifdef Q_OS_MAC
    QString operatingSystem("mac");
#endif
    
#ifdef Q_OS_LINUX
    QString operatingSystem("ubuntu");
#endif
    
    QString releaseDate;
    QString releaseNotes;
    QString latestVersion;
    QUrl downloadUrl;
    QObject* sender = QObject::sender();
    
    QXmlStreamReader xml(qobject_cast<QNetworkReply*>(sender));
    while (!xml.atEnd() && !xml.hasError()) {
        QXmlStreamReader::TokenType token = xml.readNext();
        
        if (token == QXmlStreamReader::StartElement) {
            if (xml.name() == "ReleaseDate") {
                xml.readNext();
                releaseDate = xml.text().toString();
            }
            if (xml.name() == "ReleaseNotes") {
                xml.readNext();
                releaseNotes = xml.text().toString();
            }
            if (xml.name() == "Version") {
                xml.readNext();
                latestVersion = xml.text().toString();
            }
            if (xml.name() == operatingSystem) {
                xml.readNext();
                downloadUrl = QUrl(xml.text().toString());
            }
        }
    }
    if (!shouldSkipVersion(latestVersion) && applicationVersion() != latestVersion) {
        new UpdateDialog(_glWidget, releaseNotes, latestVersion, downloadUrl);
    }
    sender->deleteLater();
}

bool Application::shouldSkipVersion(QString latestVersion) {
    QFile skipFile(SKIP_FILENAME);
    skipFile.open(QIODevice::ReadWrite);
    QString skipVersion(skipFile.readAll());
    return (skipVersion == latestVersion || applicationVersion() == "dev");
}

void Application::skipVersion(QString latestVersion) {
    QFile skipFile(SKIP_FILENAME);
    skipFile.open(QIODevice::WriteOnly | QIODevice::Truncate);
    skipFile.seek(0);
    skipFile.write(latestVersion.toStdString().c_str());
}

void Application::takeSnapshot() {
    switchToResourcesParentIfRequired();
    QMediaPlayer* player = new QMediaPlayer();
    QFileInfo inf = QFileInfo("resources/sounds/snap.wav");
    player->setMedia(QUrl::fromLocalFile(inf.absoluteFilePath()));
    player->play();

    Snapshot::saveSnapshot(_glWidget, _profile.getUsername(), _myAvatar->getPosition());
}

