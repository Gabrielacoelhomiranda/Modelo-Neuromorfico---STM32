/* ========================== INCLUDES ========================== */
#include "stm32f7xx_hal.h"
#include "usb_device.h"
#include "usbd_cdc_if.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

/* ========================== DEFINIÇÕES ========================== */
#define ROWS 4
#define COLS 4
#define NUM_TAXELS (ROWS*COLS)
#define MSG_SIZE 128

#define VTH 30.0f
#define A 0.02f
#define B 0.2f
#define C -65.0f
#define D 8.0f
#define DT 1.0f
#define G 10.0f

#define V_MIN 0.5f
#define V_MAX 3.3f

/* ========================== ESTRUTURAS ========================== */
typedef struct {
    float v;
    float u;
    float I;
} Taxel;

typedef struct {
    GPIO_TypeDef *port;
    uint16_t pin;
} GPIO_Map;

/* ========================== HANDLES ========================== */
ADC_HandleTypeDef hadc1;
DMA_HandleTypeDef hdma_adc1;
TIM_HandleTypeDef htim6;
TIM_HandleTypeDef htim2;

extern USBD_HandleTypeDef hUsbDeviceFS;

/* ========================== VARIÁVEIS ========================== */
Taxel taxels[NUM_TAXELS];
uint16_t adc_buffer[COLS];
uint8_t current_row = 0;

/* ========================== GPIO MAP ========================== */
GPIO_Map rows[ROWS] = {
    {GPIOC, GPIO_PIN_0},
    {GPIOC, GPIO_PIN_3},
    {GPIOF, GPIO_PIN_3},
    {GPIOF, GPIO_PIN_5}
};

/* ========================== PROTÓTIPOS ========================== */
void SystemClock_Config(void);
void MX_GPIO_Init(void);
void MX_DMA_Init(void);
void MX_ADC1_Init(void);
void MX_TIM6_Init(void);
void MX_TIM2_Init(void);
void Error_Handler(void);

void select_row(uint8_t row);
void update_taxels_usb(Taxel *t, uint16_t *adc, int num);

/* ========================== FUNÇÕES ========================== */

void select_row(uint8_t row)
{
    for (uint8_t i = 0; i < ROWS; i++)
        HAL_GPIO_WritePin(rows[i].port, rows[i].pin, GPIO_PIN_RESET);

    HAL_GPIO_WritePin(rows[row].port, rows[row].pin, GPIO_PIN_SET);

    for (volatile int i = 0; i < 50; i++); // micro delay
}

/* ========================== UPDATE TAXELS ========================== */
void update_taxels_usb(Taxel *t, uint16_t *adc, int num)
{
    static uint32_t last_usb_time = 0;
    uint32_t now = HAL_GetTick();

    char msg[MSG_SIZE];
    int len = 0;

    for (int i = 0; i < num; i++)
    {
        float V = adc[i] * (V_MAX / 4095.0f);
        float Vn = (V_MAX - V) / (V_MAX - V_MIN);
        Vn = fmaxf(0.0f, fminf(1.0f, Vn));

        t[i].I = G * Vn;

        float v = t[i].v;
        float u = t[i].u;

        v += DT * (0.04f*v*v + 5*v + 140 - u + t[i].I);
        u += DT * (A * (B*v - u));

        /* ===== SPIKE ===== */
        if (v >= VTH)
        {
            v = C;
            u += D;

            uint32_t tstamp = __HAL_TIM_GET_COUNTER(&htim2);

            char spike_msg[64];
            snprintf(spike_msg, sizeof(spike_msg),
                     "SPIKE idx=%d t=%lu\r\n",
                     current_row*COLS + i, tstamp);

            if (hUsbDeviceFS.dev_state == USBD_STATE_CONFIGURED)
                CDC_Transmit_FS((uint8_t*)spike_msg, strlen(spike_msg));
        }

        t[i].v = v;
        t[i].u = u;

        /* ===== DEBUG PERIÓDICO ===== */
        if (now - last_usb_time >= 50)
        {
            len += snprintf(msg + len, MSG_SIZE - len,
                            "Row %d Col %d | V=%.3f I=%.3f\r\n",
                            current_row, i, V, t[i].I);
        }
    }

    if ((now - last_usb_time >= 50) && len > 0 &&
        hUsbDeviceFS.dev_state == USBD_STATE_CONFIGURED)
    {
        CDC_Transmit_FS((uint8_t*)msg, len);
        last_usb_time = now;
    }
}

/* ========================== TIM6 CALLBACK ========================== *///////
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
    if (htim == &htim6)
    {
        update_taxels_usb(&taxels[current_row * COLS], adc_buffer, COLS);
        current_row = (current_row + 1) % ROWS;
        select_row(current_row);
    }
}

/* ========================== MAIN ========================== */
int main(void)
{
    HAL_Init();
    SystemClock_Config();

    MX_GPIO_Init();
    MX_DMA_Init();
    MX_ADC1_Init();
    MX_TIM6_Init();
    MX_TIM2_Init();
    MX_USB_DEVICE_Init();

    while (hUsbDeviceFS.dev_state != USBD_STATE_CONFIGURED);

    for (int i = 0; i < NUM_TAXELS; i++)
    {
        taxels[i].v = -30.0f;
        taxels[i].u = B * taxels[i].v;
        taxels[i].I = 0;
    }

    select_row(current_row);

    /* ADC + DMA CIRCULAR (UMA ÚNICA VEZ) */
    HAL_ADC_Start_DMA(&hadc1, (uint32_t*)adc_buffer, COLS);

    HAL_TIM_Base_Start_IT(&htim6);
    HAL_TIM_Base_Start(&htim2);

    while (1)
    {

    }
}

/* ========================== PERIFÉRICOS ========================== */

void MX_GPIO_Init(void)
{
    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_GPIOF_CLK_ENABLE();

    GPIO_InitTypeDef g = {0};
    g.Mode = GPIO_MODE_OUTPUT_PP;
    g.Pull = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_LOW;

    g.Pin = GPIO_PIN_0 | GPIO_PIN_3;
    HAL_GPIO_Init(GPIOC, &g);

    g.Pin = GPIO_PIN_3 | GPIO_PIN_5;
    HAL_GPIO_Init(GPIOF, &g);
}

void MX_DMA_Init(void)
{
    __HAL_RCC_DMA2_CLK_ENABLE();

    hdma_adc1.Instance = DMA2_Stream0;
    hdma_adc1.Init.Channel = DMA_CHANNEL_0;
    hdma_adc1.Init.Direction = DMA_PERIPH_TO_MEMORY;
    hdma_adc1.Init.PeriphInc = DMA_PINC_DISABLE;
    hdma_adc1.Init.MemInc = DMA_MINC_ENABLE;
    hdma_adc1.Init.PeriphDataAlignment = DMA_PDATAALIGN_HALFWORD;
    hdma_adc1.Init.MemDataAlignment = DMA_MDATAALIGN_HALFWORD;
    hdma_adc1.Init.Mode = DMA_CIRCULAR;
    hdma_adc1.Init.Priority = DMA_PRIORITY_HIGH;

    HAL_DMA_Init(&hdma_adc1);
    __HAL_LINKDMA(&hadc1, DMA_Handle, hdma_adc1);
}

void MX_ADC1_Init(void)
{
    __HAL_RCC_ADC1_CLK_ENABLE();

    ADC_ChannelConfTypeDef c = {0};

    hadc1.Instance = ADC1;
    hadc1.Init.Resolution = ADC_RESOLUTION_12B;
    hadc1.Init.ScanConvMode = ENABLE;
    hadc1.Init.ContinuousConvMode = ENABLE;
    hadc1.Init.NbrOfConversion = COLS;
    hadc1.Init.ExternalTrigConv = ADC_SOFTWARE_START;
    hadc1.Init.DMAContinuousRequests = ENABLE;
    HAL_ADC_Init(&hadc1);

    uint32_t ch[COLS] = {
        ADC_CHANNEL_9,
        ADC_CHANNEL_6,
        ADC_CHANNEL_3,
        ADC_CHANNEL_0
    };

    for (int i = 0; i < COLS; i++)
    {
        c.Channel = ch[i];
        c.Rank = i + 1;
        c.SamplingTime = ADC_SAMPLETIME_15CYCLES;
        HAL_ADC_ConfigChannel(&hadc1, &c);
    }
}

void MX_TIM6_Init(void)
{
    __HAL_RCC_TIM6_CLK_ENABLE();

    htim6.Instance = TIM6;
    htim6.Init.Prescaler = 83;
    htim6.Init.Period = 249;
    HAL_TIM_Base_Init(&htim6);
}

void MX_TIM2_Init(void)
{
    __HAL_RCC_TIM2_CLK_ENABLE();

    htim2.Instance = TIM2;
    htim2.Init.Prescaler = 83;
    htim2.Init.Period = 0xFFFFFFFF;
    HAL_TIM_Base_Init(&htim2);
}

void SystemClock_Config(void)
{
    RCC_OscInitTypeDef o = {0};
    RCC_ClkInitTypeDef c = {0};

    o.OscillatorType = RCC_OSCILLATORTYPE_HSE;
    o.HSEState = RCC_HSE_BYPASS;
    o.PLL.PLLState = RCC_PLL_ON;
    o.PLL.PLLSource = RCC_PLLSOURCE_HSE;
    o.PLL.PLLM = 8;
    o.PLL.PLLN = 336;
    o.PLL.PLLP = RCC_PLLP_DIV2;
    o.PLL.PLLQ = 7;
    HAL_RCC_OscConfig(&o);

    c.ClockType = RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_HCLK |
                  RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    c.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
    c.AHBCLKDivider = RCC_SYSCLK_DIV1;
    c.APB1CLKDivider = RCC_HCLK_DIV4;
    c.APB2CLKDivider = RCC_HCLK_DIV2;
    HAL_RCC_ClockConfig(&c, FLASH_LATENCY_5);
}

void Error_Handler(void)
{
    __disable_irq();
    while (1);
}

