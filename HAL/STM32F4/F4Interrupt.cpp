#include "F4Interrupt.h"

#include <protocol/common.h>

using namespace HAL;
static STM32F4::F4Interrupt *int_table[16] = {0};

namespace STM32F4
{	
	int Pin2PinSource(uint32_t GPIO_Pin)
	{
		for(int i=0; i<16; i++)
			if (GPIO_Pin == 1 << i)
				return i;
		return -1;
	}
	
	EXTITrigger_TypeDef flag2trigger(int flag)
	{
		if (flag == interrupt_rising)
			return EXTI_Trigger_Rising;
		if (flag == interrupt_falling)
			return EXTI_Trigger_Falling;
		if (flag == interrupt_rising_or_falling)
			return EXTI_Trigger_Rising_Falling;
		
		return EXTI_Trigger_Rising_Falling;
	}
	
	int port2port_source(GPIO_TypeDef* GPIOx)
	{
		// only A - K supported yet.
		if (GPIOx < GPIOA || GPIOx > GPIOK)
			return -1;
		
		return (GPIOx - GPIOA);
	}
	
	int pin2irqn(uint32_t GPIO_Pin)
	{
		if (GPIO_Pin == GPIO_Pin_0)
			return EXTI0_IRQn;
		if (GPIO_Pin == GPIO_Pin_1)
			return EXTI1_IRQn;
		if (GPIO_Pin == GPIO_Pin_2)
			return EXTI2_IRQn;
		if (GPIO_Pin == GPIO_Pin_3)
			return EXTI3_IRQn;
		if (GPIO_Pin == GPIO_Pin_4)
			return EXTI4_IRQn;
		if (GPIO_Pin >= GPIO_Pin_5 && GPIO_Pin <=9)
			return EXTI9_5_IRQn;
		if (GPIO_Pin >= GPIO_Pin_10 && GPIO_Pin <=15)
			return EXTI15_10_IRQn;
		
		return -1;
	}
	
	F4Interrupt::F4Interrupt()
	{
	}
	
	bool F4Interrupt::init(GPIO_TypeDef* GPIOx, uint32_t GPIO_Pin, int flag)
	{
		int port_source = port2port_source(GPIOx);
		int pin_source =  Pin2PinSource(GPIO_Pin);
		if (pin_source < 0 || port_source < 0)
		{
			LOGE("invalid interrupt\n");
			return false;
		}
		
		if (!int_table[pin_source])
		{
			LOGE("warning: duplicate exti %d\n", pin_source);
		}
		
		// open everything....
		RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOA|RCC_AHB1Periph_GPIOB|RCC_AHB1Periph_GPIOC|RCC_AHB1Periph_GPIOD | RCC_AHB1Periph_GPIOE,ENABLE);
		RCC_APB2PeriphClockCmd(RCC_APB2Periph_SYSCFG,ENABLE);

		// configure GPIO.
		GPIO_InitTypeDef GPIO_InitStructure = {0};
		GPIO_InitStructure.GPIO_Pin = GPIO_Pin;
		GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IN;
		GPIO_InitStructure.GPIO_PuPd = GPIO_PuPd_UP;
		GPIO_Init(GPIOx, &GPIO_InitStructure);

		// configure exti
		EXTI_InitTypeDef   EXTI_InitStructure;
		EXTI_InitStructure.EXTI_Mode = EXTI_Mode_Interrupt;
		EXTI_InitStructure.EXTI_Trigger = flag2trigger(flag);
		EXTI_InitStructure.EXTI_LineCmd = ENABLE;
		EXTI_ClearITPendingBit(GPIO_Pin);		
		SYSCFG_EXTILineConfig(port_source, pin_source);
		EXTI_InitStructure.EXTI_Line = GPIO_Pin;
		EXTI_Init(&EXTI_InitStructure);

		NVIC_InitTypeDef NVIC_InitStructure;
		NVIC_InitStructure.NVIC_IRQChannel = pin2irqn(GPIO_Pin);
		NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 1;
		NVIC_InitStructure.NVIC_IRQChannelSubPriority = 0;
		NVIC_InitStructure.NVIC_IRQChannelCmd = ENABLE;
		NVIC_Init(&NVIC_InitStructure);
		
		int_table[pin_source] = this;
		
		return true;
	}
	
	void F4Interrupt::set_callback(HAL::interrupt_callback cb, void *parameter)
	{		
		this->cb=cb;
		this->parameter = parameter;
	}
	
	void F4Interrupt::call_callback()
	{
		if(cb)
			cb(parameter, flag);
	}
}