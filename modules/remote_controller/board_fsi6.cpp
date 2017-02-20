#include "board.h"

#include <HAL/STM32F1/F1SPI.h>
#include <HAL/STM32F1/F1GPIO.h>
#include <HAL/STM32F1/F1Interrupt.h>
#include <misc.h>
#include <HAL/STM32F1/F1Timer.h>
#include <stm32f10x.h>
#include <string.h>

using namespace STM32F1;
using namespace HAL;

HAL::ISPI *spi;
HAL::IGPIO *cs;
HAL::IGPIO *ce;
HAL::IGPIO *irq;
HAL::IGPIO *dbg;
HAL::IGPIO *dbg2;
HAL::IGPIO *SCL;
HAL::IGPIO *SDA;
HAL::IInterrupt *interrupt;
HAL::ITimer *timer;

int16_t adc_data[6] = {0};
namespace sheet1
{
	F1GPIO cs(GPIOC, GPIO_Pin_6);
	F1GPIO ce(GPIOC, GPIO_Pin_7);
	F1GPIO irq(GPIOB, GPIO_Pin_12);
	F1GPIO dbg(GPIOC, GPIO_Pin_9);
	
	F1GPIO dbg2(GPIOC, GPIO_Pin_4);
	F1GPIO SCL(GPIOC, GPIO_Pin_13);
	F1GPIO SDA(GPIOC, GPIO_Pin_14);
	
	F1SPI spi;
	F1Interrupt interrupt;
	F1Timer timer(TIM2);
	
	F1GPIO pa6(GPIOA, GPIO_Pin_6);

	
	
	static void ADC1_Mode_Config(void)
	{
		ADC_InitTypeDef ADC_InitStructure;
		GPIO_InitTypeDef GPIO_InitStructure;
		RCC_APB2PeriphClockCmd(RCC_APB2Periph_ADC1, ENABLE);
		RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA, ENABLE);
		RCC_AHBPeriphClockCmd(RCC_AHBPeriph_DMA1, ENABLE);
		
		// Configure GPIO0~2 as analog input
		for(int ADC_Channel=0; ADC_Channel<7; ADC_Channel++)
		{
			GPIO_InitStructure.GPIO_Pin = (1 << (ADC_Channel%8));
			GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AIN;
			GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
			GPIO_Init(ADC_Channel>8?GPIOB:GPIOA, &GPIO_InitStructure);
		}

		
		DMA_InitTypeDef DMA_InitStructure;
		
		/* DMA channel1 configuration */
		DMA_DeInit(DMA1_Channel1);
		DMA_InitStructure.DMA_PeripheralBaseAddr = (uint32_t)&(ADC1->DR);	 //ADC��ַ
		DMA_InitStructure.DMA_MemoryBaseAddr = (uint32_t)&adc_data;//�ڴ��ַ
		DMA_InitStructure.DMA_DIR = DMA_DIR_PeripheralSRC;
		DMA_InitStructure.DMA_BufferSize = 6;
		DMA_InitStructure.DMA_PeripheralInc = DMA_PeripheralInc_Disable;//�����ַ�̶�
		DMA_InitStructure.DMA_MemoryInc = DMA_MemoryInc_Enable;  //�ڴ��ַ�̶�
		DMA_InitStructure.DMA_PeripheralDataSize = DMA_PeripheralDataSize_HalfWord;	//����
		DMA_InitStructure.DMA_MemoryDataSize = DMA_MemoryDataSize_HalfWord;
		DMA_InitStructure.DMA_Mode = DMA_Mode_Circular;		//ѭ������
		DMA_InitStructure.DMA_Priority = DMA_Priority_High;
		DMA_InitStructure.DMA_M2M = DMA_M2M_Disable;
		DMA_Init(DMA1_Channel1, &DMA_InitStructure);
		
		/* Enable DMA channel1 */
		DMA_Cmd(DMA1_Channel1, ENABLE);
		
		/* ADC1 configuration */		
		ADC_InitStructure.ADC_Mode = ADC_Mode_Independent;	//����ADCģʽ
		ADC_InitStructure.ADC_ScanConvMode = ENABLE ; 	 //��ֹɨ��ģʽ��ɨ��ģʽ���ڶ�ͨ���ɼ�
		ADC_InitStructure.ADC_ContinuousConvMode = ENABLE;	//��������ת��ģʽ������ͣ�ؽ���ADCת��
		ADC_InitStructure.ADC_ExternalTrigConv = ADC_ExternalTrigConv_None;	//��ʹ���ⲿ����ת��
		ADC_InitStructure.ADC_DataAlign = ADC_DataAlign_Right; 	//�ɼ������Ҷ���
		ADC_InitStructure.ADC_NbrOfChannel = 6;	 	//Ҫת����ͨ����Ŀ1
		ADC_Init(ADC1, &ADC_InitStructure);
		
		/*����ADCʱ�ӣ�ΪPCLK2��8��Ƶ����9Hz*/
		RCC_ADCCLKConfig(RCC_PCLK2_Div8); 
		/*����ADC1��ͨ��11Ϊ55.	5���������ڣ�����Ϊ1 */ 

// channel map:
// PA0		throttle
// PA1		rudder
// PA2		roll
// PA4		pitch
// PA5		left switch
// PA6		right switch

		ADC_RegularChannelConfig(ADC1, ADC_Channel_2, 1, ADC_SampleTime_239Cycles5);
		ADC_RegularChannelConfig(ADC1, ADC_Channel_4, 2, ADC_SampleTime_239Cycles5);
		ADC_RegularChannelConfig(ADC1, ADC_Channel_0, 3, ADC_SampleTime_239Cycles5);
		ADC_RegularChannelConfig(ADC1, ADC_Channel_1, 4, ADC_SampleTime_239Cycles5);
		ADC_RegularChannelConfig(ADC1, ADC_Channel_5, 5, ADC_SampleTime_239Cycles5);
		ADC_RegularChannelConfig(ADC1, ADC_Channel_6, 6, ADC_SampleTime_239Cycles5);
		
		/* Enable ADC1 DMA */
		ADC_DMACmd(ADC1, ENABLE);
		
		/* Enable ADC1 */
		ADC_Cmd(ADC1, ENABLE);
		
		/*��λУ׼�Ĵ��� */   
		ADC_ResetCalibration(ADC1);
		/*�ȴ�У׼�Ĵ�����λ��� */
		while(ADC_GetResetCalibrationStatus(ADC1));
		
		/* ADCУ׼ */
		ADC_StartCalibration(ADC1);
		/* �ȴ�У׼���*/
		while(ADC_GetCalibrationStatus(ADC1));
		
		/* ����û�в����ⲿ����������ʹ���������ADCת�� */ 
		ADC_SoftwareStartConvCmd(ADC1, ENABLE);
	}

	int sheet1_init()
	{
		NVIC_PriorityGroupConfig(NVIC_PriorityGroup_3);
		
		::cs = &cs;
		::ce = &ce;
		::irq = &irq;
		::dbg = &dbg;
		::dbg2 = &dbg2;
		::SCL = &SCL;
		::SDA = &SDA;
		::spi = &spi;
		::interrupt = &interrupt;
		::timer = &timer;
		
		spi.init(SPI2);
		interrupt.init(GPIOB, GPIO_Pin_12, interrupt_falling);
		
		pa6.set_mode(MODE_IN);
		
		ADC1_Mode_Config();
		
		return 0;
	}

	
	extern "C" void TIM2_IRQHandler(void)
	{
		timer.call_callback();
	}
}

using namespace sheet1;

int board_init()
{
	return sheet1_init();
}



void read_channels(int16_t *channel, int max_channel_count)
{
	if (max_channel_count > sizeof(adc_data)/2)
		max_channel_count = sizeof(adc_data)/2;
	
	memcpy(channel, adc_data, max_channel_count * 2);	
}
