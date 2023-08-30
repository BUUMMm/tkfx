/*
 * at.c
 *
 *  Created on: 18 apr. 2020
 *      Author: Ludo
 */

#include "at.h"

#include "adc.h"
#include "error.h"
#include "lptim.h"
#include "manuf/mcu_api.h"
#include "manuf/rf_api.h"
#include "math.h"
#include "mma8653fc.h"
#include "mode.h"
#include "neom8n.h"
#include "nvic.h"
#include "nvm.h"
#include "parser.h"
#include "pwr.h"
#include "s2lp.h"
#include "sht3x.h"
#include "sigfox_ep_api.h"
#include "sigfox_ep_addon_rfp_api.h"
#include "sigfox_rc.h"
#include "sigfox_types.h"
#include "string.h"
#include "types.h"
#include "usart.h"
#include "version.h"

/*** AT local macros ***/

// Commands.
#define AT_COMMAND_BUFFER_SIZE			128
// Parameters separator.
#define AT_CHAR_SEPARATOR				','
// Replies.
#define AT_REPLY_BUFFER_SIZE			128
#define AT_REPLY_END					"\r\n"
#define AT_REPLY_TAB					"     "
#define AT_STRING_VALUE_BUFFER_SIZE		16
// Duration of RSSI command.
#define AT_RSSI_REPORT_PERIOD_MS		500

/*** AT callbacks declaration ***/

#ifdef ATM
/*******************************************************************/
static void _AT_print_ok(void);
static void _AT_print_command_list(void);
static void _AT_print_sw_version(void);
static void _AT_print_error_stack(void);
/*******************************************************************/
static void _AT_adc_callback(void);
static void _AT_ths_callback(void);
static void _AT_acc_callback(void);
/*******************************************************************/
static void _AT_gps_callback(void);
/*******************************************************************/
static void _AT_nvm_callback(void);
static void _AT_get_id_callback(void);
static void _AT_set_id_callback(void);
static void _AT_get_key_callback(void);
static void _AT_set_key_callback(void);
/*******************************************************************/
static void _AT_sb_callback(void);
static void _AT_sf_callback(void);
/*******************************************************************/
static void _AT_tm_callback(void);
/*******************************************************************/
static void _AT_cw_callback(void);
static void _AT_rssi_callback(void);
#endif

/*** AT local structures ***/

#ifdef ATM
/*******************************************************************/
typedef struct {
	PARSER_mode_t mode;
	char_t* syntax;
	char_t* parameters;
	char_t* description;
	void (*callback)(void);
} AT_command_t;
#endif

#ifdef ATM
/*******************************************************************/
typedef struct {
	// Command.
	volatile char_t command[AT_COMMAND_BUFFER_SIZE];
	volatile uint32_t command_size;
	volatile uint8_t line_end_flag;
	PARSER_context_t parser;
	// Reply.
	char_t reply[AT_REPLY_BUFFER_SIZE];
	uint32_t reply_size;
} AT_context_t;
#endif

/*** AT local global variables ***/

#ifdef ATM
static const AT_command_t AT_COMMAND_LIST[] = {
	{PARSER_MODE_COMMAND, "AT", STRING_NULL, "Ping command", _AT_print_ok},
	{PARSER_MODE_COMMAND, "AT?", STRING_NULL, "AT commands list", _AT_print_command_list},
	{PARSER_MODE_COMMAND, "AT$V?", STRING_NULL, "Get SW version", _AT_print_sw_version},
	{PARSER_MODE_COMMAND, "AT$ERROR?", STRING_NULL, "Read error stack", _AT_print_error_stack},
	{PARSER_MODE_COMMAND, "AT$RST", STRING_NULL, "Reset MCU", PWR_software_reset},
	{PARSER_MODE_COMMAND, "AT$ADC?", STRING_NULL, "Get ADC data", _AT_adc_callback},
	{PARSER_MODE_COMMAND, "AT$THS?", STRING_NULL, "Get temperature and humidity", _AT_ths_callback},
	{PARSER_MODE_COMMAND, "AT$ACC?", STRING_NULL, "Read accelerometer chip ID", _AT_acc_callback},
	{PARSER_MODE_HEADER,  "AT$GPS=", "timeout[s]", "Get GPS position", _AT_gps_callback},
	{PARSER_MODE_HEADER,  "AT$NVM=", "address[dec]", "Get NVM data", _AT_nvm_callback},
	{PARSER_MODE_COMMAND, "AT$ID?", STRING_NULL, "Get Sigfox EP ID", _AT_get_id_callback},
	{PARSER_MODE_HEADER,  "AT$ID=", "id[hex]", "Set Sigfox EP ID", _AT_set_id_callback},
	{PARSER_MODE_COMMAND, "AT$KEY?", STRING_NULL, "Get Sigfox EP key", _AT_get_key_callback},
	{PARSER_MODE_HEADER,  "AT$KEY=", "key[hex]", "Set Sigfox EP key", _AT_set_key_callback},
	{PARSER_MODE_HEADER,  "AT$SB=", "data[bit],(bidir_flag[bit])", "Sigfox send bit", _AT_sb_callback},
	{PARSER_MODE_HEADER,  "AT$SF=", "data[hex],(bidir_flag[bit])", "Sigfox send frame", _AT_sf_callback},
	{PARSER_MODE_HEADER,  "AT$TM=", "rc_index[dec],test_mode[dec]", "Sigfox RFP test mode", _AT_tm_callback},
	{PARSER_MODE_HEADER,  "AT$CW=", "frequency[hz],enable[bit],(output_power[dbm])", "Continuous wave", _AT_cw_callback},
	{PARSER_MODE_HEADER,  "AT$RSSI=", "frequency[hz],duration[s]", "Continuous RSSI measurement", _AT_rssi_callback},
};
static AT_context_t at_ctx;
#endif

/*** AT local functions ***/

/*******************************************************************/
#define _AT_exit_error(status, success, base) { \
	if (status != success) { \
		_AT_print_error(base + status); \
		ERROR_stack_add(base + status); \
		goto errors; \
	} \
}

#ifdef ATM
/*******************************************************************/
#define _AT_reply_add_char(character) { \
	at_ctx.reply[at_ctx.reply_size] = character; \
	at_ctx.reply_size = (at_ctx.reply_size + 1) % AT_REPLY_BUFFER_SIZE; \
}
#endif

#ifdef ATM
/*******************************************************************/
static void _AT_fill_rx_buffer(uint8_t rx_byte) {
	// Append byte if line end flag is not allready set.
	if (at_ctx.line_end_flag == 0) {
		// Check ending characters.
		if ((rx_byte == STRING_CHAR_CR) || (rx_byte == STRING_CHAR_LF)) {
			at_ctx.command[at_ctx.command_size] = STRING_CHAR_NULL;
			at_ctx.line_end_flag = 1;
		}
		else {
			// Store new byte.
			at_ctx.command[at_ctx.command_size] = rx_byte;
			// Manage index.
			at_ctx.command_size = (at_ctx.command_size + 1) % AT_COMMAND_BUFFER_SIZE;
		}
	}
}
#endif

#ifdef ATM
/*******************************************************************/
static void _AT_reply_add_string(char_t* tx_string) {
	// Fill TX buffer with new bytes.
	while (*tx_string) {
		_AT_reply_add_char(*(tx_string++));
	}
}
#endif

#ifdef ATM
/*******************************************************************/
static void _AT_reply_add_value(int32_t tx_value, STRING_format_t format, uint8_t print_prefix) {
	// Local variables.
	STRING_status_t string_status = STRING_SUCCESS;
	char_t str_value[AT_STRING_VALUE_BUFFER_SIZE];
	uint8_t idx = 0;
	// Reset string.
	for (idx=0 ; idx<AT_STRING_VALUE_BUFFER_SIZE ; idx++) str_value[idx] = STRING_CHAR_NULL;
	// Convert value to string.
	string_status = STRING_value_to_string(tx_value, format, print_prefix, str_value);
	STRING_stack_error();
	// Add string.
	_AT_reply_add_string(str_value);
}
#endif

#ifdef ATM
/*******************************************************************/
static void _AT_reply_send(void) {
	// Local variables.
	USART_status_t usart2_status = USART_SUCCESS;
	// Add ending string.
	_AT_reply_add_string(AT_REPLY_END);
	// Send response over UART.
	usart2_status = USART2_write((uint8_t*) at_ctx.reply, at_ctx.reply_size);
	USART2_stack_error();
	// Flush reply buffer.
	at_ctx.reply_size = 0;
}
#endif

#ifdef ATM
/*******************************************************************/
static void _AT_print_ok(void) {
	_AT_reply_add_string("OK");
	_AT_reply_send();
}
#endif

#ifdef ATM
/*******************************************************************/
static void _AT_print_error(ERROR_code_t error) {
	// Add error to stack.
	ERROR_stack_add(error);
	// Print error.
	_AT_reply_add_string("ERROR_");
	if (error < 0x0100) {
		_AT_reply_add_value(0, STRING_FORMAT_HEXADECIMAL, 1);
		_AT_reply_add_value((int32_t) error, STRING_FORMAT_HEXADECIMAL, 0);
	}
	else {
		_AT_reply_add_value((int32_t) error, STRING_FORMAT_HEXADECIMAL, 1);
	}
	_AT_reply_send();
}
#endif

#ifdef ATM
/*******************************************************************/
static void _AT_print_command_list(void) {
	// Local variables.
	uint32_t idx = 0;
	// Commands loop.
	for (idx=0 ; idx<(sizeof(AT_COMMAND_LIST) / sizeof(AT_command_t)) ; idx++) {
		// Print syntax.
		_AT_reply_add_string(AT_COMMAND_LIST[idx].syntax);
		// Print parameters.
		_AT_reply_add_string(AT_COMMAND_LIST[idx].parameters);
		_AT_reply_send();
		// Print description.
		_AT_reply_add_string(AT_REPLY_TAB);
		_AT_reply_add_string(AT_COMMAND_LIST[idx].description);
		_AT_reply_send();
	}
	_AT_print_ok();
}
#endif

#ifdef ATM
/*******************************************************************/
static void _AT_print_sw_version(void) {
	_AT_reply_add_string("SW");
	_AT_reply_add_value((int32_t) GIT_MAJOR_VERSION, STRING_FORMAT_DECIMAL, 0);
	_AT_reply_add_string(".");
	_AT_reply_add_value((int32_t) GIT_MINOR_VERSION, STRING_FORMAT_DECIMAL, 0);
	_AT_reply_add_string(".");
	_AT_reply_add_value((int32_t) GIT_COMMIT_INDEX, STRING_FORMAT_DECIMAL, 0);
	if (GIT_DIRTY_FLAG != 0) {
		_AT_reply_add_string(".d");
	}
	_AT_reply_add_string(" (");
	_AT_reply_add_value((int32_t) GIT_COMMIT_ID, STRING_FORMAT_HEXADECIMAL, 1);
	_AT_reply_add_string(")");
	_AT_reply_send();
	_AT_print_ok();
}
#endif

#ifdef ATM
/*******************************************************************/
static void _AT_print_error_stack(void) {
	// Local variables.
	SIGFOX_EP_API_status_t sigfox_ep_api_status = SIGFOX_EP_API_SUCCESS;
	SIGFOX_ERROR_t sigfox_error;
	ERROR_code_t error = SUCCESS;
	// Unstack all errors.
	_AT_reply_add_string("MCU [ ");
	do {
		error = ERROR_stack_read();
		if (error != SUCCESS) {
			_AT_reply_add_value((int32_t) error, STRING_FORMAT_HEXADECIMAL, 1);
			_AT_reply_add_string(" ");
		}
	}
	while (error != SUCCESS);
	_AT_reply_add_string("]");
	_AT_reply_send();
	// Print Sigfox library errors stack.
	_AT_reply_add_string("SIGFOX_EP_LIB [ ");
	do {
		// Read error stack.
		sigfox_ep_api_status = SIGFOX_EP_API_unstack_error(&sigfox_error);
		_AT_exit_error(sigfox_ep_api_status, SIGFOX_EP_API_SUCCESS, ERROR_BASE_SIGFOX_EP_API);
		// Check value.
		if (sigfox_error.code != SIGFOX_EP_API_SUCCESS) {
			_AT_reply_add_value((int32_t) sigfox_error.source, STRING_FORMAT_HEXADECIMAL, 1);
			_AT_reply_add_string("-");
			_AT_reply_add_value((int32_t) sigfox_error.code, STRING_FORMAT_HEXADECIMAL, 1);
			_AT_reply_add_string(" ");
		}
	}
	while (sigfox_error.code != SIGFOX_EP_API_SUCCESS);
	_AT_reply_add_string("]");
	_AT_reply_send();
	_AT_print_ok();
errors:
	return;
}
#endif

#ifdef ATM
/*******************************************************************/
static void _AT_adc_callback(void) {
	// Local variables.
	ADC_status_t adc1_status = ADC_SUCCESS;
	POWER_status_t power_status = POWER_SUCCESS;
	uint32_t voltage_mv = 0;
	int8_t tmcu_degrees = 0;
	// Trigger internal ADC conversions.
	power_status = POWER_enable(POWER_DOMAIN_ANALOG, LPTIM_DELAY_MODE_ACTIVE);
	_AT_exit_error(power_status, POWER_SUCCESS, ERROR_BASE_POWER);
	adc1_status = ADC1_perform_measurements();
	_AT_exit_error(adc1_status, ADC_SUCCESS, ERROR_BASE_ADC1);
	// Read and print data.
	// Source voltage.
	adc1_status = ADC1_get_data(ADC_DATA_INDEX_VSRC_MV, &voltage_mv);
	_AT_exit_error(adc1_status, ADC_SUCCESS, ERROR_BASE_ADC1);
	_AT_reply_add_string("Vsrc=");
	_AT_reply_add_value((int32_t) voltage_mv, STRING_FORMAT_DECIMAL, 0);
	// Supercap voltage.
	adc1_status = ADC1_get_data(ADC_DATA_INDEX_VCAP_MV, &voltage_mv);
	_AT_exit_error(adc1_status, ADC_SUCCESS, ERROR_BASE_ADC1);
	_AT_reply_add_string("mV Vcap=");
	_AT_reply_add_value((int32_t) voltage_mv, STRING_FORMAT_DECIMAL, 0);
	// MCU voltage.
	adc1_status = ADC1_get_data(ADC_DATA_INDEX_VMCU_MV, &voltage_mv);
	_AT_exit_error(adc1_status, ADC_SUCCESS, ERROR_BASE_ADC1);
	_AT_reply_add_string("mV Vmcu=");
	_AT_reply_add_value((int32_t) voltage_mv, STRING_FORMAT_DECIMAL, 0);
	// MCU temperature.
	adc1_status = ADC1_get_tmcu(&tmcu_degrees);
	_AT_exit_error(adc1_status, ADC_SUCCESS, ERROR_BASE_ADC1);
	_AT_reply_add_string("mV Tmcu=");
	_AT_reply_add_value((int32_t) tmcu_degrees, STRING_FORMAT_DECIMAL, 0);
	_AT_reply_add_string("dC");
	_AT_reply_send();
	_AT_print_ok();
errors:
	power_status = POWER_disable(POWER_DOMAIN_ANALOG);
	POWER_stack_error();
	return;
}
#endif

#ifdef ATM
/*******************************************************************/
static void _AT_ths_callback(void) {
	// Local variables.
	POWER_status_t power_status = POWER_SUCCESS;
	SHT3X_status_t sht3x_status = SHT3X_SUCCESS;
	int8_t tamb_degrees = 0;
	uint8_t hamb_percent = 0;
	// Perform measurements.
	power_status = POWER_enable(POWER_DOMAIN_SENSORS, LPTIM_DELAY_MODE_STOP);
	_AT_exit_error(power_status, POWER_SUCCESS, ERROR_BASE_POWER);
	sht3x_status = SHT3X_perform_measurements(SHT3X_I2C_ADDRESS);
	_AT_exit_error(sht3x_status, SHT3X_SUCCESS, ERROR_BASE_SHT3X);
	// Read data.
	sht3x_status = SHT3X_get_temperature(&tamb_degrees);
	_AT_exit_error(sht3x_status, SHT3X_SUCCESS, ERROR_BASE_SHT3X);
	sht3x_status = SHT3X_get_humidity(&hamb_percent);
	_AT_exit_error(sht3x_status, SHT3X_SUCCESS, ERROR_BASE_SHT3X);
	// Print results.
	_AT_reply_add_string("T=");
	_AT_reply_add_value((int32_t) tamb_degrees, STRING_FORMAT_DECIMAL, 0);
	_AT_reply_add_string("dC H=");
	_AT_reply_add_value((int32_t) hamb_percent, STRING_FORMAT_DECIMAL, 0);
	_AT_reply_add_string("%");
	_AT_reply_send();
	_AT_print_ok();
errors:
	power_status = POWER_disable(POWER_DOMAIN_SENSORS);
	POWER_stack_error();
	return;
}
#endif

#ifdef ATM
/*******************************************************************/
static void _AT_acc_callback(void) {
	// Local variables.
	POWER_status_t power_status = POWER_SUCCESS;
	MMA8653FC_status_t mma8653fc_status = MMA8653FC_SUCCESS;
	uint8_t chip_id = 0;
	// Get ID.
	power_status = POWER_enable(POWER_DOMAIN_SENSORS, LPTIM_DELAY_MODE_STOP);
	_AT_exit_error(power_status, POWER_SUCCESS, ERROR_BASE_POWER);
	mma8653fc_status = MMA8653FC_get_id(&chip_id);
	_AT_exit_error(mma8653fc_status, MMA8653FC_SUCCESS, ERROR_BASE_MMA8653FC);
	// Print data.
	_AT_reply_add_string("MMA8653FC chip ID: ");
	_AT_reply_add_value(chip_id, STRING_FORMAT_HEXADECIMAL, 1);
	_AT_reply_send();
	_AT_print_ok();
errors:
	power_status = POWER_disable(POWER_DOMAIN_SENSORS);
	POWER_stack_error();
	return;
}
#endif

#ifdef ATM
/*******************************************************************/
static void _AT_gps_callback(void) {
	// Local variables.
	POWER_status_t power_status = POWER_SUCCESS;
	PARSER_status_t parser_status = PARSER_ERROR_UNKNOWN_COMMAND;
	NEOM8N_status_t neom8n_status = NEOM8N_SUCCESS;
	int32_t timeout_seconds = 0;
	uint32_t fix_duration_seconds = 0;
	NEOM8N_position_t gps_position;
	// Read timeout parameter.
	parser_status = PARSER_get_parameter(&at_ctx.parser, STRING_FORMAT_DECIMAL, STRING_CHAR_NULL, &timeout_seconds);
	_AT_exit_error(parser_status, PARSER_SUCCESS, ERROR_BASE_PARSER);
	// Power on GPS.
	power_status = POWER_enable(POWER_DOMAIN_GPS, LPTIM_DELAY_MODE_STOP);
	_AT_exit_error(power_status, POWER_SUCCESS, ERROR_BASE_POWER);
	// Start GPS fix.
	neom8n_status = NEOM8N_get_position(&gps_position, (uint32_t) timeout_seconds, 0, &fix_duration_seconds);
	_AT_exit_error(neom8n_status, NEOM8N_SUCCESS, ERROR_BASE_NEOM8N);
	// Latitude.
	_AT_reply_add_string("Lat=");
	_AT_reply_add_value((gps_position.lat_degrees), STRING_FORMAT_DECIMAL, 0);
	_AT_reply_add_string("d");
	_AT_reply_add_value((gps_position.lat_minutes), STRING_FORMAT_DECIMAL, 0);
	_AT_reply_add_string("'");
	_AT_reply_add_value((gps_position.lat_seconds), STRING_FORMAT_DECIMAL, 0);
	_AT_reply_add_string("''");
	_AT_reply_add_string(((gps_position.lat_north_flag) == 0) ? "S" : "N");
	// Longitude.
	_AT_reply_add_string(" Long=");
	_AT_reply_add_value((gps_position.long_degrees), STRING_FORMAT_DECIMAL, 0);
	_AT_reply_add_string("d");
	_AT_reply_add_value((gps_position.long_minutes), STRING_FORMAT_DECIMAL, 0);
	_AT_reply_add_string("'");
	_AT_reply_add_value((gps_position.long_seconds), STRING_FORMAT_DECIMAL, 0);
	_AT_reply_add_string("''");
	_AT_reply_add_string(((gps_position.long_east_flag) == 0) ? "W" : "E");
	// Altitude.
	_AT_reply_add_string(" Alt=");
	_AT_reply_add_value((gps_position.altitude), STRING_FORMAT_DECIMAL, 0);
	// Fix duration.
	_AT_reply_add_string("m Fix=");
	_AT_reply_add_value(fix_duration_seconds, STRING_FORMAT_DECIMAL, 0);
	_AT_reply_add_string("s");
	_AT_reply_send();
	_AT_print_ok();
errors:
	power_status = POWER_disable(POWER_DOMAIN_GPS);
	POWER_stack_error();
	return;
}
#endif

#ifdef ATM
/*******************************************************************/
static void _AT_nvm_callback(void) {
	// Local variables.
	PARSER_status_t parser_status = PARSER_ERROR_UNKNOWN_COMMAND;
	NVM_status_t nvm_status = NVM_SUCCESS;
	int32_t address = 0;
	uint8_t nvm_data = 0;
	// Read address parameters.
	parser_status = PARSER_get_parameter(&at_ctx.parser, STRING_FORMAT_DECIMAL, STRING_CHAR_NULL, &address);
	_AT_exit_error(parser_status, PARSER_SUCCESS, ERROR_BASE_PARSER);
	// Read byte at requested address.
	nvm_status = NVM_read_byte((uint16_t) address, &nvm_data);
	_AT_exit_error(nvm_status, NVM_SUCCESS, ERROR_BASE_NVM);
	// Print data.
	_AT_reply_add_value(nvm_data, STRING_FORMAT_HEXADECIMAL, 1);
	_AT_reply_send();
	_AT_print_ok();
errors:
	return;
}
#endif

#ifdef ATM
/*******************************************************************/
static void _AT_get_id_callback(void) {
	// Local variables.
	NVM_status_t nvm_status = NVM_SUCCESS;
	uint8_t idx = 0;
	uint8_t id_byte = 0;
	// Retrieve device ID in NVM.
	for (idx=0 ; idx<SIGFOX_EP_ID_SIZE_BYTES ; idx++) {
		nvm_status = NVM_read_byte((NVM_ADDRESS_SIGFOX_EP_ID + idx), &id_byte);
		_AT_exit_error(nvm_status, NVM_SUCCESS, ERROR_BASE_NVM);
		_AT_reply_add_value(id_byte, STRING_FORMAT_HEXADECIMAL, ((idx == 0) ? 1 : 0));
	}
	_AT_reply_send();
	_AT_print_ok();
errors:
	return;
}
#endif

#ifdef ATM
/*******************************************************************/
static void _AT_set_id_callback(void) {
	// Local variables.
	PARSER_status_t parser_status = PARSER_ERROR_UNKNOWN_COMMAND;
	NVM_status_t nvm_status = NVM_SUCCESS;
	uint8_t sigfox_ep_id[SIGFOX_EP_ID_SIZE_BYTES];
	uint8_t extracted_length = 0;
	uint8_t idx = 0;
	// Read ID parameter.
	parser_status = PARSER_get_byte_array(&at_ctx.parser, STRING_CHAR_NULL, SIGFOX_EP_ID_SIZE_BYTES, 1, sigfox_ep_id, &extracted_length);
	_AT_exit_error(parser_status, PARSER_SUCCESS, ERROR_BASE_PARSER);
	// Write device ID in NVM.
	for (idx=0 ; idx<SIGFOX_EP_ID_SIZE_BYTES ; idx++) {
		nvm_status = NVM_write_byte((NVM_ADDRESS_SIGFOX_EP_ID + idx), sigfox_ep_id[idx]);
		_AT_exit_error(nvm_status, NVM_SUCCESS, ERROR_BASE_NVM);
	}
	_AT_print_ok();
errors:
	return;
}
#endif

#ifdef ATM
/*******************************************************************/
static void _AT_get_key_callback(void) {
	// Local variables.
	NVM_status_t nvm_status = NVM_SUCCESS;
	uint8_t idx = 0;
	uint8_t key_byte = 0;
	// Retrieve device key in NVM.
	for (idx=0 ; idx<SIGFOX_EP_KEY_SIZE_BYTES ; idx++) {
		nvm_status = NVM_read_byte((NVM_ADDRESS_SIGFOX_EP_KEY + idx), &key_byte);
		_AT_exit_error(nvm_status, NVM_SUCCESS, ERROR_BASE_NVM);
		_AT_reply_add_value(key_byte, STRING_FORMAT_HEXADECIMAL, ((idx == 0) ? 1 : 0));
	}
	_AT_reply_send();
	_AT_print_ok();
errors:
	return;
}
#endif

#ifdef ATM
/*******************************************************************/
static void _AT_set_key_callback(void) {
	// Local variables.
	PARSER_status_t parser_status = PARSER_ERROR_UNKNOWN_COMMAND;
	NVM_status_t nvm_status = NVM_SUCCESS;
	uint8_t sigfox_ep_key[SIGFOX_EP_KEY_SIZE_BYTES];
	uint8_t extracted_length = 0;
	uint8_t idx = 0;
	// Read key parameter.
	parser_status = PARSER_get_byte_array(&at_ctx.parser, STRING_CHAR_NULL, SIGFOX_EP_KEY_SIZE_BYTES, 1, sigfox_ep_key, &extracted_length);
	_AT_exit_error(parser_status, PARSER_SUCCESS, ERROR_BASE_PARSER);
	// Write device ID in NVM.
	for (idx=0 ; idx<SIGFOX_EP_KEY_SIZE_BYTES ; idx++) {
		nvm_status = NVM_write_byte((NVM_ADDRESS_SIGFOX_EP_KEY + idx), sigfox_ep_key[idx]);
		_AT_exit_error(nvm_status, NVM_SUCCESS, ERROR_BASE_NVM);
	}
	_AT_print_ok();
errors:
	return;
}
#endif

#ifdef ATM
/*******************************************************************/
static void _AT_print_dl_payload(void) {
	// Local variables.
	SIGFOX_EP_API_status_t sigfox_ep_api_status = SIGFOX_EP_API_SUCCESS;
	sfx_u8 dl_payload[SIGFOX_DL_PAYLOAD_SIZE_BYTES];
	sfx_s16 dl_rssi_dbm = 0;
	// Read downlink payload.
	sigfox_ep_api_status = SIGFOX_EP_API_get_dl_payload(dl_payload, SIGFOX_DL_PAYLOAD_SIZE_BYTES, &dl_rssi_dbm);
	_AT_exit_error(sigfox_ep_api_status, SIGFOX_EP_API_SUCCESS, ERROR_BASE_SIGFOX_EP_API);
	// Print downlink payload.
	AT_print_dl_payload(dl_payload, SIGFOX_DL_PAYLOAD_SIZE_BYTES, dl_rssi_dbm);
errors:
	return;
}
#endif

#ifdef ATM
/*******************************************************************/
static void _AT_sb_callback(void) {
	// Local variables.
	PARSER_status_t parser_status = PARSER_ERROR_UNKNOWN_COMMAND;
	SIGFOX_EP_API_status_t sigfox_ep_api_status = SIGFOX_EP_API_SUCCESS;
	SIGFOX_EP_API_config_t lib_config;
	SIGFOX_EP_API_application_message_t application_message;
	int32_t ul_bit = 0;
	int32_t bidir_flag = 0;
	// Library configuration.
	lib_config.rc = &SIGFOX_RC1;
	// Default application message parameters.
	application_message.common_parameters.number_of_frames = 3;
	application_message.common_parameters.ul_bit_rate = SIGFOX_UL_BIT_RATE_100BPS;
	application_message.ul_payload = SFX_NULL;
	application_message.ul_payload_size_bytes = 0;
	// First try with 2 parameters.
	parser_status = PARSER_get_parameter(&at_ctx.parser, STRING_FORMAT_BOOLEAN, AT_CHAR_SEPARATOR, &ul_bit);
	if (parser_status == PARSER_SUCCESS) {
		// Try parsing downlink request parameter.
		parser_status =  PARSER_get_parameter(&at_ctx.parser, STRING_FORMAT_BOOLEAN, STRING_CHAR_NULL, &bidir_flag);
		_AT_exit_error(parser_status, PARSER_SUCCESS, ERROR_BASE_PARSER);
		// Update parameters.
		application_message.type = (SIGFOX_APPLICATION_MESSAGE_TYPE_BIT0 + ul_bit);
		application_message.bidirectional_flag = bidir_flag;
	}
	else {
		// Try with 1 parameter.
		parser_status = PARSER_get_parameter(&at_ctx.parser, STRING_FORMAT_BOOLEAN, STRING_CHAR_NULL, &ul_bit);
		_AT_exit_error(parser_status, PARSER_SUCCESS, ERROR_BASE_PARSER);
		// Update parameters.
		application_message.type = (SIGFOX_APPLICATION_MESSAGE_TYPE_BIT0 + ul_bit);
		application_message.bidirectional_flag = 0;
	}
	// Open library.
	sigfox_ep_api_status = SIGFOX_EP_API_open(&lib_config);
	_AT_exit_error(sigfox_ep_api_status, SIGFOX_EP_API_SUCCESS, ERROR_BASE_SIGFOX_EP_API);
	// Send application message.
	sigfox_ep_api_status = SIGFOX_EP_API_send_application_message(&application_message);
	_AT_exit_error(sigfox_ep_api_status, SIGFOX_EP_API_SUCCESS, ERROR_BASE_SIGFOX_EP_API);
	// Read and print DL payload if needed.
	if ((application_message.bidirectional_flag) == SFX_TRUE) {
		_AT_print_dl_payload();
	}
	// Print OK.
	_AT_print_ok();
errors:
	// Close library.
	SIGFOX_EP_API_close();
	return;
}
#endif

#ifdef ATM
/*******************************************************************/
static void _AT_sf_callback(void) {
	// Local variables.
	PARSER_status_t parser_status = PARSER_ERROR_UNKNOWN_COMMAND;
	SIGFOX_EP_API_status_t sigfox_ep_api_status = SIGFOX_EP_API_SUCCESS;
	SIGFOX_EP_API_config_t lib_config;
	SIGFOX_EP_API_application_message_t application_message;
	sfx_u8 data[SIGFOX_UL_PAYLOAD_MAX_SIZE_BYTES];
	uint8_t extracted_length = 0;
	int32_t bidir_flag = 0;
	// Library configuration.
	lib_config.rc = &SIGFOX_RC1;
	// Default application message parameters.
	application_message.common_parameters.number_of_frames = 3;
	application_message.common_parameters.ul_bit_rate = SIGFOX_UL_BIT_RATE_100BPS;
	application_message.type = SIGFOX_APPLICATION_MESSAGE_TYPE_BYTE_ARRAY;
	application_message.bidirectional_flag = 0;
	application_message.ul_payload = SFX_NULL;
	application_message.ul_payload_size_bytes = 0;
	// First try with 2 parameters.
	parser_status = PARSER_get_byte_array(&at_ctx.parser, AT_CHAR_SEPARATOR, SIGFOX_UL_PAYLOAD_MAX_SIZE_BYTES, 0, data, &extracted_length);
	if (parser_status == PARSER_SUCCESS) {
		// Try parsing downlink request parameter.
		parser_status =  PARSER_get_parameter(&at_ctx.parser, STRING_FORMAT_BOOLEAN, STRING_CHAR_NULL, &bidir_flag);
		_AT_exit_error(parser_status, PARSER_SUCCESS, ERROR_BASE_PARSER);
		// Update parameters.
		application_message.ul_payload = (sfx_u8*) data;
		application_message.ul_payload_size_bytes = extracted_length;
		application_message.bidirectional_flag = bidir_flag;
	}
	else {
		// Try with 1 parameter.
		parser_status = PARSER_get_byte_array(&at_ctx.parser, STRING_CHAR_NULL, SIGFOX_UL_PAYLOAD_MAX_SIZE_BYTES, 0, data, &extracted_length);
		_AT_exit_error(parser_status, PARSER_SUCCESS, ERROR_BASE_PARSER);
		// Update parameters.
		application_message.ul_payload = (sfx_u8*) data;
		application_message.ul_payload_size_bytes = extracted_length;
	}
	// Open library.
	sigfox_ep_api_status = SIGFOX_EP_API_open(&lib_config);
	_AT_exit_error(sigfox_ep_api_status, SIGFOX_EP_API_SUCCESS, ERROR_BASE_SIGFOX_EP_API);
	// Send application message.
	sigfox_ep_api_status = SIGFOX_EP_API_send_application_message(&application_message);
	_AT_exit_error(sigfox_ep_api_status, SIGFOX_EP_API_SUCCESS, ERROR_BASE_SIGFOX_EP_API);
	// Read and print DL payload if needed.
	if ((application_message.bidirectional_flag) == SFX_TRUE) {
		_AT_print_dl_payload();
	}
	// Print OK.
	_AT_print_ok();
errors:
	// Close library.
	SIGFOX_EP_API_close();
	return;
}
#endif

#ifdef ATM
/*******************************************************************/
static void _AT_tm_callback(void) {
	// Local variables.
	PARSER_status_t parser_status = PARSER_ERROR_UNKNOWN_COMMAND;
	SIGFOX_EP_ADDON_RFP_API_status_t sigfox_ep_addon_rfp_status = SIGFOX_EP_ADDON_RFP_API_SUCCESS;
	SIGFOX_EP_ADDON_RFP_API_config_t addon_config;
	SIGFOX_EP_ADDON_RFP_API_test_mode_t test_mode;
	int32_t rc_index = 0;
	int32_t test_mode_reference = 0;
	// Read RC parameter.SIGFOX_EP_ADDON_RFP_API_close();
	parser_status = PARSER_get_parameter(&at_ctx.parser, STRING_FORMAT_DECIMAL, AT_CHAR_SEPARATOR, &rc_index);
	_AT_exit_error(parser_status, PARSER_SUCCESS, ERROR_BASE_PARSER);
	// Read test mode parameter.
	parser_status =  PARSER_get_parameter(&at_ctx.parser, STRING_FORMAT_DECIMAL, STRING_CHAR_NULL, &test_mode_reference);
	_AT_exit_error(parser_status, PARSER_SUCCESS, ERROR_BASE_PARSER);
	// Addon configuration.
	addon_config.rc = &SIGFOX_RC1;
	// Test mode parameters.
	test_mode.test_mode_reference = (SIGFOX_EP_ADDON_RFP_API_test_mode_reference_t) test_mode_reference;
	test_mode.ul_bit_rate = SIGFOX_UL_BIT_RATE_100BPS;
	// Open addon.
	sigfox_ep_addon_rfp_status = SIGFOX_EP_ADDON_RFP_API_open(&addon_config);
	_AT_exit_error(sigfox_ep_addon_rfp_status, SIGFOX_EP_ADDON_RFP_API_SUCCESS, ERROR_BASE_SIGFOX_EP_ADDON_RFP);
	// Call test mode function.
	sigfox_ep_addon_rfp_status = SIGFOX_EP_ADDON_RFP_API_test_mode(&test_mode);
	_AT_exit_error(sigfox_ep_addon_rfp_status, SIGFOX_EP_ADDON_RFP_API_SUCCESS, ERROR_BASE_SIGFOX_EP_ADDON_RFP);
	_AT_print_ok();
errors:
	// Close addon.
	SIGFOX_EP_ADDON_RFP_API_close();
	return;
}
#endif

#ifdef ATM
/*******************************************************************/
static void _AT_cw_callback(void) {
	// Local variables.
	PARSER_status_t parser_status = PARSER_ERROR_UNKNOWN_COMMAND;
	RF_API_status_t rf_api_status = RF_API_SUCCESS;
	RF_API_radio_parameters_t radio_params;
	S2LP_status_t s2lp_status = S2LP_SUCCESS;
	int32_t enable = 0;
	int32_t frequency_hz = 0;
	int32_t power_dbm = 0;
	// Set common radio parameters.
	radio_params.rf_mode = RF_API_MODE_TX;
	radio_params.modulation = RF_API_MODULATION_NONE;
	radio_params.bit_rate_bps = 0;
#ifdef BIDIRECTIONAL
	radio_params.deviation_hz = 0;
#endif
	// Read frequency parameter.
	parser_status = PARSER_get_parameter(&at_ctx.parser, STRING_FORMAT_DECIMAL, AT_CHAR_SEPARATOR, &frequency_hz);
	_AT_exit_error(parser_status, PARSER_SUCCESS, ERROR_BASE_PARSER);
	// Update radio configuration.
	radio_params.frequency_hz = (sfx_u32) frequency_hz;
	// First try with 3 parameters.
	parser_status = PARSER_get_parameter(&at_ctx.parser, STRING_FORMAT_BOOLEAN, AT_CHAR_SEPARATOR, &enable);
	if (parser_status == PARSER_SUCCESS) {
		// There is a third parameter, try to parse power.
		parser_status = PARSER_get_parameter(&at_ctx.parser, STRING_FORMAT_DECIMAL, STRING_CHAR_NULL, &power_dbm);
		_AT_exit_error(parser_status, PARSER_SUCCESS, ERROR_BASE_PARSER);
		// Update radio configuration.
		radio_params.tx_power_dbm_eirp = (sfx_s8) power_dbm;
	}
	else {
		// Power is not given, try to parse enable as last parameter.
		parser_status = PARSER_get_parameter(&at_ctx.parser, STRING_FORMAT_BOOLEAN, STRING_CHAR_NULL, &enable);
		_AT_exit_error(parser_status, PARSER_SUCCESS, ERROR_BASE_PARSER);
		// Update radio configuration.
		radio_params.tx_power_dbm_eirp = TX_POWER_DBM_EIRP;
	}
	// Stop CW.
	rf_api_status = RF_API_de_init();
	_AT_exit_error(rf_api_status, RF_API_SUCCESS, ERROR_BASE_SIGFOX_RF_API);
	rf_api_status = RF_API_sleep();
	_AT_exit_error(rf_api_status, RF_API_SUCCESS, ERROR_BASE_SIGFOX_RF_API);
	// Restart if required.
	if (enable != 0) {
		// Init radio.
		rf_api_status = RF_API_wake_up();
		_AT_exit_error(rf_api_status, RF_API_SUCCESS, ERROR_BASE_SIGFOX_RF_API);
		rf_api_status = RF_API_init(&radio_params);
		_AT_exit_error(rf_api_status, RF_API_SUCCESS, ERROR_BASE_SIGFOX_RF_API);
		// Start CW.
		s2lp_status = S2LP_send_command(S2LP_COMMAND_READY);
		if (s2lp_status != S2LP_SUCCESS) goto errors;
		s2lp_status = S2LP_wait_for_state(S2LP_STATE_READY);
		if (s2lp_status != S2LP_SUCCESS) goto errors;
		s2lp_status = S2LP_send_command(S2LP_COMMAND_TX);
		if (s2lp_status != S2LP_SUCCESS) goto errors;
		_AT_reply_add_string("CW running...");
		_AT_reply_send();
	}
	_AT_print_ok();
	return;
errors:
	// Force radio off.
	RF_API_de_init();
	RF_API_sleep();
	return;
}
#endif

#ifdef ATM
/*******************************************************************/
static void _AT_rssi_callback(void) {
	// Local variables.
	PARSER_status_t parser_status = PARSER_ERROR_UNKNOWN_COMMAND;
	RF_API_status_t rf_api_status = RF_API_SUCCESS;
	RF_API_radio_parameters_t radio_params;
	S2LP_status_t s2lp_status = S2LP_SUCCESS;
	LPTIM_status_t lptim1_status = LPTIM_SUCCESS;
	int32_t frequency_hz = 0;
	int32_t duration_seconds = 0;
	int16_t rssi_dbm = 0;
	uint32_t report_loop = 0;
	// Read frequency parameter.
	parser_status = PARSER_get_parameter(&at_ctx.parser, STRING_FORMAT_DECIMAL, AT_CHAR_SEPARATOR, &frequency_hz);
	_AT_exit_error(parser_status, PARSER_SUCCESS, ERROR_BASE_PARSER);
	// Read duration parameters.
	parser_status = PARSER_get_parameter(&at_ctx.parser, STRING_FORMAT_DECIMAL, STRING_CHAR_NULL, &duration_seconds);
	_AT_exit_error(parser_status, PARSER_SUCCESS, ERROR_BASE_PARSER);
	// Radio configuration.
	radio_params.rf_mode = RF_API_MODE_RX;
	radio_params.frequency_hz = (sfx_u32) frequency_hz;
	radio_params.modulation = RF_API_MODULATION_NONE;
	radio_params.bit_rate_bps = 0;
	radio_params.tx_power_dbm_eirp = TX_POWER_DBM_EIRP;
	radio_params.deviation_hz = 0;
	// Init radio.
	rf_api_status = RF_API_wake_up();
	_AT_exit_error(rf_api_status, RF_API_SUCCESS, ERROR_BASE_SIGFOX_RF_API);
	rf_api_status = RF_API_init(&radio_params);
	_AT_exit_error(rf_api_status, RF_API_SUCCESS, ERROR_BASE_SIGFOX_RF_API);
	// Start continuous listening.
	s2lp_status = S2LP_send_command(S2LP_COMMAND_READY);
	_AT_exit_error(s2lp_status, S2LP_SUCCESS, ERROR_BASE_S2LP);
	s2lp_status = S2LP_wait_for_state(S2LP_STATE_READY);
	_AT_exit_error(s2lp_status, S2LP_SUCCESS, ERROR_BASE_S2LP);
	s2lp_status = S2LP_send_command(S2LP_COMMAND_RX);
	_AT_exit_error(s2lp_status, S2LP_SUCCESS, ERROR_BASE_S2LP);
	// Measurement loop.
	while (report_loop < ((duration_seconds * 1000) / AT_RSSI_REPORT_PERIOD_MS)) {
		// Read RSSI.
		s2lp_status = S2LP_get_rssi(S2LP_RSSI_TYPE_RUN, &rssi_dbm);
		_AT_exit_error(s2lp_status, S2LP_SUCCESS, ERROR_BASE_S2LP);
		// Print RSSI.
		_AT_reply_add_string("RSSI=");
		_AT_reply_add_value(rssi_dbm, STRING_FORMAT_DECIMAL, 0);
		_AT_reply_add_string("dBm");
		_AT_reply_send();
		// Report delay.
		lptim1_status = LPTIM1_delay_milliseconds(AT_RSSI_REPORT_PERIOD_MS, LPTIM_DELAY_MODE_ACTIVE);
		_AT_exit_error(lptim1_status, LPTIM_SUCCESS, ERROR_BASE_LPTIM1);
		report_loop++;
	}
	_AT_print_ok();
errors:
	// Force radio off.
	RF_API_de_init();
	RF_API_sleep();
	return;
}
#endif

#ifdef ATM
/*******************************************************************/
static void _AT_reset_parser(void) {
	// Flush buffers.
	at_ctx.command_size = 0;
	at_ctx.reply_size = 0;
	// Reset flag.
	at_ctx.line_end_flag = 0;
	// Reset parser.
	at_ctx.parser.buffer = (char_t*) at_ctx.command;
	at_ctx.parser.buffer_size = 0;
	at_ctx.parser.separator_idx = 0;
	at_ctx.parser.start_idx = 0;
}
#endif

#ifdef ATM
/*******************************************************************/
static void _AT_decode(void) {
	// Local variables.
	uint8_t idx = 0;
	uint8_t decode_success = 0;
	// Update parser length.
	at_ctx.parser.buffer_size = at_ctx.command_size;
	// Loop on available commands.
	for (idx=0 ; idx<(sizeof(AT_COMMAND_LIST) / sizeof(AT_command_t)) ; idx++) {
		// Check type.
		if (PARSER_compare(&at_ctx.parser, AT_COMMAND_LIST[idx].mode, AT_COMMAND_LIST[idx].syntax) == PARSER_SUCCESS) {
			// Execute callback and exit.
			AT_COMMAND_LIST[idx].callback();
			decode_success = 1;
			break;
		}
	}
	if (decode_success == 0) {
		_AT_print_error(ERROR_BASE_PARSER + PARSER_ERROR_UNKNOWN_COMMAND); // Unknown command.
		goto errors;
	}
errors:
	_AT_reset_parser();
	return;
}
#endif

/*** AT functions ***/

#ifdef ATM
/*******************************************************************/
void AT_init(void) {
	// Init context.
	_AT_reset_parser();
	// Init USART.
	USART2_init(&_AT_fill_rx_buffer);
	USART2_enable_rx();
}
#endif

#ifdef ATM
/*******************************************************************/
void AT_task(void) {
	// Trigger decoding function if line end found.
	if (at_ctx.line_end_flag != 0) {
		// Decode and execute command.
		USART2_disable_rx();
		_AT_decode();
		USART2_enable_rx();
	}
}
#endif

#ifdef ATM
/*******************************************************************/
void AT_print_dl_payload(sfx_u8 *dl_payload, sfx_u8 dl_payload_size, sfx_s16 rssi_dbm) {
	// Local variables.
	uint8_t idx = 0;
	// Print DL payload.
	_AT_reply_add_string("+RX=");
	for (idx=0 ; idx<dl_payload_size ; idx++) {
		_AT_reply_add_value(dl_payload[idx], STRING_FORMAT_HEXADECIMAL, 0);
	}
	_AT_reply_add_string(" (RSSI=");
	_AT_reply_add_value(rssi_dbm, STRING_FORMAT_DECIMAL, 0);
	_AT_reply_add_string("dBm)");
	_AT_reply_send();
}
#endif
