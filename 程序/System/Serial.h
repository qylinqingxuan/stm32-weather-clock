#ifndef __SERIAL_H__
#define __SERIAL_H__
#include <stdio.h>
#include <stdarg.h>

#define USART1_RX_BUF_SIZE   1024
#define USART2_RX_BUF_SIZE   2560

void Serial_Init (uint8_t USART_Name);
void Serial_SendByte (uint8_t USART_Name,uint8_t Byte);
void Serial_SendArray (uint8_t USART_Name,uint8_t *Array,uint16_t Length);
void Serial_SendString (uint8_t USART_Name,char *String);
void Serial_SendNumber (uint8_t USART_Name,uint32_t Number,uint16_t Length);
void Serial_Printf (uint8_t USART_Name,char *format,...);
extern char USART1_RX_BUF[USART1_RX_BUF_SIZE];
extern uint16_t USART1_RX_LEN;
extern char USART2_RX_BUF[USART2_RX_BUF_SIZE];
extern uint16_t USART2_RX_LEN;
#endif
