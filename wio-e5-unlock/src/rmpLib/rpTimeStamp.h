//
// Created by daver on 3/20/2025.
//

#ifndef RPTIMESTAMP_H
#define RPTIMESTAMP_H

#include "rpTypes.h"
#include "rpLoopTime.h"

#define RPTIMESTAMP_SEC_TO_NANOSECONDS 1000000000ll
#define RPTIMESTAMP_MIN_TO_NANOSECONDS (RPTIMESTAMP_SEC_TO_NANOSECONDS * 60ll)
#define RPTIMESTAMP_HOUR_TO_NANOSECONDS (RPTIMESTAMP_MIN_TO_NANOSECONDS * 60ll)
#define RPTIMESTAMP_DAY_TO_NANOSECONDS (RPTIMESTAMP_HOUR_TO_NANOSECONDS * 24ll)
#define RPTIMESTAMP_YEAR_TO_NANOSECONDS (RPTIMESTAMP_DAY_TO_NANOSECONDS * 365ll)

#define RPTIMESTAMP_MILLISEC_TO_NANOSECONDS 1000000ll
class rpTimeStamp {
private:

    void DetermineDaysFromYears(rptype::u64 iNanoseconds, int & iYear, int & iDays, rptype::u64 & iLeft);
    void setTimeFromParts(int iUTCMonth1_12, int iUTCDay1_31, int iUTCYear1970_2190, int iUTC_24hoursHour0_23,
        int iUTC_Minutes0_59, int iUTCSeconds0_59, rptype::u64 iUTCTimeNanoSeconds);
    void GetRealTimeFromParts(int & iUTCMonth1_12, int & iUTCDay1_31, int & iUTCYear1970_2190,
        int & iUTC_24hoursHour0_23, int & iUTC_Minutes0_59, int & iUTCSeconds0_59, rptype::u64 & iUTCTimeNanoSeconds);

public:

    rpTimeStamp();

    rpLoopTime obTime;

    // Since 1970 (The Unix Epoch), Rolls over every 500 years!
    //rptype::u64 m_iStartTimeNs=0;
    rptype::u64 uiStartRTCTimeNanoSeconds=0;

    void initWithRTCTime(unsigned int iYear,unsigned  int iMonth,unsigned  int iDay,
                unsigned int iDOW,unsigned  int iHour,unsigned  int iMin,unsigned  int iSec);

    rptype::u64 getTimeNow();
    rptype::u64 getTimeNowNs();
    rptype::u64 getTimeNowUs();

    void getTimeNowAsText(char * pString17CharsLong); // 16 characters and a null
    int getTimeNowAsBase64(char * pString, int iMaxSize);
};



#endif //RPTIMESTAMP_H
