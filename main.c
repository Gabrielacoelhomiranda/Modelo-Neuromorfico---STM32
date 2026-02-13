/* ========================== INCLUDES ========================== */
#include "stm32f7xx_hal.h"
#include "usb_device.h"
#include "usbd_cdc_if.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

/* ========================== DEFINIÇÕES ========================== */
#define ROWS 5
#define COLS 5
#define NUM_TAXELS (ROWS*COLS)

#define VTH 30.0f
#define A 0.02f
#define B 0.2f
#define C -65.0f
#define D 8.0f
#define DT 0.10f
#define G 20.0f

#define V_MIN 0.5f
#define V_MAX 3.3f
#define USB_TX_BUFFER_SIZE 4096
#define USB_PACKET_SIZE 64
#define SEND_INTERVAL_MS 10  // envio ADC a cada 10 ms

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

volatile uint8_t spike_flags[NUM_TAXELS] = {0};
volatile uint16_t last_adc[NUM_TAXELS] = {0};

uint8_t usb_tx_buffer[USB_TX_BUFFER_SIZE];
volatile uint16_t usb_head = 0;
volatile uint16_t usb_tail = 0;

/* ========================== GPIO MAP ========================== */
GPIO_Map rows[ROWS] = {
    {GPIOC, GPIO_PIN_0},
    {GPIOC, GPIO_PIN_3},
    {GPIOF, GPIO_PIN_3},
    {GPIOF, GPIO_PIN_5},
    {GPIOF, GPIO_PIN_10}
};

/* ========================== PROTÓTIPOS ========================== */
void SystemClock_Config(void);
void MX_GPIO_Init(void);
void MX_DMA_Init(void);
void MX_ADC1_Init(void);
void MX_TIM6_Init(void);
void MX_TIM2_Init(void);
void select_row(uint8_t row);
void update_taxels(Taxel *t, uint16_t *adc, uint8_t row_idx);
bool process_spikes(void);
void send_adc_continuous(void);
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
        else
        {
            // Buffer cheio, descarta o dado
            break;
        }
    }
}

void usb_buffer_process(void)
{
    static uint8_t packet[USB_PACKET_SIZE];
    uint16_t len = 0;
    uint16_t temp_tail = usb_tail;

    while (temp_tail != usb_head && len < USB_PACKET_SIZE)
    {
        packet[len++] = usb_tx_buffer[temp_tail];
        temp_tail = (temp_tail + 1) % USB_TX_BUFFER_SIZE;
    }

    if (len > 0)
    {
        if (hUsbDeviceFS.dev_state == USBD_STATE_CONFIGURED)
        {
            if (CDC_Transmit_FS(packet, len) == USBD_OK)
            {
                usb_tail = temp_tail; // confirma envio
            }
        }
    }
}

/* ========================== SELECT ROW ========================== */
void select_row(uint8_t row)
{
    for (uint8_t i = 0; i < ROWS; i++)
        HAL_GPIO_WritePin(rows[i].port, rows[i].pin, GPIO_PIN_RESET);

    HAL_GPIO_WritePin(rows[row].port, rows[row].pin, GPIO_PIN_SET);

    for (volatile int i = 0; i < 50; i++); // pequeno delay
}

/* ========================== IZHIKEVICH ========================== */
void update_taxels(Taxel *t, uint16_t *adc, uint8_t row_idx)
{
    for (int i = 0; i < COLS; i++)
    {
        float V = adc[i] * (V_MAX / 4095.0f);
        float Vn = (V_MAX - V) / (V_MAX - V_MIN);

        t[i].I = G * Vn;

        float v = t[i].v;
        float u = t[i].u;

        v += DT * (0.04f*v*v + 5*v + 140 - u + t[i].I);
        u += DT * (A * (B*v - u));

        int global_idx = row_idx * COLS + i;
        last_adc[global_idx] = adc[i];

        if (v >= VTH)
        {
            v = C;
            u += D;
            spike_flags[global_idx] = 1;
        }

        t[i].v = v;
        t[i].u = u;
    }
}

/* ========================== PROCESS SPIKES ========================== */
bool process_spikes(void)
{
    static char batch_msg[USB_TX_BUFFER_SIZE];
    static uint16_t batch_count = 0;
    char msg[80];
    uint32_t tstamp = __HAL_TIM_GET_COUNTER(&htim2);
    bool has_spike = false;

    for (int i = 0; i < NUM_TAXELS; i++)
    {
        if (spike_flags[i])
        {
            spike_flags[i] = 0;
            has_spike = true;

            int n = snprintf(msg, sizeof(msg),
                             "SPIKE,idx=%d,adc=%d,t=%lu\r\n",
                             i, last_adc[i], tstamp);

            if ((batch_count + n) >= USB_TX_BUFFER_SIZE)
                n = USB_TX_BUFFER_SIZE - batch_count;

            memcpy(batch_msg + batch_count, msg, n);
            batch_count += n;
        }
    }

    if (batch_count > 0)
    {
        usb_buffer_write(batch_msg, batch_count);
        batch_count = 0;
    }

    return has_spike;
}

/* ========================== SEND ADC CONTINUOUS ========================== */
void send_adc_continuous(void)
{
    static char batch_msg[USB_TX_BUFFER_SIZE];
    static uint16_t batch_count = 0;
    static uint32_t last_time = 0;

    uint32_t tstamp = __HAL_TIM_GET_COUNTER(&htim2);
    if ((tstamp - last_time) < (SEND_INTERVAL_MS * 1000)) return;
    last_time = tstamp;

    char msg[128];
    for (uint8_t i = 0; i < NUM_TAXELS; i++)
    {
        int n = snprintf(msg, sizeof(msg),
                         "DATA,idx=%d,adc=%d,t=%lu\r\n",
                         i, last_adc[i], tstamp);

        if ((batch_count + n) >= USB_TX_BUFFER_SIZE)
            n = USB_TX_BUFFER_SIZE - batch_count;

        memcpy(batch_msg + batch_count, msg, n);
        batch_count += n;
    }

    if (batch_count > 0)
    {
        usb_buffer_write(batch_msg, batch_count);
        batch_count = 0;
    }
}

/* ========================== CALLBACK ADC ========================== */
void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef *hadc)
{
    if (hadc == &hadc1)
    {
        select_row(current_row);  // Seleciona linha antes de processar ADC
        update_taxels(&taxels[current_row * COLS], adc_buffer, current_row);
        current_row = (current_row + 1) % ROWS;
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
    HAL_TIM_Base_Start(&htim6);
    HAL_ADC_Start_DMA(&hadc1, (uint32_t*)adc_buffer, COLS);

    usb_buffer_write("BOOT OK\r\n", 9);

    while (1)
    {
        bool spikes = process_spikes();  // envia spikes se houver
        if (!spikes)
        {
            send_adc_continuous();       // envia ADC contínuo se não houver spikes
        }
        usb_buffer_process();            // envia USB
    }
}

/* ========================== GPIO ========================== */
void MX_GPIO_Init(void)
{
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_GPIOF_CLK_ENABLE();

    GPIO_InitTypeDef g = {0};

    /* LINHAS */
    g.Mode = GPIO_MODE_OUTPUT_PP;
    g.Pull = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_LOW;

    g.Pin = GPIO_PIN_0 | GPIO_PIN_3;
    HAL_GPIO_Init(GPIOC, &g);

    g.Pin = GPIO_PIN_3 | GPIO_PIN_5 | GPIO_PIN_10;
    HAL_GPIO_Init(GPIOF, &g);

    /* PINOS ADC */
    g.Mode = GPIO_MODE_ANALOG;
    g.Pull = GPIO_NOPULL;
    g.Pin = GPIO_PIN_0 | GPIO_PIN_3 | GPIO_PIN_4; // ADC0, ADC3, ADC4
    HAL_GPIO_Init(GPIOA, &g);

    g.Pin = GPIO_PIN_2 | GPIO_PIN_4; // ADC6 e ADC9
    HAL_GPIO_Init(GPIOF, &g);       // Ajuste dependendo do seu mapeamento
}

/* ========================== DMA ========================== */
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
    hdma_adc1.Init.FIFOMode = DMA_FIFOMODE_DISABLE;

    HAL_DMA_Init(&hdma_adc1);
    __HAL_LINKDMA(&hadc1, DMA_Handle, hdma_adc1);

    HAL_NVIC_SetPriority(DMA2_Stream0_IRQn, 0, 0);
    HAL_NVIC_EnableIRQ(DMA2_Stream0_IRQn);
}

void DMA2_Stream0_IRQHandler(void)
{
    HAL_DMA_IRQHandler(&hdma_adc1);
}

/* ========================== ADC ========================== */
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
    hadc1.Init.DMAContinuousRequests = ENABLE;
    HAL_ADC_Init(&hadc1);

    uint32_t ch[COLS] = {
        ADC_CHANNEL_9,
        ADC_CHANNEL_6,
        ADC_CHANNEL_3,
        ADC_CHANNEL_0,
        ADC_CHANNEL_4
    };

    for (int i = 0; i < COLS; i++)
    {
        c.Channel = ch[i];
        c.Rank = i + 1;
        c.SamplingTime = ADC_SAMPLETIME_15CYCLES;
        HAL_ADC_ConfigChannel(&hadc1, &c);
    }
}

/* ========================== TIM6 ========================== */
void MX_TIM6_Init(void)
{
    __HAL_RCC_TIM6_CLK_ENABLE();

    htim6.Instance = TIM6;
    htim6.Init.Prescaler = 83;
    htim6.Init.Period = 199;

    HAL_TIM_Base_Init(&htim6);

    TIM_MasterConfigTypeDef s = {0};
    s.MasterOutputTrigger = TIM_TRGO_UPDATE;
    s.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
    HAL_TIMEx_MasterConfigSynchronization(&htim6, &s);
}

/* ========================== TIM2 ========================== */
void MX_TIM2_Init(void)
{
    __HAL_RCC_TIM2_CLK_ENABLE();

    htim2.Instance = TIM2;
    htim2.Init.Prescaler = 83;
    htim2.Init.Period = 0xFFFFFFFF;
    HAL_TIM_Base_Init(&htim2);
}

/* ========================== CLOCK ========================== */
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

