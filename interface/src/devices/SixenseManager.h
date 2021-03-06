//
//  SixenseManager.h
//  interface
//
//  Created by Andrzej Kapolka on 11/15/13.
//  Copyright (c) 2013 High Fidelity, Inc. All rights reserved.
//

#ifndef __interface__SixenseManager__
#define __interface__SixenseManager__

/// Handles interaction with the Sixense SDK (e.g., Razer Hydra).
class SixenseManager : public QObject {
    Q_OBJECT
public:
    
    SixenseManager();
    ~SixenseManager();
    
    void update(float deltaTime);
    
public slots:
    
    void setFilter(bool filter);

private:
    
    quint64 _lastMovement;
};

#endif /* defined(__interface__SixenseManager__) */

