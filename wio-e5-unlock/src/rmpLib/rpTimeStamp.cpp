//
// Created by daver on 3/20/2025.
//

#include "rpTimeStamp.h"

#include <cstdio>

#ifndef PC_SIMULATION
#include "rpBase64.h"
#endif

//#define DayN 		(3456000000000LL * TONANOSEC) 	// --- 24*3600*40000000 	ticks per minute
#define DayN		1000000000LL * 3600LL * 24LL
#define HourN		(1000000000LL * 3600LL)					// --- 3600*40000000 	ticks per minute
#define MinN		(1000000000LL * 60LL)					// --- 60*40000000 	ticks per minute
#define SecN		(1000000000LL) 				// --- 40000000 		ticks per second
#define FracN		(400000LL * TONANOSEC) 			// --- 400000			ticks per 1/100 of a second
#define MsN			(40000LL  * TONANOSEC)			// 	ticks per msecond
#define UsN			(40LL  * TONANOSEC)				// 	ticks per usecond




typedef struct _SDateTime {
	unsigned long options;   	// Bitfield with options flags.
	unsigned short usec;  		// --- microseconds (0-999)
	unsigned short msec;  		// --- milliseconds (0-999)
	unsigned char Sec;	   	// --- Seconds (00-59)
	unsigned char Min;		// --- (00-59)
	unsigned char Hour;		// --- (00-23)
	unsigned char DOW;		// --- (01-07)
	unsigned char Day;		// --- (01-31)
	unsigned char Month;		// --- (01-12)
	unsigned short Year;		// --- (2000-2999)
	unsigned char reserved; // ---- Always Zero
} SDateTime;

rptype::u64 RTC_RTCToTicks(const SDateTime * rtc);

int rpTimeStamp::getTimeNowAsBase64(char * pString, int iMaxSize) {
#ifdef PC_SIMULATION
	return 0;
#else
	rpBase64 obBase64;

	rptype::u64  iTimeNow = getTimeNow();
	obBase64.Encode((const unsigned char * ) & iTimeNow,sizeof(rptype::u64),(unsigned char *)pString,iMaxSize);
	return iMaxSize;
#endif
}

void rpTimeStamp::getTimeNowAsText(char * pString17CharsLong)
{
	//rpBase64 obBase64;
	rptype::u64  iTimeNow = getTimeNowNs();
	unsigned char * pBytes = (unsigned char *) & iTimeNow;
	char szByte[4];
	for (int i=0; i < sizeof(rptype::u64); i++)
	{
		sprintf(szByte,"%02X",pBytes[i]);
		pString17CharsLong[(7-i)*2] =	szByte[0];
		pString17CharsLong[(7-i)*2+1] = szByte[1];
	}
	pString17CharsLong[16]=0;

}

rpTimeStamp::rpTimeStamp()
{
	// without rtc just have an increasing timer
	obTime.resetStats();
	obTime.timerStart();
}

// sets the reference time to the real time clock
void rpTimeStamp::initWithRTCTime(unsigned int iYear, unsigned int iMonth, unsigned int iDay, unsigned int iDOW,
   unsigned int iHour, unsigned int iMin, unsigned int iSec)
{
   obTime.resetStats();
   obTime.timerStart();

	// only valid dates and times
	if (iYear < 2000 || iYear > 2099)
		return;
	if (iMonth < 1 || iMonth > 12)
		return;
	if (iDay < 1 || iDay > 31)
		return;
	if (iHour < 0 || iHour > 24)
		return;
	if (iMin < 0 || iMin > 59)
		return;
	if (iSec < 0 || iSec > 59)
		return;

   setTimeFromParts(iMonth,iDay,iYear,iHour,iMin, iSec,0);
}

rptype::u64 rpTimeStamp::getTimeNowUs()
{
	obTime.timerStop();
	return obTime.m_iCurrentTimeUs;
}

rptype::u64 rpTimeStamp::getTimeNowNs()
{
	obTime.timerStop();
	return (obTime.m_iCurrentTimeUs*1000ll)+uiStartRTCTimeNanoSeconds;
}

rptype::u64 rpTimeStamp::getTimeNow()
{
   obTime.timerStop();
   return obTime.m_iCurrentTimeUs+uiStartRTCTimeNanoSeconds;
}

void rpTimeStamp::setTimeFromParts(int iUTCMonth1_12, int iUTCDay1_31, int iUTCYear1970_2190, int iUTC_24hoursHour0_23,
	int iUTC_Minutes0_59, int iUTCSeconds0_59, rptype::u64 iUTCTimeNanoSeconds)
{
	SDateTime stTM = { 0 };

	stTM.Month = iUTCMonth1_12;
	stTM.Day = iUTCDay1_31;
	stTM.Year = iUTCYear1970_2190;

	stTM.Hour = iUTC_24hoursHour0_23;
	stTM.Min = iUTC_Minutes0_59;
	stTM.Sec = iUTCSeconds0_59;

	uiStartRTCTimeNanoSeconds = RTC_RTCToTicks(&stTM);
	uiStartRTCTimeNanoSeconds += iUTCTimeNanoSeconds;

	//fwString sTime;
	//fwString sFormat;
	//sFormat = "0.00";
	//GetFormattedTime(sTime, sFormat, eicsRealTimeFormatUTC);

}

bool IsLeapYear(int iYear)
{
	if ((iYear & 3) == 0) // leap year
		return true;
	else
		return false;
}


void RTC_DaysToMonth(unsigned int year, int iDays, int & iMonth, int & iDaysRemaining)
{
	unsigned char DaysInMonth[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
	if (IsLeapYear(year)) // leap year
		DaysInMonth[1] = 29;

	//int i;
	int iDaysTemp = iDays;
	iMonth = 0;
	do
	{
		iDaysTemp -= DaysInMonth[iMonth++];

	} while (iDaysTemp > 0);

	iMonth--;
	iDaysRemaining = iDaysTemp + DaysInMonth[iMonth];

}

void rpTimeStamp::DetermineDaysFromYears(rptype::u64 iNanoseconds, int & iYear, int & iDays, rptype::u64 & iLeft)
{

	int iYearCount = 1970;
	bool bIsLeapYear = false;


	iLeft = iNanoseconds;

	do
	{
		bIsLeapYear = IsLeapYear(iYearCount);

		iLeft -= RPTIMESTAMP_YEAR_TO_NANOSECONDS;
		if (bIsLeapYear)
			iLeft -= RPTIMESTAMP_DAY_TO_NANOSECONDS; // add the extra day

		iYearCount++;

	} while (iLeft > 0);

	iYearCount--;
	iLeft += RPTIMESTAMP_YEAR_TO_NANOSECONDS;
	if (bIsLeapYear)
		iLeft += RPTIMESTAMP_DAY_TO_NANOSECONDS; // add the extra day

	iYear = iYearCount;

	//__int64 iNumYears = uiTimeNanoSeconds / CICSREALTIME_YEAR_TO_NANOSECONDS;
	//iYear = iNumYears + 1970;
	//iLeft = uiTimeNanoSeconds - (iNumYears * CICSREALTIME_YEAR_TO_NANOSECONDS);

	rptype::u64 iNumNanoSeconds = RPTIMESTAMP_DAY_TO_NANOSECONDS;
	rptype::u64 iNumDays = iLeft / iNumNanoSeconds;

	iDays = (int)iNumDays;

}

/*
int iUTCMonth1_12, int iUTCDay1_31, int iUTCYear, int iUTC_24hoursHour0_23,
	int iUTC_Minutes0_59, int iUTCSeconds0_59, __int64 iUTCTimeNanoSeconds
*/
void rpTimeStamp::GetRealTimeFromParts(int & iUTCMonth1_12, int & iUTCDay1_31, int & iUTCYear1970_2190,
	int & iUTC_24hoursHour0_23, int & iUTC_Minutes0_59, int & iUTCSeconds0_59, rptype::u64 & iUTCTimeNanoSeconds)
{
	rptype::u64 iLeft;
	int iNumDays;

	DetermineDaysFromYears(uiStartRTCTimeNanoSeconds, iUTCYear1970_2190, iNumDays, iLeft);

	RTC_DaysToMonth(iUTCYear1970_2190, iNumDays, iUTCMonth1_12, iUTCDay1_31);
	iUTCMonth1_12++; // months start at 1
	iUTCDay1_31++;

	iLeft -= ((rptype::u64)iNumDays * RPTIMESTAMP_DAY_TO_NANOSECONDS);


	rptype::u64 iNumHours = iLeft / RPTIMESTAMP_HOUR_TO_NANOSECONDS;
	iUTC_24hoursHour0_23 = (int)iNumHours;

	iLeft -= iNumHours * RPTIMESTAMP_HOUR_TO_NANOSECONDS;

	rptype::u64 iNumMinutes = iLeft / RPTIMESTAMP_MIN_TO_NANOSECONDS;
	iUTC_Minutes0_59 = (int)iNumMinutes;

	iLeft -= iNumMinutes * RPTIMESTAMP_MIN_TO_NANOSECONDS;

	rptype::u64 iNumSeconds = iLeft / RPTIMESTAMP_SEC_TO_NANOSECONDS;
	iUTCSeconds0_59 = (int)iNumSeconds;

	iLeft -= iNumSeconds * RPTIMESTAMP_SEC_TO_NANOSECONDS;

	iUTCTimeNanoSeconds = iLeft;


}


//months is exclusive
unsigned long long RTC_MonthsToDays(signed int month, unsigned int year)
{
	unsigned char DaysInMonth[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
	rptype::u64 days = 0;
	signed int i;

	if (month == 0 || month > 12)
		return 0;

	if (IsLeapYear(1970+year)) // leap year
		DaysInMonth[1] = 29;

	for (i = 0; i < (month - 1); i++)
	{
		days += DaysInMonth[i];
	}

	return days;
}

//months is exclusive
unsigned long long RTC_YearsToDays(unsigned int year)
{
	unsigned int i;
	rptype::u64 days = 0;
	bool bIsLeapYear = false;
//	bool bCountLeapYears = false;
	int iYearCount = 1970;
//	int iLeapCount = 0;

//	ics_assert(year < 3000); /* sanity check the year */
//	if (year >= 3000)  /* we don't want to spin loop if year is bad */
//		return 0;
	for (i = 0; i < year; ++i)
	{

		bIsLeapYear = IsLeapYear(iYearCount);

		days += 365;
		if (bIsLeapYear)
			++days;

		iYearCount++;
	}

	return days;
}



rptype::u64 M64(long long int A, long long int B)
{
	return A * B;
}
rptype::u64 D64(long long int A, long long int B)
{
	//ics_assert(B);
	if (B == 0)
		return 0;
	return A / B;
}



rptype::u64 RTC_RTCToTicks(const SDateTime * rtc)
{
	rptype::u64  time = 0;
	//long long int start_offset;


	volatile rptype::u64  yr, mon, dy, hr, min, sec, msec, usec;

	//if (rtc->Year > 2000)
	//	yr = rtc->Year - 2000;
	//else
	yr = rtc->Year - 1970;// years since epoch // rtc->Year;

	mon = rtc->Month;

	dy = rtc->Day - 1;

	hr = rtc->Hour;

	min = rtc->Min;

	sec = rtc->Sec;

	msec = rtc->msec;
	usec = rtc->usec;

	// --- Convert  Days:Hours:Min:Sec, FractionSec   into 64 bit timer in [HL LL]


	time += M64(DayN, RTC_YearsToDays((int)yr));


	time += M64(DayN, RTC_MonthsToDays((int)mon, (int)yr));

	time += M64(DayN, dy);			// --- Day


	time += M64(HourN, hr);		// --- Hours

	time += M64(MinN, min);			// --- Min

	time += M64(SecN, sec);			// --- Seconds

	return time;

}