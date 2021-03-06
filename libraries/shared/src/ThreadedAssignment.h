//
//  ThreadedAssignment.h
//  hifi
//
//  Created by Stephen Birarda on 12/3/2013.
//  Copyright (c) 2013 HighFidelity, Inc. All rights reserved.
//

#ifndef __hifi__ThreadedAssignment__
#define __hifi__ThreadedAssignment__

#include "Assignment.h"

class ThreadedAssignment : public Assignment {
    Q_OBJECT
public:
    ThreadedAssignment(const QByteArray& packet);
    
    void setFinished(bool isFinished);
public slots:
    /// threaded run of assignment
    virtual void run() = 0;
    
    virtual void deleteLater();
    
    virtual void processDatagram(const QByteArray& dataByteArray, const HifiSockAddr& senderSockAddr) = 0;
protected:
    void commonInit(const char* targetName, NodeType_t nodeType);
    bool _isFinished;
private slots:
    void checkInWithDomainServerOrExit();
signals:
    void finished();
};


#endif /* defined(__hifi__ThreadedAssignment__) */
