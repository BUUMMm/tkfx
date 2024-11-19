/*
 * s2lp_hw.c
 *
 *  Created on: 17 nov. 2024
 *      Author: Ludo
 */

#include "s2lp_hw.h"

#ifndef S2LP_DRIVER_DISABLE_FLAGS_FILE
#include "s2lp_driver_flags.h"
#endif
#include "error.h"
#include "gpio.h"
#include "gpio_mapping.h"
#include "lptim.h"
#include "s2lp.h"
#include "spi.h"
#include "types.h"

#ifndef S2LP_DRIVER_DISABLE

/*** S2LP HW macros ***/

#define S2LP_HW_SPI_INSTANCE    SPI_INSTANCE_SPI1

/*** S2LP HW functions ***/

/*******************************************************************/
S2LP_status_t S2LP_HW_init(void) {
    // Local variables.
    S2LP_status_t status = S2LP_SUCCESS;
    SPI_status_t spi_status = SPI_SUCCESS;
    SPI_configuration_t spi_config;
    // Init SPI.
    spi_config.baud_rate_prescaler = SPI_BAUD_RATE_PRESCALER_2;
    spi_config.data_format = SPI_DATA_FORMAT_8_BITS;
    spi_config.clock_polarity = SPI_CLOCK_POLARITY_LOW;
    spi_status = SPI_init(S2LP_HW_SPI_INSTANCE, &GPIO_S2LP_SPI, &spi_config);
    SPI_exit_error(S2LP_ERROR_BASE_SPI);
    // Configure GPIOs as input
#ifdef HW1_1
    GPIO_configure(&GPIO_S2LP_SDN, GPIO_MODE_ANALOG, GPIO_TYPE_OPEN_DRAIN, GPIO_SPEED_LOW, GPIO_PULL_NONE);
#endif
    // Configure chip select pin.
    GPIO_configure(&GPIO_S2LP_CS, GPIO_MODE_OUTPUT, GPIO_TYPE_PUSH_PULL, GPIO_SPEED_LOW, GPIO_PULL_NONE);
    GPIO_write(&GPIO_S2LP_CS, 1);
errors:
    return status;
}

/*******************************************************************/
S2LP_status_t S2LP_HW_de_init(void) {
    // Local variables.
    S2LP_status_t status = S2LP_SUCCESS;
    SPI_status_t spi_status = SPI_SUCCESS;
    // Release chip select pin.
    GPIO_write(&GPIO_S2LP_CS, 0);
    // Release SPI.
    spi_status = SPI_de_init(S2LP_HW_SPI_INSTANCE, &GPIO_S2LP_SPI);
    SPI_exit_error(S2LP_ERROR_BASE_SPI);
errors:
    return status;
}

/*******************************************************************/
S2LP_status_t S2LP_HW_spi_write_read_8(uint8_t* tx_data, uint8_t* rx_data, uint8_t transfer_size) {
    // Local variables.
    S2LP_status_t status = S2LP_SUCCESS;
    SPI_status_t spi_status = SPI_SUCCESS;
    // CS low.
    GPIO_write(&GPIO_S2LP_CS, 0);
    // SPI transfer.
    spi_status = SPI_write_read_8(S2LP_HW_SPI_INSTANCE, tx_data, rx_data, transfer_size);
    SPI_exit_error(S2LP_ERROR_BASE_SPI);
errors:
    // CS high.
    GPIO_write(&GPIO_S2LP_CS, 1);
    return status;
}

/*******************************************************************/
S2LP_status_t S2LP_HW_set_sdn_gpio(uint8_t state) {
    // Local variables.
    S2LP_status_t status = S2LP_SUCCESS;
    // Set pin.
#ifdef HW1_0
    UNUSED(state);
#else
    GPIO_write(&GPIO_S2LP_SDN, state);
#endif
    return status;
}

/*******************************************************************/
S2LP_status_t S2LP_HW_delay_milliseconds(uint32_t delay_ms) {
    // Local variables.
    S2LP_status_t status = S2LP_SUCCESS;
    LPTIM_status_t lptim_status = LPTIM_SUCCESS;
    // Perform delay.
    lptim_status = LPTIM_delay_milliseconds(delay_ms, LPTIM_DELAY_MODE_SLEEP);
    LPTIM_exit_error(S2LP_ERROR_BASE_DELAY);
errors:
    return status;
}

#endif /* S2LP_DRIVER_DISABLE */
