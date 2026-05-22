//
// Created by daver on 7/24/2024.
//

#include "rpLoopTime.h"
#include "stdio.h"


rpLoopTime::rpLoopTime() {
    resetStats();
}


void rpLoopTime::resetStats()
{
    m_iMinTimeUs = 0;
    m_iMaxTimeUs = 0;
    m_iCurrentTimeUs=0;
    m_bStatsSet = false;
    m_bHasStarted = false;
}

void rpLoopTime::timerStart()
{
    stStartTime = get_absolute_time();
    m_bHasStarted = true;
}

void rpLoopTime::formatTime(char * szFormatedTime, int64_t iValue)
{
    if (iValue < 1000) {
        sprintf(szFormatedTime,"%d us",(int) iValue);
    }
    else {
        float fValue = (float)iValue / 1000.0;
        sprintf(szFormatedTime,"%.3f ms", fValue);
    }
}

void rpLoopTime::timerStop()
{
    stEndTime = get_absolute_time();
    m_iCurrentTimeUs = absolute_time_diff_us(stStartTime,stEndTime);

    if (m_bStatsSet)
    {
        if (m_iCurrentTimeUs < m_iMinTimeUs)
             m_iMinTimeUs=m_iCurrentTimeUs;
        if (m_iCurrentTimeUs > m_iMaxTimeUs)
            m_iMaxTimeUs=m_iCurrentTimeUs;
    }
    else
    {
        m_iMinTimeUs = m_iCurrentTimeUs;
        m_iMaxTimeUs = m_iCurrentTimeUs;
        m_bStatsSet = true;
    }
}
