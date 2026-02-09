/* ========================== INCLUDES ========================== */
// BIBLIOTECAS
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
#define NUM_TAXELS (ROWS*COLS) //16 TAXELS

/* Modelo de Izhikevich */
#define VTH 30.0f // LIMIAR DE POTENCIAL DE MEMBRANA
#define A 0.02f
#define B 0.2f
#define C -65.0f
#define D 8.0f
#define DT 1.0f
#define G 10.0f // GANHO
#define IDX_VALIDACAO 10  // taxel que você quer validar

#define V_MIN 0.5f
#define V_MAX 3.3f

#define USB_TX_BUFFER_SIZE 1024  // BUFFER CIRCULAR

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

/* ========================== USB BUFFER ========================== */
uint8_t usb_tx_buffer[USB_TX_BUFFER_SIZE];
volatile uint16_t usb_head = 0;
volatile uint16_t usb_tail = 0;

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

void select_row(uint8_t row);
void update_taxels(Taxel *t, uint16_t *adc);
void usb_buffer_write(const char *data, uint16_t len);
void usb_buffer_process(void);

/* ========================== USB BUFFER ========================== */
void usb_buffer_write(const char *data, uint16_t len)
{
    for (uint16_t i = 0; i < len; i++)
    {
        uint16_t next = (usb_head + 1) % USB_TX_BUFFER_SIZE;
        if (next != usb_tail)
        {
            usb_tx_buffer[usb_head] = data[i];
            usb_head = next;
        }
    }
}

void usb_buffer_process(void)
{
    static uint8_t packet[64];

    if (usb_head == usb_tail) return;
    if (hUsbDeviceFS.dev_state != USBD_STATE_CONFIGURED) return;

    uint16_t len = 0;
    uint16_t temp_tail = usb_tail;

    while (temp_tail != usb_head && len < sizeof(packet))
    {
        packet[len++] = usb_tx_buffer[temp_tail];
        temp_tail = (temp_tail + 1) % USB_TX_BUFFER_SIZE;
    }

    if (CDC_Transmit_FS(packet, len) == USBD_OK)
        usb_tail = temp_tail;
}

/* ========================== ROW SELECT ========================== */
void select_row(uint8_t row)
{
    for (uint8_t i = 0; i < ROWS; i++)
        HAL_GPIO_WritePin(rows[i].port, rows[i].pin, GPIO_PIN_RESET);

    HAL_GPIO_WritePin(rows[row].port, rows[row].pin, GPIO_PIN_SET);

    for (volatile int i = 0; i < 50; i++);
}

/* ========================== TAXELS UPDATE ========================== */
void update_taxels(Taxel *t, uint16_t *adc) {
    char msg[128];

    // TIM2: contador em µs
    uint32_t tstamp = __HAL_TIM_GET_COUNTER(&htim2);
    uint32_t t_ms = tstamp / 1000;  // converte para ms

    // Atualiza cada coluna do taxel
    for (int i = 0; i < COLS; i++) {
        // Normaliza leitura ADC
        float V  = adc[i] * (V_MAX / 4095.0f);
        float Vn = (V_MAX - V) / (V_MAX - V_MIN);
        Vn = fmaxf(0.0f, fminf(1.0f, Vn));

        // Corrente de entrada para Izhikevich
        t[i].I = G * Vn;

        // Atualiza o neurônio
        float v = t[i].v;
        float u = t[i].u;

        v += DT * (0.04f * v * v + 5 * v + 140 - u + t[i].I);
        u += DT * (A * (B * v - u));

        // Detecta spike
        if (v >= VTH) {
            v = C;
            u += D;

            int n = snprintf(msg, sizeof(msg),
                             "SPIKE,idx=%d,t=%lu\r\n",
                             current_row * COLS + i, t_ms);
            usb_buffer_write(msg, n);
        }

        t[i].v = v;
        t[i].u = u;

        // Envia corrente I para o Python (opcional: pode ser só para IDX_VALIDACAO)
        if ((current_row * COLS + i) == IDX_VALIDACAO)
        {
            int n = snprintf(msg, sizeof(msg),
                             "I,idx=%d,t=%lu,I=%.3f\r\n",
                             current_row * COLS + i, t_ms, t[i].I);
            usb_buffer_write(msg, n);
        }
    }
}




/* ========================== ADC CALLBACK ========================== */
void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef *hadc)
{
    if (hadc == &hadc1)
    {


        update_taxels(&taxels[current_row * COLS], adc_buffer);

        current_row = (current_row + 1) % ROWS;
        select_row(current_row);
    }
}


/* ========================== TIM6 CALLBACK ========================== */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
    if (htim == &htim6)
    {
        /* TIM6 apenas gera TRGO */
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

    for (int i = 0; i < NUM_TAXELS; i++)
    {
        taxels[i].v = -30.0f;
        taxels[i].u = B * taxels[i].v;
        taxels[i].I = 0.0f;
    }

    select_row(0);

    HAL_TIM_Base_Start(&htim2);
    HAL_ADC_Start_DMA(&hadc1, (uint32_t*)adc_buffer, COLS);
    HAL_TIM_Base_Start_IT(&htim6);

    usb_buffer_write("BOOT OK\r\n", 9);

    while (1)
    {
        usb_buffer_process();
        HAL_Delay(1);
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

    /* 🔥 ISSO AQUI É O QUE FALTAVA */
    HAL_NVIC_SetPriority(DMA2_Stream0_IRQn, 0, 0);
    HAL_NVIC_EnableIRQ(DMA2_Stream0_IRQn);
}


void MX_ADC1_Init(void)
{
    __HAL_RCC_ADC1_CLK_ENABLE();

    ADC_ChannelConfTypeDef c = {0};

    hadc1.Instance = ADC1;
    hadc1.Init.Resolution = ADC_RESOLUTION_12B;
    hadc1.Init.ScanConvMode = ENABLE;
    hadc1.Init.ContinuousConvMode = DISABLE;
    hadc1.Init.NbrOfConversion = COLS;
    hadc1.Init.ExternalTrigConv = ADC_EXTERNALTRIGCONV_T6_TRGO;
    hadc1.Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_RISING;
    hadc1.Init.DMAContinuousRequests = ENABLE;   // <<< CRÍTICO NO F7
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

    TIM_MasterConfigTypeDef s = {0};
    s.MasterOutputTrigger = TIM_TRGO_UPDATE;
    s.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
    HAL_TIMEx_MasterConfigSynchronization(&htim6, &s);
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

void DMA2_Stream0_IRQHandler(void)
{
    HAL_DMA_IRQHandler(&hdma_adc1);
}

void Error_Handler(void)
{
    __disable_irq();
    while (1);
}
