/*
 * rtc.h
 *
 *  Created on: 18 apr. 2020
 *      Author: Ludo
 */

#ifndef __RTC_H__
#define __RTC_H__

#include "types.h"

/*** RTC macros ***/

// RTC wake-up timer period.
// Warning: this value must be lower than the watchdog period = 25s.
 #define RTC_WAKEUP_PERIOD_SECONDS	10

/*** RTC structures ***/

typedef enum {
	RTC_SUCCESS = 0,
	RTC_ERROR_INITIALIZATION_MODE,
	RTC_ERROR_WAKEUP_TIMER_DELAY,
	RTC_ERROR_WAKEUP_TIMER_RUNNING,
	RTC_ERROR_BASE_LAST = 0x0100
} RTC_status_t;

typedef struct {
	// Date.
	uint16_t year;
	uint8_t month;
	uint8_t date;
	// Time.
	uint8_t hours;
	uint8_t minutes;
	uint8_t seconds;
} RTC_time_t;

/*** RTC functions ***/

void RTC_reset(void);
RTC_status_t __attribute__((optimize("-O0"))) RTC_init(uint8_t* rtc_use_lse, uint32_t lsi_freq_hz);

RTC_status_t RTC_start_wakeup_timer(uint32_t delay_seconds);
RTC_status_t RTC_stop_wakeup_timer(void);
volatile uint8_t RTC_get_wakeup_timer_flag(void);
void RTC_clear_wakeup_timer_flag(void);

#define RTC_status_check(error_base) { if (rtc_status != RTC_SUCCESS) { status = error_base + rtc_status; goto errors; }}
#define RTC_error_check() { ERROR_status_check(rtc_status, RTC_SUCCESS, ERROR_BASE_RTC); }
#define RTC_error_check_print() { ERROR_status_check(rtc_status, RTC_SUCCESS, ERROR_BASE_RTC); }

#endif /* __RTC_H__ */
