//
//  Application.h
//  interface
//
//  Created by Andrzej Kapolka on 5/10/13.
//  Copyright (c) 2013 High Fidelity, Inc. All rights reserved.
//

#ifndef __interface__Application__
#define __interface__Application__

#include <map>
#include <time.h>

#include <QApplication>
#include <QAction>
#include <QSettings>
#include <QTouchEvent>
#include <QList>
#include <QStringList>
#include <QPointer>

#include <NetworkPacket.h>
#include <NodeList.h>
#include <PacketHeaders.h>
#include <ParticleCollisionSystem.h>
#include <ParticleEditPacketSender.h>
#include <ScriptEngine.h>
#include <VoxelQuery.h>

#include "Audio.h"

#include "BandwidthMeter.h"
#include "Camera.h"
#include "Cloud.h"
#include "DatagramProcessor.h"
#include "Environment.h"
#include "GLCanvas.h"
#include "MetavoxelSystem.h"
#include "PacketHeaders.h"
#include "PieMenu.h"
#include "Menu.h"
#include "Stars.h"
#include "Swatch.h"
#include "ToolsPalette.h"
#include "ViewFrustum.h"
#include "VoxelFade.h"
#include "VoxelEditPacketSender.h"
#include "VoxelHideShowThread.h"
#include "VoxelPacketProcessor.h"
#include "VoxelSystem.h"
#include "VoxelImporter.h"
#include "avatar/Avatar.h"
#include "avatar/AvatarManager.h"
#include "avatar/MyAvatar.h"
#include "avatar/Profile.h"
#include "devices/Faceshift.h"
#include "devices/SixenseManager.h"
#include "renderer/AmbientOcclusionEffect.h"
#include "renderer/GeometryCache.h"
#include "renderer/GlowEffect.h"
#include "renderer/PointShader.h"
#include "renderer/TextureCache.h"
#include "renderer/VoxelShader.h"
#include "ui/BandwidthDialog.h"
#include "ui/ChatEntry.h"
#include "ui/VoxelStatsDialog.h"
#include "ui/RearMirrorTools.h"
#include "ui/LodToolsDialog.h"
#include "ui/LogDialog.h"
#include "ui/UpdateDialog.h"
#include "FileLogger.h"
#include "ParticleTreeRenderer.h"
#include "ControllerScriptingInterface.h"


class QAction;
class QActionGroup;
class QGLWidget;
class QKeyEvent;
class QMainWindow;
class QMouseEvent;
class QNetworkAccessManager;
class QSettings;
class QWheelEvent;
class Menu;
class Node;
class ProgramObject;

static const float NODE_ADDED_RED   = 0.0f;
static const float NODE_ADDED_GREEN = 1.0f;
static const float NODE_ADDED_BLUE  = 0.0f;
static const float NODE_KILLED_RED   = 1.0f;
static const float NODE_KILLED_GREEN = 0.0f;
static const float NODE_KILLED_BLUE  = 0.0f;

class Application : public QApplication {
    Q_OBJECT

    friend class VoxelPacketProcessor;
    friend class VoxelEditPacketSender;
    friend class DatagramProcessor;

public:
    static Application* getInstance() { return static_cast<Application*>(QCoreApplication::instance()); }

    Application(int& argc, char** argv, timeval &startup_time);
    ~Application();

    void restoreSizeAndPosition();
    void loadScript(const QString& fileNameString);    
    void loadScripts();
    void storeSizeAndPosition();
    void saveScripts();
    void initializeGL();
    void paintGL();
    void resizeGL(int width, int height);

    void keyPressEvent(QKeyEvent* event);
    void keyReleaseEvent(QKeyEvent* event);

    void mouseMoveEvent(QMouseEvent* event);
    void mousePressEvent(QMouseEvent* event);
    void mouseReleaseEvent(QMouseEvent* event);

    void touchBeginEvent(QTouchEvent* event);
    void touchEndEvent(QTouchEvent* event);
    void touchUpdateEvent(QTouchEvent* event);

    void updateWindowTitle();

    void wheelEvent(QWheelEvent* event);

    void makeVoxel(glm::vec3 position,
                   float scale,
                   unsigned char red,
                   unsigned char green,
                   unsigned char blue,
                   bool isDestructive);

    void removeVoxel(glm::vec3 position, float scale);

    glm::vec3 getMouseVoxelWorldCoordinates(const VoxelDetail& mouseVoxel);

    QGLWidget* getGLWidget() { return _glWidget; }
    MyAvatar* getAvatar() { return _myAvatar; }
    Audio* getAudio() { return &_audio; }
    Camera* getCamera() { return &_myCamera; }
    ViewFrustum* getViewFrustum() { return &_viewFrustum; }
    VoxelSystem* getVoxels() { return &_voxels; }
    ParticleTreeRenderer* getParticles() { return &_particles; }
    MetavoxelSystem* getMetavoxels() { return &_metavoxels; }
    VoxelSystem* getSharedVoxelSystem() { return &_sharedVoxelSystem; }
    VoxelTree* getClipboard() { return &_clipboard; }
    Environment* getEnvironment() { return &_environment; }
    bool isMousePressed() const { return _mousePressed; }
    bool isMouseHidden() const { return _mouseHidden; }
    const glm::vec3& getMouseRayOrigin() const { return _mouseRayOrigin; }
    const glm::vec3& getMouseRayDirection() const { return _mouseRayDirection; }
    Faceshift* getFaceshift() { return &_faceshift; }
    SixenseManager* getSixenseManager() { return &_sixenseManager; }
    BandwidthMeter* getBandwidthMeter() { return &_bandwidthMeter; }
    QSettings* getSettings() { return _settings; }
    Swatch*  getSwatch() { return &_swatch; }
    QMainWindow* getWindow() { return _window; }
    NodeToVoxelSceneStats* getOcteeSceneStats() { return &_octreeServerSceneStats; }
    void lockVoxelSceneStats() { _voxelSceneStatsLock.lockForRead(); }
    void unlockVoxelSceneStats() { _voxelSceneStatsLock.unlock(); }

    QNetworkAccessManager* getNetworkAccessManager() { return _networkAccessManager; }
    GeometryCache* getGeometryCache() { return &_geometryCache; }
    TextureCache* getTextureCache() { return &_textureCache; }
    GlowEffect* getGlowEffect() { return &_glowEffect; }

    AvatarManager& getAvatarManager() { return _avatarManager; }
    Profile* getProfile() { return &_profile; }
    void resetProfile(const QString& username);

    void controlledBroadcastToNodes(const QByteArray& packet, const NodeSet& destinationNodeTypes);

    void setupWorldLight();

    void displaySide(Camera& whichCamera, bool selfAvatarOnly = false);

    /// Loads a view matrix that incorporates the specified model translation without the precision issues that can
    /// result from matrix multiplication at high translation magnitudes.
    void loadTranslatedViewMatrix(const glm::vec3& translation);

    const glm::mat4& getShadowMatrix() const { return _shadowMatrix; }

    /// Computes the off-axis frustum parameters for the view frustum, taking mirroring into account.
    void computeOffAxisFrustum(float& left, float& right, float& bottom, float& top, float& nearVal,
        float& farVal, glm::vec4& nearClipPlane, glm::vec4& farClipPlane) const;

    VoxelShader& getVoxelShader() { return _voxelShader; }
    PointShader& getPointShader() { return _pointShader; }
    FileLogger* getLogger() { return _logger; }
    Menu* getMenu() {return _menu;}

    glm::vec2 getViewportDimensions() const{ return glm::vec2(_glWidget->width(),_glWidget->height()); }
    NodeToJurisdictionMap& getVoxelServerJurisdictions() { return _voxelServerJurisdictions; }
    NodeToJurisdictionMap& getParticleServerJurisdictions() { return _particleServerJurisdictions; }
    void pasteVoxelsToOctalCode(const unsigned char* octalCodeDestination);

    /// set a voxel which is to be rendered with a highlight
    void setHighlightVoxel(const VoxelDetail& highlightVoxel) { _highlightVoxel = highlightVoxel; }
    void setIsHighlightVoxel(bool isHighlightVoxel) { _isHighlightVoxel = isHighlightVoxel; }
    
    void skipVersion(QString latestVersion);

    bool getPipelineWarningsOption();

signals:

    /// Fired when we're rendering in-world interface elements; allows external parties to hook in.
    void renderingInWorldInterface();
    
public slots:
    void domainChanged(const QString& domainHostname);
    void nodeKilled(SharedNodePointer node);
    void packetSent(quint64 length);
    
    void exportVoxels();
    void importVoxels();
    void cutVoxels();
    void copyVoxels();
    void pasteVoxels();
    void nudgeVoxels();
    void deleteVoxels();

    void setRenderVoxels(bool renderVoxels);
    void doKillLocalVoxels();
    void decreaseVoxelSize();
    void increaseVoxelSize();
    void loadDialog();
    void toggleLogDialog();
    void initAvatarAndViewFrustum();

private slots:

    void timer();
    void idle();

    void setFullscreen(bool fullscreen);
    void setEnable3DTVMode(bool enable3DTVMode);
    
    void renderThrustAtVoxel(const glm::vec3& thrust);

    void renderCoverageMap();
    void renderCoverageMapsRecursively(CoverageMap* map);

    void renderCoverageMapV2();
    void renderCoverageMapsV2Recursively(CoverageMapV2* map);

    glm::vec2 getScaledScreenPoint(glm::vec2 projectedPoint);

    void closeMirrorView();
    void restoreMirrorView();
    void shrinkMirrorView();
    void resetSensors();
    
    void parseVersionXml();

    void removeScriptName(const QString& fileNameString);

private:
    void resetCamerasOnResizeGL(Camera& camera, int width, int height);
    void updateProjectionMatrix();
    void updateProjectionMatrix(Camera& camera, bool updateViewFrustum = true);

    static bool sendVoxelsOperation(OctreeElement* node, void* extraData);
    void sendPingPackets();

    void initDisplay();
    void init();

    void update(float deltaTime);

    // Various helper functions called during update()
    void updateMouseRay();
    void updateFaceshift();
    void updateMyAvatarLookAtPosition(glm::vec3& lookAtSpot);
    void updateHoverVoxels(float deltaTime, float& distance, BoxFace& face);
    void updateMouseVoxels(float deltaTime, float& distance, BoxFace& face);
    void updateHandAndTouch(float deltaTime);
    void updateLeap(float deltaTime);
    void updateSixense(float deltaTime);
    void updateSerialDevices(float deltaTime);
    void updateThreads(float deltaTime);
    void updateParticles(float deltaTime);
    void updateMetavoxels(float deltaTime);
    void updateCamera(float deltaTime);
    void updateDialogs(float deltaTime);
    void updateAudio(float deltaTime);
    void updateCursor(float deltaTime);

    Avatar* findLookatTargetAvatar(glm::vec3& eyePosition, QUuid &nodeUUID);
    bool isLookingAtMyAvatar(Avatar* avatar);

    void renderLookatIndicator(glm::vec3 pointOfInterest);
    void renderHighlightVoxel(VoxelDetail voxel);

    void updateMyAvatar(float deltaTime);
    void queryOctree(NodeType_t serverType, PacketType packetType, NodeToJurisdictionMap& jurisdictions);
    void loadViewFrustum(Camera& camera, ViewFrustum& viewFrustum);

    glm::vec3 getSunDirection();

    void updateShadowMap();
    void displayOverlay();
    void displayStatsBackground(unsigned int rgba, int x, int y, int width, int height);
    void displayStats();
    void checkStatsClick();
    void toggleStatsExpanded();
    void renderAvatars(bool forceRenderHead, bool selfAvatarOnly = false);
    void renderViewFrustum(ViewFrustum& viewFrustum);

    void checkBandwidthMeterClick();

    bool maybeEditVoxelUnderCursor();
    void deleteVoxelUnderCursor();
    void eyedropperVoxelUnderCursor();

    void setMenuShortcutsEnabled(bool enabled);

    static void attachNewHeadToNode(Node *newNode);
    static void* networkReceive(void* args); // network receive thread

    void findAxisAlignment();

    void displayRearMirrorTools();

    QMainWindow* _window;
    QGLWidget* _glWidget;

    bool _statsExpanded;
    BandwidthMeter _bandwidthMeter;
    
    QThread* _nodeThread;
    DatagramProcessor _datagramProcessor;

    QNetworkAccessManager* _networkAccessManager;
    QSettings* _settings;

    glm::vec3 _gravity;

    // Frame Rate Measurement
    int _frameCount;
    float _fps;
    timeval _applicationStartupTime;
    timeval _timerStart, _timerEnd;
    timeval _lastTimeUpdated;
    bool _justStarted;

    Stars _stars;

    Cloud _cloud;

    VoxelSystem _voxels;
    VoxelTree _clipboard; // if I copy/paste
    VoxelImporter* _voxelImporter;
    VoxelSystem _sharedVoxelSystem;
    ViewFrustum _sharedVoxelSystemViewFrustum;

    ParticleTreeRenderer _particles;
    ParticleCollisionSystem _particleCollisionSystem;

    QByteArray _voxelsFilename;
    bool _wantToKillLocalVoxels;

    MetavoxelSystem _metavoxels;

    ViewFrustum _viewFrustum; // current state of view frustum, perspective, orientation, etc.

    Oscilloscope _audioScope;

    VoxelQuery _voxelQuery; // NodeData derived class for querying voxels from voxel server

    AvatarManager _avatarManager;
    MyAvatar* _myAvatar;            // TODO: move this and relevant code to AvatarManager (or MyAvatar as the case may be)
    Profile _profile;               // The data-server linked profile for this user

    Faceshift _faceshift;

    SixenseManager _sixenseManager;
    QStringList _activeScripts;
    Menu* _menu;

    Camera _myCamera;                  // My view onto the world
    Camera _viewFrustumOffsetCamera;   // The camera we use to sometimes show the view frustum from an offset mode
    Camera _mirrorCamera;              // Cammera for mirror view
    QRect _mirrorViewRect;
    RearMirrorTools* _rearMirrorTools;

    glm::mat4 _untranslatedViewMatrix;
    glm::vec3 _viewMatrixTranslation;

    glm::mat4 _shadowMatrix;

    Environment _environment;

    int _mouseX;
    int _mouseY;
    int _mouseDragStartedX;
    int _mouseDragStartedY;
    quint64 _lastMouseMove;
    bool _mouseHidden;
    bool _seenMouseMove;

    glm::vec3 _mouseRayOrigin;
    glm::vec3 _mouseRayDirection;

    float _touchAvgX;
    float _touchAvgY;
    float _lastTouchAvgX;
    float _lastTouchAvgY;
    float _touchDragStartedAvgX;
    float _touchDragStartedAvgY;
    bool _isTouchPressed; //  true if multitouch has been pressed (clear when finished)

    VoxelDetail _mouseVoxelDragging;
    bool _mousePressed; //  true if mouse has been pressed (clear when finished)

    VoxelDetail _hoverVoxel;      // Stuff about the voxel I am hovering or clicking
    bool _isHoverVoxel;
    bool _isHoverVoxelSounding;
    nodeColor _hoverVoxelOriginalColor;

    VoxelDetail _mouseVoxel;      // details of the voxel to be edited
    float _mouseVoxelScale;       // the scale for adding/removing voxels
    bool _mouseVoxelScaleInitialized;
    glm::vec3 _lastMouseVoxelPos; // the position of the last mouse voxel edit
    bool _justEditedVoxel;        // set when we've just added/deleted/colored a voxel

    VoxelDetail _highlightVoxel;
    bool _isHighlightVoxel;

    VoxelDetail _nudgeVoxel; // details of the voxel to be nudged
    bool _nudgeStarted;
    bool _lookingAlongX;
    bool _lookingAwayFromOrigin;
    glm::vec3 _nudgeGuidePosition;

    ChatEntry _chatEntry; // chat entry field
    bool _chatEntryOn;    // Whether to show the chat entry

    GeometryCache _geometryCache;
    TextureCache _textureCache;

    GlowEffect _glowEffect;
    AmbientOcclusionEffect _ambientOcclusionEffect;
    VoxelShader _voxelShader;
    PointShader _pointShader;

    Audio _audio;

    bool _enableProcessVoxelsThread;
    VoxelPacketProcessor _voxelProcessor;
    VoxelHideShowThread _voxelHideShowThread;
    VoxelEditPacketSender _voxelEditSender;
    ParticleEditPacketSender _particleEditSender;

    int _packetsPerSecond;
    int _bytesPerSecond;

    int _recentMaxPackets; // recent max incoming voxel packets to process
    bool _resetRecentMaxPacketsSoon;

    StDev _idleLoopStdev;
    float _idleLoopMeasuredJitter;

    ToolsPalette _palette;
    Swatch _swatch;

    bool _pasteMode;

    PieMenu _pieMenu;

    int parseOctreeStats(const QByteArray& packet, const HifiSockAddr& senderAddress);
    void trackIncomingVoxelPacket(const QByteArray& packet, const HifiSockAddr& senderSockAddr, bool wasStatsPacket);

    NodeToJurisdictionMap _voxelServerJurisdictions;
    NodeToJurisdictionMap _particleServerJurisdictions;
    NodeToVoxelSceneStats _octreeServerSceneStats;
    QReadWriteLock _voxelSceneStatsLock;

    std::vector<VoxelFade> _voxelFades;
    ControllerScriptingInterface _controllerScriptingInterface;
    QPointer<LogDialog> _logDialog;

    FileLogger* _logger;

    void checkVersion();
    void displayUpdateDialog();
    bool shouldSkipVersion(QString latestVersion);
    void takeSnapshot();
};

#endif /* defined(__interface__Application__) */
