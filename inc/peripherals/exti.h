/*
 * exti.h
 *
 *  Created on: 18 apr. 2020
 *      Author: Ludo
 */

#ifndef EXTI_H
#define EXTI_H

#include "gpio.h"

/*** EXTI structures ***/

typedef enum {
	EXTI_TRIGGER_RISING_EDGE,
	EXTI_TRIGGER_FALLING_EDGE,
	EXTI_TRIGGER_ANY_EDGE
} EXTI_Trigger;

/*** EXTI functions ***/

void EXTI_Init(void);
void EXTI_ConfigureInterrupt(const GPIO* gpio, EXTI_Trigger edge_trigger);
void EXTI_ClearAllFlags(void);

#endif /* EXTI_H_ */
