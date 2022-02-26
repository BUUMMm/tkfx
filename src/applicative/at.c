/*
 * at.c
 *
 *  Created on: 18 apr. 2020
 *      Author: Ludo
 */

#include "at.h"

#include "adc.h"
#include "addon_sigfox_rf_protocol_api.h"
#include "aes.h"
#include "flash_reg.h"
#include "i2c.h"
#include "lpuart.h"
#include "lptim.h"
#include "mapping.h"
#include "math.h"
#include "mma8653fc.h"
#include "mode.h"
#include "neom8n.h"
#include "nvic.h"
#include "nvm.h"
#include "parser.h"
#include "rf_api.h"
#include "s2lp.h"
#include "sht3x.h"
#include "sigfox_api.h"
#include "spi.h"
#include "string.h"
#include "usart.h"

#ifdef ATM

/*** AT local macros ***/

// Enabled commands.
#define AT_COMMANDS_GPS
#define AT_COMMANDS_SENSORS
#define AT_COMMANDS_NVM
#define AT_COMMANDS_SIGFOX
#define AT_COMMANDS_CW
#define AT_COMMANDS_TEST_MODES
// Common macros.
#define AT_COMMAND_LENGTH_MIN			2
#define AT_COMMAND_BUFFER_LENGTH		128
#define AT_RESPONSE_BUFFER_LENGTH		128
#define AT_STRING_VALUE_BUFFER_LENGTH	16
// Input commands without parameter.
#define AT_COMMAND_TEST					"AT"
#define AT_COMMAND_ADC					"AT$ADC?"
#define AT_COMMAND_THS					"AT$THS?"
#define AT_COMMAND_ACC					"AT$ACC?"
#define AT_COMMAND_ID					"AT$ID?"
#define AT_COMMAND_KEY					"AT$KEY?"
#define AT_COMMAND_NVMR					"AT$NVMR"
#define AT_COMMAND_OOB					"AT$SO"
// Input commands with parameters (headers).
#define AT_HEADER_ACC					"AT$ACC="
#define AT_HEADER_GPS					"AT$GPS="
#define AT_HEADER_NVM					"AT$NVM="
#define AT_HEADER_ID					"AT$ID="
#define AT_HEADER_KEY					"AT$KEY="
#define AT_HEADER_SF					"AT$SF="
#define AT_HEADER_SB					"AT$SB="
#define AT_HEADER_CW					"AT$CW="
#define AT_HEADER_TM					"AT$TM="
// Parameters separator.
#define AT_CHAR_SEPARATOR				','
// Responses.
#define AT_RESPONSE_OK					"OK"
#define AT_RESPONSE_END					"\r\n"
#define AT_RESPONSE_ERROR_PSR			"PSR_ERROR_"
#define AT_RESPONSE_ERROR_SFX			"SFX_ERROR_"
#define AT_RESPONSE_ERROR_APP			"APP_ERROR_"

/*** AT local structures ***/

typedef enum {
	AT_ERROR_SOURCE_PSR,
	AT_ERROR_SOURCE_SFX,
	AT_ERROR_SOURCE_APP
} AT_error_source_t;

typedef enum {
	APP_ERROR_NVM_ADDRESS_OVERFLOW,
	APP_ERROR_RF_FREQUENCY_UNDERFLOW,
	APP_ERROR_RF_FREQUENCY_OVERFLOW,
	APP_ERROR_RF_OUTPUT_POWER_OVERFLOW,
	APP_ERROR_INVALID_RC,
	APP_ERROR_INVALID_TEST_MODE,
	APP_ERROR_GPS_INVALID_TIMEOUT,
	APP_ERROR_GPS_TIMEOUT
} AT_applicative_error_t;

typedef struct {
	// AT command buffer.
	volatile unsigned char at_command_buf[AT_COMMAND_BUFFER_LENGTH];
	volatile unsigned int at_command_buf_idx;
	volatile unsigned char at_line_end_flag;
	PARSER_Context at_parser;
	char at_response_buf[AT_RESPONSE_BUFFER_LENGTH];
	unsigned int at_response_buf_idx;
	unsigned char accelero_measurement_flag;
} AT_context_t;

/*** AT local global variables ***/

static AT_context_t at_ctx;

/*** AT local functions ***/

/* APPEND A STRING TO THE REPONSE BUFFER.
 * @param tx_string:	String to add.
 * @return:				None.
 */
static void AT_ResponseAddString(char* tx_string) {
	// Fill TX buffer with new bytes.
	while (*tx_string) {
		at_ctx.at_response_buf[at_ctx.at_response_buf_idx++] = *(tx_string++);
		// Manage rollover.
		if (at_ctx.at_response_buf_idx >= AT_RESPONSE_BUFFER_LENGTH) {
			at_ctx.at_response_buf_idx = 0;
		}
	}
}

/* APPEND A VALUE TO THE REPONSE BUFFER.
 * @param tx_value:		Value to add.
 * @param format:       Printing format.
 * @param print_prefix: Print base prefix is non zero.
 * @return:				None.
 */
static void AT_ResponseAddValue(int tx_value, STRING_format_t format, unsigned char print_prefix) {
	// Local variables.
	char str_value[AT_STRING_VALUE_BUFFER_LENGTH];
	unsigned char idx = 0;
	// Reset string.
	for (idx=0 ; idx<AT_STRING_VALUE_BUFFER_LENGTH ; idx++) str_value[idx] = '\0';
	// Convert value to string.
	STRING_convert_value(tx_value, format, print_prefix, str_value);
	// Add string.
	AT_ResponseAddString(str_value);
}

/* PRINT OK THROUGH AT INTERFACE.
 * @param:	None.
 * @return:	None.
 */
static void AT_ReplyOk(void) {
	AT_ResponseAddString(AT_RESPONSE_OK);
	AT_ResponseAddString(AT_RESPONSE_END);
}

/* PRINT AN ERROR THROUGH AT INTERFACE.
 * @param error_code:	Error code to display.
 * @return:				None.
 */
static void AT_ReplyError(AT_error_source_t error_source, unsigned int error_code) {
	switch (error_source) {
	case AT_ERROR_SOURCE_PSR:
		AT_ResponseAddString(AT_RESPONSE_ERROR_PSR);
		break;
	case AT_ERROR_SOURCE_SFX:
		AT_ResponseAddString(AT_RESPONSE_ERROR_SFX);
		break;
	case AT_ERROR_SOURCE_APP:
		AT_ResponseAddString(AT_RESPONSE_ERROR_APP);
		break;
	default:
		break;
	}
	AT_ResponseAddValue(error_code, STRING_FORMAT_HEXADECIMAL, 1);
	AT_ResponseAddString(AT_RESPONSE_END);
}

/* PRINT ADC DATA ON AT INTERFACE.
 * @param:	None.
 * @return:	None.
 */
static void AT_PrintAdcData(void) {
	// Local variables.
	unsigned int generic_int = 0;
	signed char tmcu_degrees = 0;
	// Vpv.
	ADC1_get_data(ADC_DATA_IDX_VSRC_MV, &generic_int);
	AT_ResponseAddString("Vsrc=");
	AT_ResponseAddValue(generic_int, STRING_FORMAT_DECIMAL, 0);
	// Vout.
	ADC1_get_data(ADC_DATA_IDX_VCAP_MV, &generic_int);
	AT_ResponseAddString("mV Vcap=");
	AT_ResponseAddValue(generic_int, STRING_FORMAT_DECIMAL, 0);
	// Vmcu.
	ADC1_get_data(ADC_DATA_IDX_VMCU_MV, &generic_int);
	AT_ResponseAddString("uA Vmcu=");
	AT_ResponseAddValue(generic_int, STRING_FORMAT_DECIMAL, 0);
	// Tmcu.
	ADC1_get_tmcu_comp2(&tmcu_degrees);
	AT_ResponseAddString("mV Tmcu=");
	AT_ResponseAddValue((signed int) tmcu_degrees, STRING_FORMAT_DECIMAL, 0);
	AT_ResponseAddString("dC");
	AT_ResponseAddString(AT_RESPONSE_END);
}

/* PRINT SIGFOX DOWNLINK DATA ON AT INTERFACE.
 * @param sfx_downlink_data:	Downlink data to print.
 * @return:						None.
 */
static void AT_PrintDownlinkData(sfx_u8* sfx_downlink_data) {
	AT_ResponseAddString("+RX=");
	unsigned char byte_idx = 0;
	for (byte_idx=0 ; byte_idx<8 ; byte_idx++) {
		AT_ResponseAddValue(sfx_downlink_data[byte_idx], STRING_FORMAT_HEXADECIMAL, 0);
	}
	AT_ResponseAddString(AT_RESPONSE_END);
}

/* PRINT GPS POSITION ON USART.
 * @param gps_position:	Pointer to GPS position to print.
 * @return:				None.
 */
static void AT_PrintNEOM8N_position_t(NEOM8N_position_t* gps_position, unsigned int gps_fix_duration) {
	// Latitude.
	AT_ResponseAddString("Lat=");
	AT_ResponseAddValue((gps_position -> lat_degrees), STRING_FORMAT_DECIMAL, 0);
	AT_ResponseAddString("d");
	AT_ResponseAddValue((gps_position -> lat_minutes), STRING_FORMAT_DECIMAL, 0);
	AT_ResponseAddString("'");
	AT_ResponseAddValue((gps_position -> lat_seconds), STRING_FORMAT_DECIMAL, 0);
	AT_ResponseAddString("''-");
	AT_ResponseAddString(((gps_position -> lat_north_flag) == 0) ? "S" : "N");
	// Longitude.
	AT_ResponseAddString(" Long=");
	AT_ResponseAddValue((gps_position -> long_degrees), STRING_FORMAT_DECIMAL, 0);
	AT_ResponseAddString("d");
	AT_ResponseAddValue((gps_position -> long_minutes), STRING_FORMAT_DECIMAL, 0);
	AT_ResponseAddString("'");
	AT_ResponseAddValue((gps_position -> long_seconds), STRING_FORMAT_DECIMAL, 0);
	AT_ResponseAddString("''-");
	AT_ResponseAddString(((gps_position -> long_east_flag) == 0) ? "W" : "E");
	// Altitude.
	AT_ResponseAddString(" Alt=");
	AT_ResponseAddValue((gps_position -> altitude), STRING_FORMAT_DECIMAL, 0);
	AT_ResponseAddString("m Fix=");
	AT_ResponseAddValue(gps_fix_duration, STRING_FORMAT_DECIMAL, 0);
	AT_ResponseAddString("s");
	AT_ResponseAddString(AT_RESPONSE_END);
}

/* PRINT ACCELEROMETER DATA ON USART.
 * @param:	None.
 * @return:	None.
 */
static void AT_PrintAcceleroData(void) {
	// Get data.
	signed int x = 0;
	signed int y = 0;
	signed int z = 0;
	MMA8653FC_get_data(&x, &y, &z);
	// Print data.
	AT_ResponseAddString("x=");
	AT_ResponseAddValue(x, STRING_FORMAT_DECIMAL, 0);
	AT_ResponseAddString(" y=");
	AT_ResponseAddValue(y, STRING_FORMAT_DECIMAL, 0);
	AT_ResponseAddString(" z=");
	AT_ResponseAddValue(z, STRING_FORMAT_DECIMAL, 0);
	AT_ResponseAddString(AT_RESPONSE_END);
}

/* PARSE THE CURRENT AT COMMAND BUFFER.
 * @param:	None.
 * @return:	None.
 */
static void AT_DecodeRxBuffer(void) {
	// Local variables.
	PARSER_Status parser_status = PARSER_ERROR_UNKNOWN_COMMAND;
	int generic_int_1 = 0;
	int generic_int_2 = 0;
	unsigned char generic_byte = 0;
	unsigned char generic_byte_array_1[AES_BLOCK_SIZE];
	unsigned char idx = 0;
	unsigned char extracted_length = 0;
#ifdef AT_COMMANDS_GPS
	NEOM8N_position_t gps_position;
	NEOM8N_status_t get_position_result;
#endif
#ifdef AT_COMMANDS_SENSORS
	signed char generic_signed_byte = 0;
#endif
#ifdef AT_COMMANDS_SIGFOX
	sfx_error_t sfx_error = 0;
	sfx_rc_t rc1 = RC1;
	unsigned char generic_byte_array_2[AES_BLOCK_SIZE];
#endif
#ifdef AT_COMMANDS_CW
	int generic_int_3 = 0;
#endif
	// Empty or too short command.
	if (at_ctx.at_command_buf_idx < AT_COMMAND_LENGTH_MIN) {
		AT_ReplyError(AT_ERROR_SOURCE_PSR, PARSER_ERROR_UNKNOWN_COMMAND);
	}
	else {
		// Update parser length.
		at_ctx.at_parser.rx_buf_length = (at_ctx.at_command_buf_idx - 1); // To ignore line end.
		// Test command AT<CR>.
		if (PARSER_compare_command(&at_ctx.at_parser, AT_COMMAND_TEST) == PARSER_SUCCESS) {
			AT_ReplyOk();
		}
#ifdef AT_COMMANDS_GPS
		// GPS command AT$GPS=<timeout_seconds><CR>.
		else if (PARSER_compare_header(&at_ctx.at_parser, AT_HEADER_GPS) == PARSER_SUCCESS) {
			// Search timeout parameter.
			parser_status = PARSER_get_parameter(&at_ctx.at_parser, PARSER_PARAMETER_TYPE_DECIMAL, AT_CHAR_SEPARATOR, 1, &generic_int_1);
			if (parser_status == PARSER_SUCCESS) {
				// Start GPS fix.
				LPUART1_power_on();
				get_position_result = NEOM8N_get_position(&gps_position, generic_int_1, 0, &generic_int_2);
				LPUART1_power_off();
				switch (get_position_result) {
				case NEOM8N_SUCCESS:
					AT_PrintNEOM8N_position_t(&gps_position, generic_int_2);
					break;
				case NEOM8N_ERROR_TIMEOUT:
					AT_ReplyError(AT_ERROR_SOURCE_APP, APP_ERROR_GPS_TIMEOUT);
					break;
				default:
					break;
				}
			}
			else {
				AT_ReplyError(AT_ERROR_SOURCE_PSR, parser_status); // Error in timeout parameter.
			}
		}
#endif
#ifdef AT_COMMANDS_SENSORS
		// ADC command AT$ADC?<CR>.
		else if (PARSER_compare_command(&at_ctx.at_parser, AT_COMMAND_ADC) == PARSER_SUCCESS) {
			// Perform ADC measurements.
			ADC1_power_on();
			ADC1_perform_measurements();
			ADC1_power_off();
			AT_PrintAdcData();
		}
		// Temperature and humidity sensor command AT$THS?<CR>.
		else if (PARSER_compare_command(&at_ctx.at_parser, AT_COMMAND_THS) == PARSER_SUCCESS) {
			// Perform measurements.
			I2C1_power_on();
			SHT3X_perform_measurements();
			I2C1_power_off();
			SHT3X_get_temperature_comp2(&generic_signed_byte);
			SHT3X_get_humidity(&generic_byte);
			// Print results.
			AT_ResponseAddString("T=");
			AT_ResponseAddValue(generic_signed_byte, STRING_FORMAT_DECIMAL, 0);
			AT_ResponseAddString("dC H=");
			AT_ResponseAddValue(generic_byte, STRING_FORMAT_DECIMAL, 0);
			AT_ResponseAddString("%");
			AT_ResponseAddString(AT_RESPONSE_END);
		}
		// Accelerometer check command AT$ACC?<CR>.
		else if (PARSER_compare_command(&at_ctx.at_parser, AT_COMMAND_ACC) == PARSER_SUCCESS) {
			// Get sensor ID.
			I2C1_power_on();
			generic_byte = MMA8653FC_get_id();
			I2C1_power_off();
			// Print results.
			AT_ResponseAddString("WhoAmI=");
			AT_ResponseAddValue(generic_byte, STRING_FORMAT_HEXADECIMAL, 0);
			AT_ResponseAddString(AT_RESPONSE_END);
		}
		// Accelerometer data command AT$ACC=<enable><CR>.
		else if (PARSER_compare_header(&at_ctx.at_parser, AT_HEADER_ACC) == PARSER_SUCCESS) {
			// Get enable parameter.
			parser_status = PARSER_get_parameter(&at_ctx.at_parser, PARSER_PARAMETER_TYPE_BOOLEAN, AT_CHAR_SEPARATOR, 1, &generic_int_1);
			if (parser_status == PARSER_SUCCESS) {
				// Check enable bit.
				if (generic_int_1 == 0) {
					// Stop measurement.
					I2C1_power_off();
					at_ctx.accelero_measurement_flag = 0;
					AT_ReplyOk();
				}
				else {
					// Start measurement.
					I2C1_power_on();
					at_ctx.accelero_measurement_flag = 1;
					AT_ReplyOk();
				}
			}
			else {
				// Error in enable parameter.
				AT_ReplyError(AT_ERROR_SOURCE_PSR, parser_status);
			}
		}
#endif
#ifdef AT_COMMANDS_NVM
		// NVM reset command AT$NVMR<CR>.
		else if (PARSER_compare_command(&at_ctx.at_parser, AT_COMMAND_NVMR) == PARSER_SUCCESS) {
			// Reset all NVM field to default value.
			NVM_enable();
			NVM_reset_default();
			NVM_disable();
			AT_ReplyOk();
		}
		// NVM read command AT$NVM=<address_offset><CR>.
		else if (PARSER_compare_header(&at_ctx.at_parser, AT_HEADER_NVM) == PARSER_SUCCESS) {
			parser_status = PARSER_get_parameter(&at_ctx.at_parser, PARSER_PARAMETER_TYPE_DECIMAL, AT_CHAR_SEPARATOR, 1, &generic_int_1);
			if (parser_status == PARSER_SUCCESS) {
				// Check if address is reachable.
				if (generic_int_1 < EEPROM_SIZE) {
					// Read byte at requested address.
					NVM_enable();
					NVM_read_byte(generic_int_1, &generic_byte);
					NVM_disable();
					// Print byte.
					AT_ResponseAddValue(generic_byte, STRING_FORMAT_HEXADECIMAL, 1);
					AT_ResponseAddString(AT_RESPONSE_END);
				}
				else {
					AT_ReplyError(AT_ERROR_SOURCE_APP, APP_ERROR_NVM_ADDRESS_OVERFLOW);
				}
			}
			else {
				AT_ReplyError(AT_ERROR_SOURCE_PSR, parser_status); // Error in address parameter.
			}
		}
		// Get ID command AT$ID?<CR>.
		else if (PARSER_compare_command(&at_ctx.at_parser, AT_COMMAND_ID) == PARSER_SUCCESS) {
			// Retrieve device ID in NVM.
			NVM_enable();
			for (idx=0 ; idx<ID_LENGTH ; idx++) {
				NVM_read_byte((NVM_SIGFOX_ID_ADDRESS_OFFSET + ID_LENGTH - idx - 1), &generic_byte);
				AT_ResponseAddValue(generic_byte, STRING_FORMAT_HEXADECIMAL, (idx==0 ? 1 : 0));
			}
			NVM_disable();
			AT_ResponseAddString(AT_RESPONSE_END);
		}
		// Set ID command AT$ID=<id><CR>.
		else if (PARSER_compare_header(&at_ctx.at_parser, AT_HEADER_ID) == PARSER_SUCCESS) {
			parser_status = PARSER_get_byte_array(&at_ctx.at_parser, AT_CHAR_SEPARATOR, 1, ID_LENGTH, generic_byte_array_1, &extracted_length);
			if (parser_status == PARSER_SUCCESS) {
				// Check length.
				if (extracted_length == ID_LENGTH) {
					// Write device ID in NVM.
					NVM_enable();
					for (idx=0 ; idx<ID_LENGTH ; idx++) {
						NVM_write_byte((NVM_SIGFOX_ID_ADDRESS_OFFSET + ID_LENGTH - idx - 1), generic_byte_array_1[idx]);
					}
					NVM_disable();
					AT_ReplyOk();
				}
				else {
					AT_ReplyError(AT_ERROR_SOURCE_PSR, PARSER_ERROR_PARAMETER_BYTE_ARRAY_INVALID_LENGTH);
				}
			}
			else {
				AT_ReplyError(AT_ERROR_SOURCE_PSR, parser_status); // Error in ID parameter.
			}
		}
		// Get key command AT$KEY?<CR>.
		else if (PARSER_compare_command(&at_ctx.at_parser, AT_COMMAND_KEY) == PARSER_SUCCESS) {
			// Retrieve device key in NVM.
			NVM_enable();
			for (idx=0 ; idx<AES_BLOCK_SIZE ; idx++) {
				NVM_read_byte((NVM_SIGFOX_KEY_ADDRESS_OFFSET + idx), &generic_byte);
				AT_ResponseAddValue(generic_byte, STRING_FORMAT_HEXADECIMAL, (idx==0 ? 1 : 0));
			}
			NVM_disable();
			AT_ResponseAddString(AT_RESPONSE_END);
		}
		// Set key command AT$KEY=<id><CR>.
		else if (PARSER_compare_header(&at_ctx.at_parser, AT_HEADER_KEY) == PARSER_SUCCESS) {
			parser_status = PARSER_get_byte_array(&at_ctx.at_parser, AT_CHAR_SEPARATOR, 1, AES_BLOCK_SIZE, generic_byte_array_1, &extracted_length);
			if (parser_status == PARSER_SUCCESS) {
				// Check length.
				if (extracted_length == AES_BLOCK_SIZE) {
					// Write device key in NVM.
					NVM_enable();
					for (idx=0 ; idx<AES_BLOCK_SIZE ; idx++) {
						NVM_write_byte((NVM_SIGFOX_KEY_ADDRESS_OFFSET + idx), generic_byte_array_1[idx]);
					}
					NVM_disable();
					AT_ReplyOk();
				}
				else {
					AT_ReplyError(AT_ERROR_SOURCE_PSR, PARSER_ERROR_PARAMETER_BYTE_ARRAY_INVALID_LENGTH);
				}
			}
			else {
				AT_ReplyError(AT_ERROR_SOURCE_PSR, parser_status); // Error in key parameter.
			}
		}
#endif
#ifdef AT_COMMANDS_SIGFOX
		// Sigfox send OOB command AT$SO<CR>.
		else if (PARSER_compare_command(&at_ctx.at_parser, AT_COMMAND_OOB) == PARSER_SUCCESS) {
			// Send Sigfox OOB frame.
			sfx_error = SIGFOX_API_open(&rc1);
			if (sfx_error == SFX_ERR_NONE) {
				sfx_error = SIGFOX_API_send_outofband(SFX_OOB_SERVICE);
			}
			SIGFOX_API_close();
			if (sfx_error == SFX_ERR_NONE) {
				AT_ReplyOk();
			}
			else {
				AT_ReplyError(AT_ERROR_SOURCE_SFX, sfx_error);
			}
		}
		// Sigfox send bit command AT$SB=<bit>,<downlink_request><CR>.
		else if (PARSER_compare_header(&at_ctx.at_parser, AT_HEADER_SB) == PARSER_SUCCESS) {
			// First try with 2 parameters.
			parser_status = PARSER_get_parameter(&at_ctx.at_parser, PARSER_PARAMETER_TYPE_BOOLEAN, AT_CHAR_SEPARATOR, 0, &generic_int_1);
			if (parser_status == PARSER_SUCCESS) {
				// Try parsing downlink request parameter.
				parser_status =  PARSER_get_parameter(&at_ctx.at_parser, PARSER_PARAMETER_TYPE_BOOLEAN, AT_CHAR_SEPARATOR, 1, &generic_int_2);
				if (parser_status == PARSER_SUCCESS) {
					// Send Sigfox bit with specified downlink request.
					sfx_error = SIGFOX_API_open(&rc1);
					if (sfx_error == SFX_ERR_NONE) {
						sfx_error = SIGFOX_API_send_bit((sfx_bool) generic_int_1, generic_byte_array_2, 2, (sfx_bool) generic_int_2);
					}
					SIGFOX_API_close();
					if (sfx_error == SFX_ERR_NONE) {
						if (generic_int_2 != 0) {
							AT_PrintDownlinkData(generic_byte_array_2);
						}
						AT_ReplyOk();
					}
					else {
						AT_ReplyError(AT_ERROR_SOURCE_SFX, sfx_error);
					}
				}
				else {
					AT_ReplyError(AT_ERROR_SOURCE_PSR, parser_status); // Error in downlink request parameter.
				}
			}
			else {
				// Try with 1 parameter.
				parser_status = PARSER_get_parameter(&at_ctx.at_parser, PARSER_PARAMETER_TYPE_BOOLEAN, AT_CHAR_SEPARATOR, 1, &generic_int_1);
				if (parser_status == PARSER_SUCCESS) {
					// Send Sigfox bit with no downlink request (by default).
					sfx_error = SIGFOX_API_open(&rc1);
					if (sfx_error == SFX_ERR_NONE) {
						sfx_error = SIGFOX_API_send_bit((sfx_bool) generic_int_1, generic_byte_array_2, 2, 0);
					}
					SIGFOX_API_close();
					if (sfx_error == SFX_ERR_NONE) {
						AT_ReplyOk();
					}
					else {
						AT_ReplyError(AT_ERROR_SOURCE_SFX, sfx_error);
					}
				}
				else {
					AT_ReplyError(AT_ERROR_SOURCE_PSR, parser_status); // Error in data parameter.
				}
			}
		}
		// Sigfox send frame command AT$SF=<data>,<downlink_request><CR>.
		else if (PARSER_compare_header(&at_ctx.at_parser, AT_HEADER_SF) == PARSER_SUCCESS) {
			// First try with 2 parameters.
			parser_status = PARSER_get_byte_array(&at_ctx.at_parser, AT_CHAR_SEPARATOR, 0, 12, generic_byte_array_1, &extracted_length);
			if (parser_status == PARSER_SUCCESS) {
				// Try parsing downlink request parameter.
				parser_status =  PARSER_get_parameter(&at_ctx.at_parser, PARSER_PARAMETER_TYPE_BOOLEAN, AT_CHAR_SEPARATOR, 1, &generic_int_2);
				if (parser_status == PARSER_SUCCESS) {
					// Send Sigfox frame with specified downlink request.
					sfx_error = SIGFOX_API_open(&rc1);
					if (sfx_error == SFX_ERR_NONE) {
						sfx_error = SIGFOX_API_send_frame(generic_byte_array_1, extracted_length, generic_byte_array_2, 2, (sfx_bool) generic_int_2);
					}
					SIGFOX_API_close();
					if (sfx_error == SFX_ERR_NONE) {
						if (generic_int_2 != 0) {
							AT_PrintDownlinkData(generic_byte_array_2);
						}
						AT_ReplyOk();
					}
					else {
						AT_ReplyError(AT_ERROR_SOURCE_SFX, sfx_error);
					}
				}
				else {
					AT_ReplyError(AT_ERROR_SOURCE_PSR, parser_status); // Error in downlink request parameter.
				}
			}
			else {
				// Try with 1 parameter.
				parser_status = PARSER_get_byte_array(&at_ctx.at_parser, AT_CHAR_SEPARATOR, 1, 12, generic_byte_array_1, &extracted_length);
				if (parser_status == PARSER_SUCCESS) {
					// Send Sigfox frame with no downlink request (by default).
					sfx_error = SIGFOX_API_open(&rc1);
					if (sfx_error == SFX_ERR_NONE) {
						sfx_error = SIGFOX_API_send_frame(generic_byte_array_1, extracted_length, generic_byte_array_2, 2, 0);
					}
					SIGFOX_API_close();
					if (sfx_error == SFX_ERR_NONE) {
						AT_ReplyOk();
					}
					else {
						AT_ReplyError(AT_ERROR_SOURCE_SFX, sfx_error);
					}
				}
				else {
					AT_ReplyError(AT_ERROR_SOURCE_PSR, parser_status); // Error in data parameter.
				}
			}
		}
#endif
#ifdef AT_COMMANDS_CW
		// CW command AT$CW=<frequency_hz>,<enable>,<output_power_dbm><CR>.
		else if (PARSER_compare_header(&at_ctx.at_parser, AT_HEADER_CW) == PARSER_SUCCESS) {
			// Search frequency parameter.
			parser_status = PARSER_get_parameter(&at_ctx.at_parser, PARSER_PARAMETER_TYPE_DECIMAL, AT_CHAR_SEPARATOR, 0, &generic_int_1);
			if (parser_status == PARSER_SUCCESS) {
				// First try with 3 parameters.
				parser_status = PARSER_get_parameter(&at_ctx.at_parser, PARSER_PARAMETER_TYPE_BOOLEAN, AT_CHAR_SEPARATOR, 0, &generic_int_2);
				if (parser_status != PARSER_SUCCESS) {
					// Power is not given, try to parse enable as last parameter.
					parser_status = PARSER_get_parameter(&at_ctx.at_parser, PARSER_PARAMETER_TYPE_BOOLEAN, AT_CHAR_SEPARATOR, 1, &generic_int_2);
					if (parser_status == PARSER_SUCCESS) {
						// CW with current output power.
						SIGFOX_API_stop_continuous_transmission();
						if (generic_int_2 != 0) {
							SIGFOX_API_start_continuous_transmission(generic_int_1, SFX_NO_MODULATION);
						}
						AT_ReplyOk();
					}
					else {
						// Error in enable parameter.
						AT_ReplyError(AT_ERROR_SOURCE_PSR, parser_status);
					}
				}
				else if (parser_status == PARSER_SUCCESS) {
					// There is a third parameter, try to parse power.
					parser_status = PARSER_get_parameter(&at_ctx.at_parser, PARSER_PARAMETER_TYPE_DECIMAL, AT_CHAR_SEPARATOR, 1, &generic_int_3);
					if (parser_status == PARSER_SUCCESS) {
						// CW with given output power.
						SIGFOX_API_stop_continuous_transmission();
						if (generic_int_2 != 0) {
							RF_API_SetCwOutputPower((signed char) generic_int_3);
							SIGFOX_API_start_continuous_transmission(generic_int_1, SFX_NO_MODULATION);
						}
						AT_ReplyOk();
					}
					else {
						AT_ReplyError(AT_ERROR_SOURCE_PSR, parser_status); // Error in power parameter.
					}
				}
				else {
					AT_ReplyError(AT_ERROR_SOURCE_PSR, parser_status); // Error in enable parameter.
				}
			}
			else {
				AT_ReplyError(AT_ERROR_SOURCE_PSR, parser_status); // Error in frequency parameter.
			}
		}
#endif
#ifdef AT_COMMANDS_TEST_MODES
		// Sigfox test mode command AT$TM=<rc>,<test_mode><CR>.
		else if (PARSER_compare_header(&at_ctx.at_parser, AT_HEADER_TM) == PARSER_SUCCESS) {
			// Search RC parameter.
			parser_status = PARSER_get_parameter(&at_ctx.at_parser, PARSER_PARAMETER_TYPE_DECIMAL, AT_CHAR_SEPARATOR, 0, &generic_int_1);
			if (parser_status == PARSER_SUCCESS) {
				// Check value.
				if (generic_int_1 < SFX_RC_LIST_MAX_SIZE) {
					// Search test mode number.
					parser_status =  PARSER_get_parameter(&at_ctx.at_parser, PARSER_PARAMETER_TYPE_DECIMAL, AT_CHAR_SEPARATOR, 1, &generic_int_2);
					if (parser_status == PARSER_SUCCESS) {
						// Check parameters.
						if (generic_int_2 <= SFX_TEST_MODE_NVM) {
							// Call test mode function wth public key.
							sfx_error = ADDON_SIGFOX_RF_PROTOCOL_API_test_mode(generic_int_1, generic_int_2);
							if (sfx_error == SFX_ERR_NONE) {
								AT_ReplyOk();
							}
							else {
								AT_ReplyError(AT_ERROR_SOURCE_SFX, sfx_error);
							}
						}
						else {
							AT_ReplyError(AT_ERROR_SOURCE_APP, APP_ERROR_INVALID_TEST_MODE);
						}
					}
					else {
						AT_ReplyError(AT_ERROR_SOURCE_PSR, parser_status); // Error in test_mode parameter.
					}
				}
				else {
					AT_ReplyError(AT_ERROR_SOURCE_APP, APP_ERROR_INVALID_RC);
				}
			}
			else {
				AT_ReplyError(AT_ERROR_SOURCE_PSR, parser_status); // Error in RC parameter.
			}
		}
#endif
		// Unknown command.
		else {
			AT_ReplyError(AT_ERROR_SOURCE_PSR, PARSER_ERROR_UNKNOWN_COMMAND);
		}
	}
	// Send response.
	USART2_send_string(at_ctx.at_response_buf);
	// Reset AT parser.
	AT_init();
}

/*** AT functions ***/

/* INIT AT MANAGER.
 * @param:	None.
 * @return:	None.
 */
void AT_init(void) {
	// Init context.
	unsigned int idx = 0;
	for (idx=0 ; idx<AT_COMMAND_BUFFER_LENGTH ; idx++) at_ctx.at_command_buf[idx] = '\0';
	at_ctx.at_command_buf_idx = 0;
	at_ctx.at_line_end_flag = 0;
	for (idx=0 ; idx<AT_RESPONSE_BUFFER_LENGTH ; idx++) at_ctx.at_response_buf[idx] = '\0';
	at_ctx.at_response_buf_idx = 0;
	at_ctx.accelero_measurement_flag = 0;
	// Parsing variables.
	at_ctx.at_parser.rx_buf = (unsigned char*) at_ctx.at_command_buf;
	at_ctx.at_parser.rx_buf_length = 0;
	at_ctx.at_parser.separator_idx = 0;
	at_ctx.at_parser.start_idx = 0;
	// Enable USART interrupt.
	NVIC_enable_interrupt(NVIC_IT_USART2);
}

/* MAIN TASK OF AT COMMAND MANAGER.
 * @param:	None.
 * @return:	None.
 */
void AT_task(void) {
	// Trigger decoding function if line end found.
	if (at_ctx.at_line_end_flag) {
		AT_DecodeRxBuffer();
	}
	// Perform accelero measurement if required.
	if (at_ctx.accelero_measurement_flag != 0) {
		AT_PrintAcceleroData();
	}
}

/* FILL AT COMMAND BUFFER WITH A NEW BYTE (CALLED BY USART INTERRUPT).
 * @param rx_byte:	Incoming byte.
 * @return:			None.
 */
void AT_fill_rx_buffer(unsigned char rx_byte) {
	// Append byte if LF flag is not allready set.
	if (at_ctx.at_line_end_flag == 0) {
		// Store new byte.
		at_ctx.at_command_buf[at_ctx.at_command_buf_idx] = rx_byte;
		// Manage index.
		at_ctx.at_command_buf_idx++;
		if (at_ctx.at_command_buf_idx >= AT_COMMAND_BUFFER_LENGTH) {
			at_ctx.at_command_buf_idx = 0;
		}
	}
	// Set LF flag to trigger decoding.
	if ((rx_byte == STRING_CHAR_CR) || (rx_byte == STRING_CHAR_LF)) {
		at_ctx.at_line_end_flag = 1;
	}
}

/* PRINT SIGFOX LIBRARY RESULT.
 * @param test_result:	Test result.
 * @param rssi:			Downlink signal rssi in dBm.
 */
void AT_print_test_result(unsigned char test_result, int rssi_dbm) {
	// Check result.
	if (test_result == 0) {
		AT_ResponseAddString("Test failed.");
	}
	else {
		AT_ResponseAddString("Test passed. RSSI=");
		AT_ResponseAddValue(rssi_dbm, STRING_FORMAT_DECIMAL, 0);
		AT_ResponseAddString("dBm");
	}
	AT_ResponseAddString(AT_RESPONSE_END);
	// Send response.
	USART2_send_string(at_ctx.at_response_buf);
	// Reset AT parser.
	AT_init();
}

#endif
