/*
 * rcc.c
 *
 *  Created on: 18 apr. 2020
 *      Author: Ludo
 */

#include "rcc.h"

#include "error.h"
#include "flash.h"
#include "nvic.h"
#include "rcc_reg.h"
#include "pwr.h"
#include "tim.h"
#include "types.h"

/*** RCC local macros ***/

#define RCC_TIMEOUT_COUNT			1000000

#define RCC_LSI_AVERAGING_COUNT		5
#define RCC_LSI_FREQUENCY_MIN_HZ	26000
#define RCC_LSI_FREQUENCY_MAX_HZ	56000

/*******************************************************************/
void __attribute__((optimize("-O0"))) RCC_IRQHandler(void) {
	// Clear all flags.
	RCC -> CICR |= (0b11 << 0);
}

/*******************************************************************/
void _RCC_enable_lsi(void) {
	// Enable LSI.
	RCC -> CSR |= (0b1 << 0); // LSION='1'.
	// Enable interrupt.
	RCC -> CIER |= (0b1 << 0);
	NVIC_enable_interrupt(NVIC_INTERRUPT_RCC_CRS, NVIC_PRIORTY_RCC_CRS);
	// Wait for LSI to be stable.
	while (((RCC -> CSR) & (0b1 << 1)) == 0) {
		PWR_enter_sleep_mode();
	}
	NVIC_disable_interrupt(NVIC_INTERRUPT_RCC_CRS);
}

/*******************************************************************/
RCC_status_t _RCC_enable_lse(void) {
	// Local variables.
	RCC_status_t status = RCC_SUCCESS;
	uint32_t loop_count = 0;
	// Enable LSE (32.768kHz crystal).
	RCC -> CSR |= (0b1 << 8); // LSEON='1'.
	// Wait for LSE to be stable.
	while (((RCC -> CSR) & (0b1 << 9)) == 0) {
		// Wait for LSERDY='1' ready flag or timeout.
		loop_count++;
		if (loop_count > RCC_TIMEOUT_COUNT) {
			status = RCC_ERROR_LSE_READY;
			goto errors;
		}
	}
errors:
	return status;
}

/*** RCC functions ***/

/*******************************************************************/
RCC_status_t __attribute__((optimize("-O0"))) RCC_init(void) {
	// Local variables.
	RCC_status_t status = RCC_SUCCESS;
	uint8_t i = 0;
	// Reset backup domain.
	RCC -> CSR |= (0b1 << 19); // RTCRST='1'.
	for (i=0 ; i<100 ; i++);
	RCC -> CSR &= ~(0b1 << 19); // RTCRST='0'.
	// Enable low speed oscillators.
	_RCC_enable_lsi();
	status = _RCC_enable_lse();
	return status;
}

/*******************************************************************/
RCC_status_t RCC_switch_to_hsi(void) {
	// Local variables.
	RCC_status_t status = RCC_SUCCESS;
	FLASH_status_t flash_status = FLASH_SUCCESS;
	uint32_t loop_count = 0;
	// Set flash latency.
	flash_status = FLASH_set_latency(1);
	FLASH_check_status(RCC_ERROR_BASE_FLASH);
	// Enable HSI.
	RCC -> CR |= (0b1 << 0); // Enable HSI (HSI16ON='1').
	// Wait for HSI to be stable.
	while (((RCC -> CR) & (0b1 << 2)) == 0) {
		// Wait for HSIRDYF='1' or timeout.
		loop_count++;
		if (loop_count > RCC_TIMEOUT_COUNT) {
			status = RCC_ERROR_HSI_READY;
			goto errors;
		}
	}
	// Switch SYSCLK.
	RCC -> CFGR &= ~(0b11 << 0); // Reset bits 0-1.
	RCC -> CFGR |= (0b01 << 0); // Use HSI as system clock (SW='01').
	// Wait for clock switch.
	loop_count = 0;
	while (((RCC -> CFGR) & (0b11 << 2)) != (0b01 << 2)) {
		// Wait for SWS='01' or timeout.
		loop_count++;
		if (loop_count > RCC_TIMEOUT_COUNT) {
			status = RCC_ERROR_HSI_SWITCH;
			goto errors;
		}
	}
	// Disable MSI.
	RCC -> CR &= ~(0b1 << 8); // MSION='0'.
errors:
	return status;
}

/*******************************************************************/
RCC_status_t RCC_switch_to_msi(RCC_msi_range_t msi_range) {
	// Local variables.
	RCC_status_t status = RCC_SUCCESS;
	FLASH_status_t flash_status = FLASH_SUCCESS;
	uint32_t loop_count = 0;
	// Check parameter.
	if (msi_range >= RCC_MSI_RANGE_LAST) {
		status = RCC_ERROR_MSI_RANGE;
		goto errors;
	}
	// Set frequency.
	RCC -> ICSCR &= ~(0b111 << 13);
	RCC -> ICSCR |= (msi_range << 13);
	// Enable MSI.
	RCC -> CR |= (0b1 << 8); // MSION='1'.
	// Wait for MSI to be stable.
	while (((RCC -> CR) & (0b1 << 9)) == 0) {
		// Wait for MSIRDYF='1' or timeout.
		loop_count++;
		if (loop_count > RCC_TIMEOUT_COUNT) {
			status = RCC_ERROR_MSI_READY;
			goto errors;
		}
	}
	// Switch SYSCLK.
	RCC -> CFGR &= ~(0b11 << 0); // Use MSI as system clock (SW='00').
	// Wait for clock switch.
	while (((RCC -> CFGR) & (0b11 << 2)) != (0b00 << 2)) {
		// Wait for SWS='00' or timeout.
		loop_count++;
		if (loop_count > RCC_TIMEOUT_COUNT) {
			status = RCC_ERROR_MSI_SWITCH;
			goto errors;
		}
	}
	// Set flash latency.
	flash_status = FLASH_set_latency(0);
	FLASH_check_status(RCC_ERROR_BASE_FLASH);
	// Disable HSI.
	RCC -> CR &= ~(0b1 << 0); // HSI16ON='0'.
errors:
	return status;
}

/*******************************************************************/
RCC_status_t RCC_measure_lsi_frequency(uint32_t* lsi_frequency_hz) {
	// Local variables.
	RCC_status_t status = RCC_SUCCESS;
	TIM_status_t tim21_status = TIM_SUCCESS;
	uint32_t lsi_frequency_sample = 0;
	uint8_t sample_idx = 0;
	// Check parameter.
	if (lsi_frequency_hz == NULL) {
		status = RCC_ERROR_NULL_PARAMETER;
		goto errors;
	}
	// Reset result.
	(*lsi_frequency_hz) = RCC_LSI_FREQUENCY_HZ;
	// Init measurement timer.
	TIM21_init();
	// Compute average.
	for (sample_idx=0 ; sample_idx<RCC_LSI_AVERAGING_COUNT ; sample_idx++) {
		// Perform measurement.
		tim21_status = TIM21_measure_lsi_frequency(&lsi_frequency_sample);
		TIM21_stack_error(RCC_ERROR_BASE_TIM);
		(*lsi_frequency_hz) = (((*lsi_frequency_hz) * sample_idx) + lsi_frequency_sample) / (sample_idx + 1);
	}
	// Check value.
	if (((*lsi_frequency_hz) < RCC_LSI_FREQUENCY_MIN_HZ) || ((*lsi_frequency_hz) > RCC_LSI_FREQUENCY_MAX_HZ)) {
		// Set to default value if out of expected range
		(*lsi_frequency_hz) = RCC_LSI_FREQUENCY_HZ;
		status = RCC_ERROR_LSI_MEASUREMENT;
	}
errors:
	TIM21_de_init();
	return status;
}

/*******************************************************************/
uint8_t RCC_get_lsi_status(void) {
	// Read LSI ready flag.
	return (((RCC -> CSR) >> 1) & 0b1);
}

/*******************************************************************/
uint8_t RCC_get_lse_status(void) {
	// Read LSE ready flag.
	return (((RCC -> CSR) >> 9) & 0b1);
}
