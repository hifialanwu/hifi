//
//  VoxelStatsDialog.cpp
//  interface
//
//  Created by Brad Hefta-Gaub on 7/19/13.
//  Copyright (c) 2013 High Fidelity, Inc. All rights reserved.
//

#include <sstream>

#include <QFormLayout>
#include <QDialogButtonBox>

#include <QPalette>
#include <QColor>

#include <VoxelSceneStats.h>

#include "Application.h"

#include "ui/VoxelStatsDialog.h"

VoxelStatsDialog::VoxelStatsDialog(QWidget* parent, NodeToVoxelSceneStats* model) :
    QDialog(parent, Qt::Window | Qt::WindowCloseButtonHint | Qt::WindowStaysOnTopHint),
    _model(model) {
    
    _statCount = 0;
    _voxelServerLabelsCount = 0;

    for (int i = 0; i < MAX_VOXEL_SERVERS; i++) {
        _voxelServerLables[i] = 0;
        _extraServerDetails[i] = LESS;
    }

    for (int i = 0; i < MAX_STATS; i++) {
        _labels[i] = NULL;
    }

    this->setWindowTitle("Octree Server Statistics");

    // Create layouter
    _form = new QFormLayout();
    this->QDialog::setLayout(_form);

    // Setup stat items
    _serverVoxels = AddStatItem("Elements on Servers");
    _localVoxels = AddStatItem("Local Elements");
    _localVoxelsMemory = AddStatItem("Elements Memory");
    _voxelsRendered = AddStatItem("Voxels Rendered");
    _sendingMode = AddStatItem("Sending Mode");
    
    layout()->setSizeConstraint(QLayout::SetFixedSize); 
}

void VoxelStatsDialog::RemoveStatItem(int item) {
    QLabel* myLabel = _labels[item];
    QWidget* automaticLabel = _form->labelForField(myLabel);
    _form->removeWidget(myLabel);
    _form->removeWidget(automaticLabel);
    automaticLabel->deleteLater();
    myLabel->deleteLater();
    _labels[item] = NULL;
}

void VoxelStatsDialog::moreless(const QString& link) {
    QStringList linkDetails = link.split("-");
    const int COMMAND_ITEM = 0;
    const int SERVER_NUMBER_ITEM = 1;
    QString serverNumberString = linkDetails[SERVER_NUMBER_ITEM];
    QString command = linkDetails[COMMAND_ITEM];
    int serverNumber = serverNumberString.toInt();
    
    if (command == "more") {
        _extraServerDetails[serverNumber-1] = MORE;
    } else if (command == "most") {
        _extraServerDetails[serverNumber-1] = MOST;
    } else {
        _extraServerDetails[serverNumber-1] = LESS;
    }
}


int VoxelStatsDialog::AddStatItem(const char* caption, unsigned colorRGBA) {
    char strBuf[64];
    const int STATS_LABEL_WIDTH = 600;
    
    _statCount++; // increment our current stat count
    
    if (colorRGBA == 0) {
        static unsigned rotatingColors[] = { GREENISH, YELLOWISH, GREYISH };
        colorRGBA = rotatingColors[_statCount % (sizeof(rotatingColors)/sizeof(rotatingColors[0]))];
    }
    QLabel* label = _labels[_statCount] = new QLabel();  

    // Set foreground color to 62.5% brightness of the meter (otherwise will be hard to read on the bright background)
    QPalette palette = label->palette();
    
    // This goofiness came from the bandwidth meter code, it basically stores a color in an unsigned and extracts it
    unsigned rgb = colorRGBA >> 8;
    const unsigned colorpart1 = 0xfefefeu;
    const unsigned colorpart2 = 0xf8f8f8;
    rgb = ((rgb & colorpart1) >> 1) + ((rgb & colorpart2) >> 3);
    palette.setColor(QPalette::WindowText, QColor::fromRgb(rgb));
    label->setPalette(palette);
    snprintf(strBuf, sizeof(strBuf), " %s:", caption);
    _form->addRow(strBuf, label);
    label->setFixedWidth(STATS_LABEL_WIDTH);
    
    return _statCount;
}

VoxelStatsDialog::~VoxelStatsDialog() {
    for (int i = 0; i < _statCount; i++) {
        delete _labels[i];
    }
}

void VoxelStatsDialog::paintEvent(QPaintEvent* event) {

    // Update labels

    VoxelSystem* voxels = Application::getInstance()->getVoxels();
    QLabel* label;
    QLocale locale(QLocale::English);
    std::stringstream statsValue;
    statsValue.precision(4);

    // Voxels Rendered    
    label = _labels[_voxelsRendered];
    statsValue << "Max: " << voxels->getMaxVoxels() / 1000.f << "K " << 
        "Drawn: " << voxels->getVoxelsWritten() / 1000.f << "K " <<
        "Abandoned: " << voxels->getAbandonedVoxels() / 1000.f << "K " <<
        "ReadBuffer: " << voxels->getVoxelsRendered() / 1000.f << "K " <<
        "Changed: " << voxels->getVoxelsUpdated() / 1000.f << "K ";
    label->setText(statsValue.str().c_str());

    // Voxels Memory Usage
    label = _labels[_localVoxelsMemory];
    statsValue.str("");
    statsValue << 
        "Elements RAM: " << OctreeElement::getTotalMemoryUsage() / 1000000.f << "MB "
        "Geometry RAM: " << voxels->getVoxelMemoryUsageRAM() / 1000000.f << "MB " <<
        "VBO: " << voxels->getVoxelMemoryUsageVBO() / 1000000.f << "MB ";
    if (voxels->hasVoxelMemoryUsageGPU()) {
        statsValue << "GPU: " << voxels->getVoxelMemoryUsageGPU() / 1000000.f << "MB ";
    }
    label->setText(statsValue.str().c_str());

    // Local Voxels
    label = _labels[_localVoxels];
    unsigned long localTotal = OctreeElement::getNodeCount();
    unsigned long localInternal = OctreeElement::getInternalNodeCount();
    unsigned long localLeaves = OctreeElement::getLeafNodeCount();
    QString localTotalString = locale.toString((uint)localTotal); // consider adding: .rightJustified(10, ' ');
    QString localInternalString = locale.toString((uint)localInternal);
    QString localLeavesString = locale.toString((uint)localLeaves);

    statsValue.str("");
    statsValue << 
        "Total: " << localTotalString.toLocal8Bit().constData() << " / " <<
        "Internal: " << localInternalString.toLocal8Bit().constData() << " / " <<
        "Leaves: " << localLeavesString.toLocal8Bit().constData() << "";
    label->setText(statsValue.str().c_str());

    // iterate all the current voxel stats, and list their sending modes, total their voxels, etc...
    std::stringstream sendingMode("");

    int serverCount = 0;
    int movingServerCount = 0;
    unsigned long totalNodes = 0;
    unsigned long totalInternal = 0;
    unsigned long totalLeaves = 0;

    Application::getInstance()->lockVoxelSceneStats();
    NodeToVoxelSceneStats* sceneStats = Application::getInstance()->getOcteeSceneStats();
    for(NodeToVoxelSceneStatsIterator i = sceneStats->begin(); i != sceneStats->end(); i++) {
        //const QUuid& uuid = i->first;
        VoxelSceneStats& stats = i->second;
        serverCount++;

        // calculate server node totals
        totalNodes += stats.getTotalElements();
        totalInternal += stats.getTotalInternal();
        totalLeaves += stats.getTotalLeaves();
    
        // Sending mode
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
    Application::getInstance()->unlockVoxelSceneStats();
    sendingMode << " - " << serverCount << " servers";
    if (movingServerCount > 0) {
        sendingMode << " <SCENE NOT STABLE>";
    } else {
        sendingMode << " <SCENE STABLE>";
    }

    label = _labels[_sendingMode];
    label->setText(sendingMode.str().c_str());
    
    // Server Voxels
    QString serversTotalString = locale.toString((uint)totalNodes); // consider adding: .rightJustified(10, ' ');
    QString serversInternalString = locale.toString((uint)totalInternal);
    QString serversLeavesString = locale.toString((uint)totalLeaves);
    label = _labels[_serverVoxels];
    statsValue.str("");
    statsValue << 
        "Total: " << serversTotalString.toLocal8Bit().constData() << " / " <<
        "Internal: " << serversInternalString.toLocal8Bit().constData() << " / " <<
        "Leaves: " << serversLeavesString.toLocal8Bit().constData() << "";
    label->setText(statsValue.str().c_str());

    showAllOctreeServers();

    this->QDialog::paintEvent(event);
}
void VoxelStatsDialog::showAllOctreeServers() {
    int serverCount = 0;

    showOctreeServersOfType(serverCount, NodeType::VoxelServer, "Voxel",
            Application::getInstance()->getVoxelServerJurisdictions());
    showOctreeServersOfType(serverCount, NodeType::ParticleServer, "Particle",
            Application::getInstance()->getParticleServerJurisdictions());

    if (_voxelServerLabelsCount > serverCount) {
        for (int i = serverCount; i < _voxelServerLabelsCount; i++) {
            int serverLabel = _voxelServerLables[i];
            RemoveStatItem(serverLabel);
            _voxelServerLables[i] = 0;
        }
        _voxelServerLabelsCount = serverCount;
    }
}

void VoxelStatsDialog::showOctreeServersOfType(int& serverCount, NodeType_t serverType, const char* serverTypeName,
                                                NodeToJurisdictionMap& serverJurisdictions) {
                                                
    QLocale locale(QLocale::English);
    
    NodeList* nodeList = NodeList::getInstance();

    foreach (const SharedNodePointer& node, nodeList->getNodeHash()) {
        // only send to the NodeTypes that are NodeType_t_VOXEL_SERVER
        if (node->getType() == serverType) {
            serverCount++;
            
            if (serverCount > _voxelServerLabelsCount) {
                char label[128] = { 0 };
                sprintf(label, "%s Server %d", serverTypeName, serverCount);
                int thisServerRow = _voxelServerLables[serverCount-1] = AddStatItem(label);
                _labels[thisServerRow]->setTextFormat(Qt::RichText);
                _labels[thisServerRow]->setTextInteractionFlags(Qt::TextBrowserInteraction);
                connect(_labels[thisServerRow], SIGNAL(linkActivated(const QString&)), this, SLOT(moreless(const QString&)));
                _voxelServerLabelsCount++;
            }
            
            std::stringstream serverDetails("");
            std::stringstream extraDetails("");
            std::stringstream linkDetails("");
            
            if (nodeList->getNodeActiveSocketOrPing(node.data())) {
                serverDetails << "active ";
            } else {
                serverDetails << "inactive ";
            }
            
            QUuid nodeUUID = node->getUUID();
            
            // lookup our nodeUUID in the jurisdiction map, if it's missing then we're
            // missing at least one jurisdiction
            if (serverJurisdictions.find(nodeUUID) == serverJurisdictions.end()) {
                serverDetails << " unknown jurisdiction ";
            } else {
                const JurisdictionMap& map = serverJurisdictions[nodeUUID];
                
                unsigned char* rootCode = map.getRootOctalCode();
                
                if (rootCode) {
                    QString rootCodeHex = octalCodeToHexString(rootCode);
                    
                    VoxelPositionSize rootDetails;
                    voxelDetailsForCode(rootCode, rootDetails);
                    AABox serverBounds(glm::vec3(rootDetails.x, rootDetails.y, rootDetails.z), rootDetails.s);
                    serverBounds.scale(TREE_SCALE);
                    serverDetails << " jurisdiction: "
                    << rootCodeHex.toLocal8Bit().constData()
                    << " ["
                    << rootDetails.x << ", "
                    << rootDetails.y << ", "
                    << rootDetails.z << ": "
                    << rootDetails.s << "] ";
                } else {
                    serverDetails << " jurisdiction has no rootCode";
                } // root code
            } // jurisdiction
            
            // now lookup stats details for this server...
            if (_extraServerDetails[serverCount-1] != LESS) {
                Application::getInstance()->lockVoxelSceneStats();
                NodeToVoxelSceneStats* sceneStats = Application::getInstance()->getOcteeSceneStats();
                if (sceneStats->find(nodeUUID) != sceneStats->end()) {
                    VoxelSceneStats& stats = sceneStats->at(nodeUUID);
                    
                    switch (_extraServerDetails[serverCount-1]) {
                        case MOST: {
                            extraDetails << "<br/>" ;
                            
                            const unsigned long USECS_PER_MSEC = 1000;
                            float lastFullEncode = stats.getLastFullTotalEncodeTime() / USECS_PER_MSEC;
                            float lastFullSend = stats.getLastFullElapsedTime() / USECS_PER_MSEC;
                            
                            QString lastFullEncodeString = locale.toString(lastFullEncode);
                            QString lastFullSendString = locale.toString(lastFullSend);
                            
                            extraDetails << "<br/>" << "Last Full Scene... " <<
                            "Encode Time: " << lastFullEncodeString.toLocal8Bit().constData() << " ms " <<
                            "Send Time: " << lastFullSendString.toLocal8Bit().constData() << " ms ";
                            
                            for (int i = 0; i < VoxelSceneStats::ITEM_COUNT; i++) {
                                VoxelSceneStats::Item item = (VoxelSceneStats::Item)(i);
                                VoxelSceneStats::ItemInfo& itemInfo = stats.getItemInfo(item);
                                extraDetails << "<br/>" << itemInfo.caption << " " << stats.getItemValue(item);
                            }
                        } // fall through... since MOST has all of MORE
                        case MORE: {
                            QString totalString = locale.toString((uint)stats.getTotalElements());
                            QString internalString = locale.toString((uint)stats.getTotalInternal());
                            QString leavesString = locale.toString((uint)stats.getTotalLeaves());
                            
                            serverDetails << "<br/>" << "Node UUID: " <<
                            nodeUUID.toString().toLocal8Bit().constData() << " ";
                            
                            serverDetails << "<br/>" << "Voxels: " <<
                            totalString.toLocal8Bit().constData() << " total " <<
                            internalString.toLocal8Bit().constData() << " internal " <<
                            leavesString.toLocal8Bit().constData() << " leaves ";
                            
                            QString incomingPacketsString = locale.toString((uint)stats.getIncomingPackets());
                            QString incomingBytesString = locale.toString((uint)stats.getIncomingBytes());
                            QString incomingWastedBytesString = locale.toString((uint)stats.getIncomingWastedBytes());
                            QString incomingOutOfOrderString = locale.toString((uint)stats.getIncomingOutOfOrder());
                            QString incomingLikelyLostString = locale.toString((uint)stats.getIncomingLikelyLost());
                            
                            int clockSkewInMS = node->getClockSkewUsec() / (int)USECS_PER_MSEC;
                            QString incomingFlightTimeString = locale.toString((int)stats.getIncomingFlightTimeAverage());
                            QString incomingPingTimeString = locale.toString(node->getPingMs());
                            QString incomingClockSkewString = locale.toString(clockSkewInMS);
                            
                            serverDetails << "<br/>" << "Incoming Packets: " <<
                            incomingPacketsString.toLocal8Bit().constData() <<
                            " Out of Order: " << incomingOutOfOrderString.toLocal8Bit().constData() <<
                            " Likely Lost: " << incomingLikelyLostString.toLocal8Bit().constData();
                            
                            serverDetails << "<br/>" <<
                            " Average Flight Time: " << incomingFlightTimeString.toLocal8Bit().constData() << " msecs";
                            
                            serverDetails << "<br/>" <<
                            " Average Ping Time: " << incomingPingTimeString.toLocal8Bit().constData() << " msecs";
                            
                            serverDetails << "<br/>" <<
                            " Average Clock Skew: " << incomingClockSkewString.toLocal8Bit().constData() << " msecs";
                            
                            serverDetails << "<br/>" << "Incoming" <<
                            " Bytes: " <<  incomingBytesString.toLocal8Bit().constData() <<
                            " Wasted Bytes: " << incomingWastedBytesString.toLocal8Bit().constData();
                            
                            serverDetails << extraDetails.str();
                            if (_extraServerDetails[serverCount-1] == MORE) {
                                linkDetails << "   " << " [<a href='most-" << serverCount << "'>most...</a>]";
                                linkDetails << "   " << " [<a href='less-" << serverCount << "'>less...</a>]";
                            } else {
                                linkDetails << "   " << " [<a href='more-" << serverCount << "'>less...</a>]";
                                linkDetails << "   " << " [<a href='less-" << serverCount << "'>least...</a>]";
                            }
                            
                        } break;
                        case LESS: {
                            // nothing
                        } break;
                    }
                }
                Application::getInstance()->unlockVoxelSceneStats();
            } else {
                linkDetails << "   " << " [<a href='more-" << serverCount << "'>more...</a>]";
                linkDetails << "   " << " [<a href='most-" << serverCount << "'>most...</a>]";
            }
            serverDetails << linkDetails.str();
            _labels[_voxelServerLables[serverCount - 1]]->setText(serverDetails.str().c_str());
        } // is VOXEL_SERVER
    }
}

void VoxelStatsDialog::reject() {
    // Just regularly close upon ESC
    this->QDialog::close();
}

void VoxelStatsDialog::closeEvent(QCloseEvent* event) {
    this->QDialog::closeEvent(event);
    emit closed();
}


