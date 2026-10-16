#include "stm32f7xx_hal.h"
#include "usb_device.h"
#include "usbd_cdc_if.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

/* ============================================================
 * SISTEMA COM DOIS SENSORES 5x5
 *
 * SENSOR 1:
 * PA0, PA3, PA4, PA6, PB1
 *
 * SENSOR 2:
 * PA5, PB0, PC0, PC3, PC2
 *
 * LINHAS COMPARTILHADAS:
 * L0 = PD0
 * L1 = PD1
 * L2 = PF3
 * L3 = PF5
 * L4 = PF10
 *
 * ADC:
 * 10 canais por trigger
 * 5 linhas por quadro
 *
 * USB:
 * DATA,S1,<25 ADCs>,S2,<25 ADCs>,t=<timestamp>
 * MEDIA_S2,<4 medias>,t=<timestamp>
 * MM1=1,t=<timestamp>
 * MM2=1,t=<timestamp>

#define DEBUG_ADC_PORT GPIOB
#define DEBUG_ADC_PIN  GPIO_PIN_8

#define DEBUG_IZH_PORT GPIOB
#define DEBUG_IZH_PIN  GPIO_PIN_9


 * MATRIZES
 * ============================================================ */

#define NUM_SENSORS 2U

#define ROWS 5U
#define COLS 5U

#define NUM_TAXELS (ROWS * COLS)
#define ADC_CHANNELS (NUM_SENSORS * COLS)

#define DIFF_BUFFER 25U


/* ============================================================
 * MODELO IZH - RA
 * ============================================================ */

#define A_RA 0.1f
#define B_RA 0.2f
#define C_RA -65.0f
#define D_RA 2.0f
#define G_RA 1000.0f


/* ============================================================
 * MODELO IZH - SA
 * ============================================================ */

#define A_SA 0.02f
#define B_SA 0.2f
#define C_SA -65.0f
#define D_SA 8.0f
#define G_SA 40.0f


/* ============================================================
 * NEURONIOS CUNEIFORMES
 * ============================================================ */

#define A_CN 0.02f
#define B_CN 0.2f
#define C_CN -65.0f
#define D_CN 8.0f

#define G_CN 100000.0f
#define DT 0.10f

#define VTH 30.0f

#define V_MIN 0.0f
#define V_MAX 3.2f


/* ============================================================
 * USB
 * ============================================================ */

#define USB_TX_BUFFER_SIZE 8192U
#define USB_PACKET_SIZE 64U
#define ADC_FRAME_QUEUE_SIZE 8U


/* ============================================================
 * SINAPSES
 * ============================================================ */

#define G_MAX 1.0f
#define W_INB 0.0f

#define TEMPLATE_SIZE 20U


/* ============================================================
 * ESTRUTURAS
 * ============================================================ */

typedef struct
{
    float v_RA;
    float u_RA;

    float v_SA;
    float u_SA;

    float I;

} Taxel;


typedef struct
{
    float v_CN;
    float u_CN;

} CuneateNeuron;

typedef struct
{
    uint16_t adc[NUM_SENSORS][NUM_TAXELS];
    uint32_t timestamp;

} ADCFrame;


/* ============================================================
 * HANDLES
 * ============================================================ */

ADC_HandleTypeDef hadc1;
DMA_HandleTypeDef hdma_adc1;

TIM_HandleTypeDef htim6;
TIM_HandleTypeDef htim2;

extern USBD_HandleTypeDef hUsbDeviceFS;


/* ============================================================
 * VARIAVEIS DOS NEURONIOS
 * ============================================================ */

Taxel taxels[NUM_SENSORS][NUM_TAXELS];

CuneateNeuron CN[NUM_SENSORS];


/* ============================================================
 * ADC + DMA
 * ============================================================ */

uint16_t adc_buffer[ADC_CHANNELS] = {0};

volatile uint16_t
last_adc[NUM_SENSORS][NUM_TAXELS] = {0};


/* ============================================================
 * FILA DE QUADROS ADC
 * ============================================================ */

static ADCFrame adc_frame_queue[ADC_FRAME_QUEUE_SIZE];

static volatile uint8_t adc_queue_head = 0U;
static volatile uint8_t adc_queue_tail = 0U;

/*
 * Contadores de diagnóstico.
 */
volatile uint32_t adc_frames_dropped = 0U;
volatile uint32_t usb_messages_dropped = 0U;


/* ============================================================
 * VARREDURA DAS LINHAS
 * ============================================================ */

volatile uint8_t current_row = 0U;


/* ============================================================
 * DERIVADA PARA RA
 * ============================================================ */

float I_buffer
    [NUM_SENSORS]
    [NUM_TAXELS]
    [DIFF_BUFFER] = {0};

uint8_t I_index
    [NUM_SENSORS]
    [NUM_TAXELS] = {0};


/* ============================================================
 * TEMPLATE SINAPTICO
 * ============================================================ */

float g_template[TEMPLATE_SIZE];

float gain_RA[NUM_SENSORS][NUM_TAXELS];
float gain_SA[NUM_SENSORS][NUM_TAXELS];


const float gain_RA_init[NUM_TAXELS] =
{
    1,1,1,1,1,
    1,1,1,1,1,
    1,1,1,1,1,
    1,1,1,1,1,
    1,1,1,1,1
};


const float gain_SA_init[NUM_TAXELS] =
{
    1,1,1,1,1,
    1,1,1,1,1,
    1,1,1,1,1,
    1,1,1,1,1,
    1,1,1,1,1
};


uint8_t template_idx_RA
    [NUM_SENSORS]
    [NUM_TAXELS] = {0};

uint8_t template_idx_SA
    [NUM_SENSORS]
    [NUM_TAXELS] = {0};

uint8_t template_idx_INH_RA
    [NUM_SENSORS]
    [NUM_TAXELS] = {0};

uint8_t template_idx_INH_SA
    [NUM_SENSORS]
    [NUM_TAXELS] = {0};


/* ============================================================
 * CORRENTE DOS CN_MM
 * ============================================================ */

float I_total_MM[NUM_SENSORS] = {0.0f, 0.0f};


/* ============================================================
 * EVENTOS CN_MM
 * ============================================================ */

volatile uint8_t cn_mm_spike_pending[NUM_SENSORS] = {0};

volatile uint32_t
cn_mm_spike_timestamp[NUM_SENSORS] = {0};


/* ============================================================
 * USB RING BUFFER
 * ============================================================ */

static uint8_t usb_tx_buffer[USB_TX_BUFFER_SIZE];

static volatile uint16_t usb_head = 0U;
static volatile uint16_t usb_tail = 0U;


/* ============================================================
 * PROTOTIPOS
 * ============================================================ */

void SystemClock_Config(void);

void MX_GPIO_Init(void);
void MX_DMA_Init(void);
void MX_ADC1_Init(void);

void MX_TIM6_Init(void);
void MX_TIM2_Init(void);

void init_template(void);
void select_row(uint8_t row);

bool izhikevich_step(
    float *v,
    float *u,
    float I,
    float a,
    float b,
    float c,
    float d
);

static bool sensor2_taxel_ativo_cn(uint8_t idx);

void update_taxels(
    uint8_t sensor,
    Taxel *t,
    uint16_t *adc,
    uint8_t row_idx
);

void process_cuneate(
    uint8_t sensor,
    uint32_t tstamp
);

bool send_adc_frame_both(
    const uint16_t frame[NUM_SENSORS][NUM_TAXELS],
    uint32_t tstamp
);

void send_pending_cn_mm(void);

uint16_t usb_buffer_free_space(void);

bool usb_buffer_write_atomic(
    const char *data,
    uint16_t len
);

void usb_buffer_process(void);

void Error_Handler(void);


/* ============================================================
 * TEMPLATE SINAPTICO
 * ============================================================ */

void init_template(void)
{
    g_template[0] = 0.0f;

    for (uint32_t i = 1U; i < TEMPLATE_SIZE; i++)
    {
        g_template[i] =
            G_MAX * expf(-(float)(i - 1U) / 4.0f);
    }
}

uint16_t usb_buffer_free_space(void)
{
    uint16_t head = usb_head;
    uint16_t tail = usb_tail;

    if (head >= tail)
    {
        return (uint16_t)(
            (USB_TX_BUFFER_SIZE - 1U) - (head - tail)
        );
    }

    return (uint16_t)(tail - head - 1U);
}

bool usb_buffer_write_atomic(
    const char *data,
    uint16_t len
)
{
    if (len == 0U)
    {
        return true;
    }

    if (usb_buffer_free_space() < len)
    {
        usb_messages_dropped++;
        return false;
    }

    uint16_t head = usb_head;

    for (uint16_t i = 0U; i < len; i++)
    {
        usb_tx_buffer[head] = (uint8_t)data[i];

        head++;

        if (head >= USB_TX_BUFFER_SIZE)
        {
            head = 0U;
        }
    }

    __DMB();

    usb_head = head;

    return true;
}


/* ============================================================
 * USB - TRANSMISSAO DE PACOTES
 * ============================================================ */

void usb_buffer_process(void)
{
    static uint8_t packet[USB_PACKET_SIZE];

    if (hUsbDeviceFS.dev_state != USBD_STATE_CONFIGURED)
    {
        return;
    }

    uint16_t tail = usb_tail;
    uint16_t head = usb_head;

    if (tail == head)
    {
        return;
    }

    uint16_t len = 0U;

    while ((tail != head) && (len < USB_PACKET_SIZE))
    {
        packet[len++] = usb_tx_buffer[tail];

        tail++;

        if (tail >= USB_TX_BUFFER_SIZE)
        {
            tail = 0U;
        }
    }

    if (CDC_Transmit_FS(packet, len) == USBD_OK)
    {
        /*
         * O CDC aceitou o pacote; libera os bytes correspondentes.
         */
        __DMB();

        usb_tail = tail;
    }
}


/* ============================================================
 * SELECAO DAS LINHAS
 * ============================================================ */

static const uint8_t row_masks[ROWS] =
{
    0b11110,
    0b11101,
    0b11011,
    0b10111,
    0b01111
};


void select_row(uint8_t row)
{
    if (row >= ROWS)
    {
        return;
    }

    uint8_t mask = row_masks[row];

    HAL_GPIO_WritePin(
        GPIOD,
        GPIO_PIN_0,
        (mask & (1U << 0))
            ? GPIO_PIN_SET
            : GPIO_PIN_RESET
    );

    HAL_GPIO_WritePin(
        GPIOD,
        GPIO_PIN_1,
        (mask & (1U << 1))
            ? GPIO_PIN_SET
            : GPIO_PIN_RESET
    );

    HAL_GPIO_WritePin(
        GPIOF,
        GPIO_PIN_3,
        (mask & (1U << 2))
            ? GPIO_PIN_SET
            : GPIO_PIN_RESET
    );

    HAL_GPIO_WritePin(
        GPIOF,
        GPIO_PIN_5,
        (mask & (1U << 3))
            ? GPIO_PIN_SET
            : GPIO_PIN_RESET
    );

    HAL_GPIO_WritePin(
        GPIOF,
        GPIO_PIN_10,
        (mask & (1U << 4))
            ? GPIO_PIN_SET
            : GPIO_PIN_RESET
    );
}


/* ============================================================
 * MODELO DE IZHIKEVICH
 * ============================================================ */

bool izhikevich_step(
    float *v,
    float *u,
    float I,
    float a,
    float b,
    float c,
    float d
)
{
    *v += DT *
    (
        0.04f * (*v) * (*v)
        + 5.0f * (*v)
        + 140.0f
        - (*u)
        + I
    );

    *u += DT * (a * (b * (*v) - (*u)));

    if (*v >= VTH)
    {
        *v = c;
        *u += d;

        return true;
    }

    return false;
}

static bool sensor2_taxel_ativo_cn(uint8_t idx)
{
    switch (idx)
    {
        case 0:
        case 1:
        case 5:
        case 6:

        case 3:
        case 4:
        case 8:
        case 9:

        case 15:
        case 16:
        case 20:
        case 21:

        case 18:
        case 19:
        case 23:
        case 24:
            return true;

        default:
            return false;
    }
}


/* ============================================================
 * PROCESSAMENTO DOS TAXELS
 * ============================================================ */

void update_taxels(
    uint8_t sensor,
    Taxel *t,
    uint16_t *adc,
    uint8_t row_idx
)
{
    HAL_GPIO_WritePin(
        DEBUG_IZH_PORT,
        DEBUG_IZH_PIN,
        GPIO_PIN_SET
    );

    for (uint32_t i = 0U; i < COLS; i++)
    {
        uint32_t global_idx = row_idx * COLS + i;

        /* ADC -> tensao normalizada */

        float V = adc[i] * (V_MAX / 4095.0f);

        float Vn = (V_MAX - V) / (V_MAX - V_MIN);

        float I_raw = Vn;

        /* Derivada para RA */

        uint8_t idx = I_index[sensor][global_idx];

        float I_old =
            I_buffer[sensor][global_idx][idx];

        I_buffer[sensor][global_idx][idx] = I_raw;

        I_index[sensor][global_idx] =
            (uint8_t)((idx + 1U) % DIFF_BUFFER);

        float dI =
            fabsf(I_raw - I_old) /
            (DIFF_BUFFER * DT);

        if (fabsf(dI) < 0.01f)
        {
            dI = 0.0f;
        }

        float I_RA = G_RA * fabsf(dI);
        float I_SA = G_SA * I_raw;

        /* Aferente RA */

        bool spike_ra = izhikevich_step(
            &t[i].v_RA,
            &t[i].u_RA,
            I_RA,
            A_RA,
            B_RA,
            C_RA,
            D_RA
        );

        if (spike_ra)
        {
            template_idx_RA[sensor][global_idx] = 1U;
            template_idx_INH_RA[sensor][global_idx] = 1U;
        }

        /* Aferente SA */

        bool spike_sa = izhikevich_step(
            &t[i].v_SA,
            &t[i].u_SA,
            I_SA,
            A_SA,
            B_SA,
            C_SA,
            D_SA
        );

        if (spike_sa)
        {
            template_idx_SA[sensor][global_idx] = 1U;
            template_idx_INH_SA[sensor][global_idx] = 1U;
        }

        /* Sinapse excitatoria RA */

        float I_ra = 0.0f;

        uint8_t idx_ra =
            template_idx_RA[sensor][global_idx];

        if (idx_ra > 0U)
        {
            I_ra =
                gain_RA[sensor][global_idx] *
                g_template[idx_ra];

            idx_ra++;

            if (idx_ra >= TEMPLATE_SIZE)
            {
                idx_ra = 0U;
            }

            template_idx_RA[sensor][global_idx] = idx_ra;
        }

        /* Sinapse excitatoria SA */

        float I_sa = 0.0f;

        uint8_t idx_sa =
            template_idx_SA[sensor][global_idx];

        if (idx_sa > 0U)
        {
            I_sa =
                gain_SA[sensor][global_idx] *
                g_template[idx_sa];

            idx_sa++;

            if (idx_sa >= TEMPLATE_SIZE)
            {
                idx_sa = 0U;
            }

            template_idx_SA[sensor][global_idx] = idx_sa;
        }

        /* Integracao multimodal */

        bool contribui_cn = false;

        if (sensor == 0U)
        {
            /* Sensor 1: 25 taxels */
            contribui_cn = true;
        }
        else
        {
            /* Sensor 2: 16 taxels dos cantos */
            contribui_cn =
                sensor2_taxel_ativo_cn((uint8_t)global_idx);
        }

        if (contribui_cn)
        {
            I_total_MM[sensor] += I_ra;
            I_total_MM[sensor] += I_sa;
        }

        /* Guarda todos os valores ADC */

        last_adc[sensor][global_idx] = adc[i];
    }

    HAL_GPIO_WritePin(
        DEBUG_IZH_PORT,
        DEBUG_IZH_PIN,
        GPIO_PIN_RESET
    );
}


/* ============================================================
 * PROCESSAMENTO DOS NEURONIOS CUNEIFORMES
 * ============================================================ */

void process_cuneate(
    uint8_t sensor,
    uint32_t tstamp
)
{
    float I_inh = 0.0f;

    /* Inibicao RA */

    for (uint32_t k = 0U; k < NUM_TAXELS; k++)
    {
        uint8_t idx_inh_ra =
            template_idx_INH_RA[sensor][k];

        if (idx_inh_ra > 0U)
        {
            I_inh -= W_INB * g_template[idx_inh_ra];

            idx_inh_ra++;

            if (idx_inh_ra >= TEMPLATE_SIZE)
            {
                idx_inh_ra = 0U;
            }

            template_idx_INH_RA[sensor][k] = idx_inh_ra;
        }
    }

    /* Inibicao SA */

    for (uint32_t k = 0U; k < NUM_TAXELS; k++)
    {
        uint8_t idx_inh_sa =
            template_idx_INH_SA[sensor][k];

        if (idx_inh_sa > 0U)
        {
            I_inh -= W_INB * g_template[idx_inh_sa];

            idx_inh_sa++;

            if (idx_inh_sa >= TEMPLATE_SIZE)
            {
                idx_inh_sa = 0U;
            }

            template_idx_INH_SA[sensor][k] = idx_inh_sa;
        }
    }

    /* Normalizacao */

    float n_taxels_ativos =
        (sensor == 0U) ? 25.0f : 16.0f;

    float I_MM_norm =
        I_total_MM[sensor] /
        (n_taxels_ativos * 2.0f);

    float I_final_MM =
        (I_MM_norm + I_inh) * G_CN;

    bool spike_mm = izhikevich_step(
        &CN[sensor].v_CN,
        &CN[sensor].u_CN,
        I_final_MM,
        A_CN,
        B_CN,
        C_CN,
        D_CN
    );

    if (spike_mm)
    {
        cn_mm_spike_timestamp[sensor] = tstamp;
        cn_mm_spike_pending[sensor] = 1U;
    }

    I_total_MM[sensor] = 0.0f;
}


/* ============================================================
 * ENVIO ADC + MEDIAS DO SENSOR 2
 * ============================================================ */

bool send_adc_frame_both(
    const uint16_t frame[NUM_SENSORS][NUM_TAXELS],
    uint32_t tstamp
)
{
    char msg[512];

    int n = snprintf(
        msg,
        sizeof(msg),

        "DATA,S1,"
        "%u,%u,%u,%u,%u,"
        "%u,%u,%u,%u,%u,"
        "%u,%u,%u,%u,%u,"
        "%u,%u,%u,%u,%u,"
        "%u,%u,%u,%u,%u,"
        "S2,"
        "%u,%u,%u,%u,%u,"
        "%u,%u,%u,%u,%u,"
        "%u,%u,%u,%u,%u,"
        "%u,%u,%u,%u,%u,"
        "%u,%u,%u,%u,%u,"
        "t=%lu\r\n",

        (unsigned)frame[0][0],
        (unsigned)frame[0][1],
        (unsigned)frame[0][2],
        (unsigned)frame[0][3],
        (unsigned)frame[0][4],
        (unsigned)frame[0][5],
        (unsigned)frame[0][6],
        (unsigned)frame[0][7],
        (unsigned)frame[0][8],
        (unsigned)frame[0][9],
        (unsigned)frame[0][10],
        (unsigned)frame[0][11],
        (unsigned)frame[0][12],
        (unsigned)frame[0][13],
        (unsigned)frame[0][14],
        (unsigned)frame[0][15],
        (unsigned)frame[0][16],
        (unsigned)frame[0][17],
        (unsigned)frame[0][18],
        (unsigned)frame[0][19],
        (unsigned)frame[0][20],
        (unsigned)frame[0][21],
        (unsigned)frame[0][22],
        (unsigned)frame[0][23],
        (unsigned)frame[0][24],

        (unsigned)frame[1][0],
        (unsigned)frame[1][1],
        (unsigned)frame[1][2],
        (unsigned)frame[1][3],
        (unsigned)frame[1][4],
        (unsigned)frame[1][5],
        (unsigned)frame[1][6],
        (unsigned)frame[1][7],
        (unsigned)frame[1][8],
        (unsigned)frame[1][9],
        (unsigned)frame[1][10],
        (unsigned)frame[1][11],
        (unsigned)frame[1][12],
        (unsigned)frame[1][13],
        (unsigned)frame[1][14],
        (unsigned)frame[1][15],
        (unsigned)frame[1][16],
        (unsigned)frame[1][17],
        (unsigned)frame[1][18],
        (unsigned)frame[1][19],
        (unsigned)frame[1][20],
        (unsigned)frame[1][21],
        (unsigned)frame[1][22],
        (unsigned)frame[1][23],
        (unsigned)frame[1][24],

        (unsigned long)tstamp
    );

    if (n <= 0 || (size_t)n >= sizeof(msg))
    {
        return false;
    }

    /* Medias das quatro regioes 2x2 do Sensor 2 */

    const uint16_t *s2 = frame[1];

    uint16_t media_sup_esq = (uint16_t)(
        ((uint32_t)s2[0] + s2[1] + s2[5] + s2[6] + 2U) / 4U
    );

    uint16_t media_sup_dir = (uint16_t)(
        ((uint32_t)s2[3] + s2[4] + s2[8] + s2[9] + 2U) / 4U
    );

    uint16_t media_inf_esq = (uint16_t)(
        ((uint32_t)s2[15] + s2[16] + s2[20] + s2[21] + 2U) / 4U
    );

    uint16_t media_inf_dir = (uint16_t)(
        ((uint32_t)s2[18] + s2[19] + s2[23] + s2[24] + 2U) / 4U
    );

    size_t restante = sizeof(msg) - (size_t)n;

    int m = snprintf(
        &msg[n],
        restante,
        "MEDIA_S2,%u,%u,%u,%u,t=%lu\r\n",

        (unsigned)media_sup_esq,
        (unsigned)media_sup_dir,
        (unsigned)media_inf_esq,
        (unsigned)media_inf_dir,
        (unsigned long)tstamp
    );

    if (m <= 0 || (size_t)m >= restante)
    {
        return false;
    }

    n += m;

    return usb_buffer_write_atomic(msg, (uint16_t)n);
}


/* ============================================================
 * ENVIO DOS EVENTOS CN_MM
 * ============================================================ */

void send_pending_cn_mm(void)
{
    for (uint8_t sensor = 0U; sensor < NUM_SENSORS; sensor++)
    {
        uint32_t timestamp;
        uint8_t pending;

        /*
         * Captura e limpa o evento de forma protegida.
         * Um novo evento pode ser registrado após a secao critica.
         */
        __disable_irq();

        pending = cn_mm_spike_pending[sensor];
        timestamp = cn_mm_spike_timestamp[sensor];

        if (pending != 0U)
        {
            cn_mm_spike_pending[sensor] = 0U;
        }

        __enable_irq();

        if (pending == 0U)
        {
            continue;
        }

        char msg[64];

        int n;

        if (sensor == 0U)
        {
            n = snprintf(
                msg,
                sizeof(msg),
                "MM1=1,t=%lu\r\n",
                (unsigned long)timestamp
            );
        }
        else
        {
            n = snprintf(
                msg,
                sizeof(msg),
                "MM2=1,t=%lu\r\n",
                (unsigned long)timestamp
            );
        }

        if (n > 0 && (size_t)n < sizeof(msg))
        {
            if (!usb_buffer_write_atomic(msg, (uint16_t)n))
            {

            }
        }
    }
}


/* ============================================================
 * CALLBACK ADC
 * ============================================================ */

void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef *hadc)
{
    if (hadc != &hadc1)
    {
        return;
    }

    HAL_GPIO_WritePin(
        DEBUG_ADC_PORT,
        DEBUG_ADC_PIN,
        GPIO_PIN_SET
    );

    /* Sensor 1: cinco canais */

    update_taxels(
        0U,
        &taxels[0][current_row * COLS],
        &adc_buffer[0],
        current_row
    );

    /* Sensor 2: cinco canais */

    update_taxels(
        1U,
        &taxels[1][current_row * COLS],
        &adc_buffer[COLS],
        current_row
    );

    /* Fim da quinta linha: quadro completo */

    if (current_row == (ROWS - 1U))
    {
        uint32_t tstamp =
            __HAL_TIM_GET_COUNTER(&htim2);

        process_cuneate(0U, tstamp);
        process_cuneate(1U, tstamp);

        uint8_t head = adc_queue_head;

        uint8_t next_head =
            (uint8_t)((head + 1U) % ADC_FRAME_QUEUE_SIZE);

        /*
         * So escreve se houver uma posicao livre.
         */
        if (next_head != adc_queue_tail)
        {
            ADCFrame *dst = &adc_frame_queue[head];

            for (uint8_t sensor = 0U;
                 sensor < NUM_SENSORS;
                 sensor++)
            {
                for (uint8_t i = 0U;
                     i < NUM_TAXELS;
                     i++)
                {
                    dst->adc[sensor][i] =
                        last_adc[sensor][i];
                }
            }

            dst->timestamp = tstamp;

            /*
             * Publica a posicao somente depois da copia completa.
             */
            __DMB();

            adc_queue_head = next_head;
        }
        else
        {
            adc_frames_dropped++;
        }
    }

    /* Seleciona a proxima linha */

    current_row = (uint8_t)((current_row + 1U) % ROWS);

    select_row(current_row);

    HAL_GPIO_WritePin(
        DEBUG_ADC_PORT,
        DEBUG_ADC_PIN,
        GPIO_PIN_RESET
    );
}


/* ============================================================
 * MAIN
 * ============================================================ */

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

    init_template();

    /* Inicializa os dois CN_MM */

    for (uint32_t sensor = 0U;
         sensor < NUM_SENSORS;
         sensor++)
    {
        CN[sensor].v_CN = -30.0f;

        CN[sensor].u_CN =
            B_CN * CN[sensor].v_CN;
    }

    /* Inicializa os 50 taxels */

    for (uint32_t sensor = 0U;
         sensor < NUM_SENSORS;
         sensor++)
    {
        for (uint32_t i = 0U; i < NUM_TAXELS; i++)
        {
            taxels[sensor][i].v_RA = -30.0f;

            taxels[sensor][i].u_RA =
                B_RA * taxels[sensor][i].v_RA;

            taxels[sensor][i].v_SA = -30.0f;

            taxels[sensor][i].u_SA =
                B_SA * taxels[sensor][i].v_SA;

            taxels[sensor][i].I = 0.0f;

            gain_RA[sensor][i] = gain_RA_init[i];
            gain_SA[sensor][i] = gain_SA_init[i];
        }
    }

    /* Primeira linha */

    current_row = 0U;

    select_row(current_row);

    /* Inicia timestamp */

    if (HAL_TIM_Base_Start(&htim2) != HAL_OK)
    {
        Error_Handler();
    }

    /* Inicia ADC + DMA */

    if (HAL_ADC_Start_DMA(
            &hadc1,
            (uint32_t *)adc_buffer,
            ADC_CHANNELS) != HAL_OK)
    {
        Error_Handler();
    }

    /* Inicia trigger do ADC */

    if (HAL_TIM_Base_Start(&htim6) != HAL_OK)
    {
        Error_Handler();
    }

    const char boot_msg[] =
        "BOOT OK - S1+S2 ADC / MM EVENT ONLY\r\n";

    (void)usb_buffer_write_atomic(
        boot_msg,
        (uint16_t)(sizeof(boot_msg) - 1U)
    );

    /* Loop principal */

    while (1)
    {

        usb_buffer_process();
        send_pending_cn_mm();
        if (adc_queue_tail != adc_queue_head)
        {
            uint8_t tail = adc_queue_tail;
            ADCFrame *src = &adc_frame_queue[tail];

            uint16_t frame_local[NUM_SENSORS][NUM_TAXELS];

            uint32_t timestamp = src->timestamp;

            for (uint8_t sensor = 0U;
                 sensor < NUM_SENSORS;
                 sensor++)
            {
                for (uint8_t i = 0U;
                     i < NUM_TAXELS;
                     i++)
                {
                    frame_local[sensor][i] =
                        src->adc[sensor][i];
                }
            }

            /*
             * Libera a posicao somente depois de copiar o quadro.
             */
            __DMB();

            adc_queue_tail =
                (uint8_t)((tail + 1U) % ADC_FRAME_QUEUE_SIZE);

            /*
             * Formata e coloca DATA + MEDIA_S2 na fila USB.
             */
            (void)send_adc_frame_both(frame_local, timestamp);
        }

        /*
         * Tenta enviar mais dados pela USB.
         */
        usb_buffer_process();
    }
}


/* ============================================================
 * GPIO
 * ============================================================ */

void MX_GPIO_Init(void)
{
    GPIO_InitTypeDef g = {0};

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_GPIOD_CLK_ENABLE();
    __HAL_RCC_GPIOF_CLK_ENABLE();

    /* Linhas compartilhadas */

    g.Mode = GPIO_MODE_OUTPUT_PP;
    g.Pull = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_LOW;

    g.Pin = GPIO_PIN_0 | GPIO_PIN_1;

    HAL_GPIO_Init(GPIOD, &g);

    g.Pin = GPIO_PIN_3 | GPIO_PIN_5 | GPIO_PIN_10;

    HAL_GPIO_Init(GPIOF, &g);

    /* Sensor 1: PA0, PA3, PA4 e PA6 */

    g.Mode = GPIO_MODE_ANALOG;
    g.Pull = GPIO_NOPULL;

    g.Pin =
        GPIO_PIN_0 |
        GPIO_PIN_3 |
        GPIO_PIN_4 |
        GPIO_PIN_6;

    HAL_GPIO_Init(GPIOA, &g);

    /* Sensor 1: PB1 */

    g.Pin = GPIO_PIN_1;

    HAL_GPIO_Init(GPIOB, &g);

    /* Sensor 2: PA5 */

    g.Pin = GPIO_PIN_5;

    HAL_GPIO_Init(GPIOA, &g);

    /* Mantido conforme o codigo enviado: PB0 */

    g.Pin = GPIO_PIN_0;

    HAL_GPIO_Init(GPIOB, &g);

    /* Sensor 2: PC0, PC2 e PC3 */

    g.Pin =
        GPIO_PIN_0 |
        GPIO_PIN_2 |
        GPIO_PIN_3;

    HAL_GPIO_Init(GPIOC, &g);

    /* Pinos de debug */

    g.Mode = GPIO_MODE_OUTPUT_PP;
    g.Pull = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_VERY_HIGH;

    g.Pin = DEBUG_ADC_PIN | DEBUG_IZH_PIN;

    HAL_GPIO_Init(GPIOB, &g);

    HAL_GPIO_WritePin(
        DEBUG_ADC_PORT,
        DEBUG_ADC_PIN,
        GPIO_PIN_RESET
    );

    HAL_GPIO_WritePin(
        DEBUG_IZH_PORT,
        DEBUG_IZH_PIN,
        GPIO_PIN_RESET
    );
}


/* ============================================================
 * DMA
 * ============================================================ */

void MX_DMA_Init(void)
{
    __HAL_RCC_DMA2_CLK_ENABLE();

    hdma_adc1.Instance = DMA2_Stream0;

    hdma_adc1.Init.Channel = DMA_CHANNEL_0;

    hdma_adc1.Init.Direction =
        DMA_PERIPH_TO_MEMORY;

    hdma_adc1.Init.PeriphInc =
        DMA_PINC_DISABLE;

    hdma_adc1.Init.MemInc =
        DMA_MINC_ENABLE;

    hdma_adc1.Init.PeriphDataAlignment =
        DMA_PDATAALIGN_HALFWORD;

    hdma_adc1.Init.MemDataAlignment =
        DMA_MDATAALIGN_HALFWORD;

    hdma_adc1.Init.Mode =
        DMA_CIRCULAR;

    hdma_adc1.Init.Priority =
        DMA_PRIORITY_HIGH;

    hdma_adc1.Init.FIFOMode =
        DMA_FIFOMODE_DISABLE;

    if (HAL_DMA_Init(&hdma_adc1) != HAL_OK)
    {
        Error_Handler();
    }

    __HAL_LINKDMA(
        &hadc1,
        DMA_Handle,
        hdma_adc1
    );

    HAL_NVIC_SetPriority(
        DMA2_Stream0_IRQn,
        0,
        0
    );

    HAL_NVIC_EnableIRQ(DMA2_Stream0_IRQn);
}


/* ============================================================
 * INTERRUPCAO DMA
 * ============================================================ */

void DMA2_Stream0_IRQHandler(void)
{
    HAL_DMA_IRQHandler(&hdma_adc1);
}


/* ============================================================
 * ADC1
 *
 * Mantida a sequencia do codigo enviado.
 * ============================================================ */

void MX_ADC1_Init(void)
{
    __HAL_RCC_ADC1_CLK_ENABLE();

    ADC_ChannelConfTypeDef c = {0};

    hadc1.Instance = ADC1;

    hadc1.Init.Resolution =
        ADC_RESOLUTION_12B;

    hadc1.Init.ScanConvMode =
        ENABLE;

    hadc1.Init.ContinuousConvMode =
        DISABLE;

    hadc1.Init.NbrOfConversion =
        ADC_CHANNELS;

    hadc1.Init.ExternalTrigConv =
        ADC_EXTERNALTRIGCONV_T6_TRGO;

    hadc1.Init.ExternalTrigConvEdge =
        ADC_EXTERNALTRIGCONVEDGE_RISING;

    hadc1.Init.DMAContinuousRequests =
        ENABLE;

    if (HAL_ADC_Init(&hadc1) != HAL_OK)
    {
        Error_Handler();
    }

    uint32_t ch[ADC_CHANNELS] =
    {
        /* Sensor 1 */

        ADC_CHANNEL_0,   /* PA0 */
        ADC_CHANNEL_3,   /* PA3 */
        ADC_CHANNEL_4,   /* PA4 */
        ADC_CHANNEL_6,   /* PA6 */
        ADC_CHANNEL_9,   /* PB1 */

        /* Sensor 2: mantido conforme enviado */

        ADC_CHANNEL_5,   /* PA5 */
        ADC_CHANNEL_8,   /* PB0 */
        ADC_CHANNEL_10,  /* PC0 */
        ADC_CHANNEL_13,  /* PC3 */
        ADC_CHANNEL_12   /* PC2 */
    };

    for (uint32_t i = 0U; i < ADC_CHANNELS; i++)
    {
        c.Channel = ch[i];

        c.Rank = i + 1U;

        c.SamplingTime =
            ADC_SAMPLETIME_15CYCLES;

        if (HAL_ADC_ConfigChannel(&hadc1, &c) != HAL_OK)
        {
            Error_Handler();
        }
    }
}


/* ============================================================
 * TIM6 - TRIGGER ADC
 * ============================================================ */

void MX_TIM6_Init(void)
{
    __HAL_RCC_TIM6_CLK_ENABLE();

    htim6.Instance = TIM6;

    htim6.Init.Prescaler = 83;

    htim6.Init.Period = 199;

    if (HAL_TIM_Base_Init(&htim6) != HAL_OK)
    {
        Error_Handler();
    }

    TIM_MasterConfigTypeDef s = {0};

    s.MasterOutputTrigger =
        TIM_TRGO_UPDATE;

    s.MasterSlaveMode =
        TIM_MASTERSLAVEMODE_DISABLE;

    if (
        HAL_TIMEx_MasterConfigSynchronization(
            &htim6,
            &s
        ) != HAL_OK
    )
    {
        Error_Handler();
    }
}


/* ============================================================
 * TIM2 - TIMESTAMP
 * ============================================================ */

void MX_TIM2_Init(void)
{
    __HAL_RCC_TIM2_CLK_ENABLE();

    htim2.Instance = TIM2;

    htim2.Init.Prescaler = 83;

    htim2.Init.Period = 0xFFFFFFFFU;

    if (HAL_TIM_Base_Init(&htim2) != HAL_OK)
    {
        Error_Handler();
    }
}


/* ============================================================
 * CLOCK
 * ============================================================ */

void SystemClock_Config(void)
{
    RCC_OscInitTypeDef o = {0};

    RCC_ClkInitTypeDef c = {0};

    o.OscillatorType =
        RCC_OSCILLATORTYPE_HSE;

    o.HSEState =
        RCC_HSE_BYPASS;

    o.PLL.PLLState =
        RCC_PLL_ON;

    o.PLL.PLLSource =
        RCC_PLLSOURCE_HSE;

    o.PLL.PLLM = 8;

    o.PLL.PLLN = 336;

    o.PLL.PLLP = RCC_PLLP_DIV2;

    o.PLL.PLLQ = 7;

    if (HAL_RCC_OscConfig(&o) != HAL_OK)
    {
        Error_Handler();
    }

    c.ClockType =
        RCC_CLOCKTYPE_SYSCLK |
        RCC_CLOCKTYPE_HCLK |
        RCC_CLOCKTYPE_PCLK1 |
        RCC_CLOCKTYPE_PCLK2;

    c.SYSCLKSource =
        RCC_SYSCLKSOURCE_PLLCLK;

    c.AHBCLKDivider =
        RCC_SYSCLK_DIV1;

    c.APB1CLKDivider =
        RCC_HCLK_DIV4;

    c.APB2CLKDivider =
        RCC_HCLK_DIV2;

    if (
        HAL_RCC_ClockConfig(
            &c,
            FLASH_LATENCY_5
        ) != HAL_OK
    )
    {
        Error_Handler();
    }
}

void Error_Handler(void)
{
    __disable_irq();

    while (1)
    {
    }
}
