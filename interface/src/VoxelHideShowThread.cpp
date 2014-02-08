//
//  VoxelHideShowThread.cpp
//  interface
//
//  Created by Brad Hefta-Gaub on 12/1/13
//  Copyright (c) 2013 High Fidelity, Inc. All rights reserved.
//
//  Threaded or non-threaded interface thread for hiding and showing voxels in the local tree.
//

#include <QDebug>
#include <NodeList.h>
#include <PerfStat.h>
#include <SharedUtil.h>

#include "Application.h"
#include "VoxelHideShowThread.h"

VoxelHideShowThread::VoxelHideShowThread(VoxelSystem* theSystem) :
    _theSystem(theSystem) {
}

bool VoxelHideShowThread::process() {
    const quint64 MSECS_TO_USECS = 1000;
    const quint64 SECS_TO_USECS = 1000 * MSECS_TO_USECS;
    const quint64 FRAME_RATE = 60;
    const quint64 USECS_PER_FRAME = SECS_TO_USECS / FRAME_RATE; // every 60fps

    quint64 start = usecTimestampNow();
    if(_theSystem){
      _theSystem->checkForCulling();
    }
    quint64 end = usecTimestampNow();
    quint64 elapsed = end - start;

    bool showExtraDebugging = Application::getInstance()->getLogger()->extraDebugging();
    if (showExtraDebugging && elapsed > USECS_PER_FRAME) {
        qDebug() << "VoxelHideShowThread::process()... checkForCulling took" << elapsed;
    }

    if (isStillRunning()) {
        if (elapsed < USECS_PER_FRAME) {
            quint64 sleepFor = USECS_PER_FRAME - elapsed;
            usleep(sleepFor);
        }
    }
    return isStillRunning();  // keep running till they terminate us
}
