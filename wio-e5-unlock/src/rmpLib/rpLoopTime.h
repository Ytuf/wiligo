//
// Created by daver on 7/24/2024.
//

#ifndef RPLOOPTIME_H
#define RPLOOPTIME_H

#include "pico/time.h"

class rpLoopTime {
private:
    bool m_bStatsSet = false;
    bool m_bHasStarted = false;

public:

    rpLoopTime();

    absolute_time_t stStartTime;
    absolute_time_t stEndTime;

    int64_t m_iMinTimeUs;
    int64_t m_iMaxTimeUs;
    int64_t m_iCurrentTimeUs;

    bool hasStarted() { return m_bHasStarted; };

    void resetStats();

    void timerStart();
    void timerStop();

    void formatTime(char * szFormatedTime, int64_t iValue);

};



#endif //RPLOOPTIME_H
