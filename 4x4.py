
import serial
import numpy as np
import matplotlib.pyplot as plt
from matplotlib.animation import FuncAnimation
import re

# ================= CONFIG ===================

PORT = 'COM9'
BAUD = 115200

ROWS, COLS = 4, 4
NUM_TAXELS = ROWS * COLS

VREF = 3.3
ADC_MAX = 4095.0
RASTER_WINDOW = 5.0

ser = serial.Serial(PORT, BAUD, timeout=0.1)

# Buffer para evitar perder linhas USB incompletas
serial_buffer = ""

# ================= MATRIZES =================

spike_times_RA = [[] for _ in range(NUM_TAXELS)]
spike_times_SA = [[] for _ in range(NUM_TAXELS)]
spike_times_CN = []

voltage_matrix = np.zeros((ROWS, COLS))
current_time = 0.0

# ================= FIGURA ===================

fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(14, 7))

# ===== HEATMAP DE TENSÃO =====

im_volt = ax1.imshow(
    voltage_matrix,
    cmap="jet",
    interpolation="bicubic",
    vmin=0,
    vmax=VREF
)

cbar_volt = plt.colorbar(im_volt, ax=ax1)
cbar_volt.set_label("Tensão (V)")

ax1.set_title("Matriz tátil 4×4 - Tensão")
ax1.set_xlabel("Coluna")
ax1.set_ylabel("Linha")
ax1.set_xticks(range(COLS))
ax1.set_yticks(range(ROWS))

texts_volt = [
    [
        ax1.text(
            c, r, "0.00",
            ha="center",
            va="center",
            fontsize=9,
            color="white"
        )
        for c in range(COLS)
    ]
    for r in range(ROWS)
]

# ===== RASTER PLOT =====
# RA: neurônios 0 a 15
# SA: neurônios 16 a 31
# CN: neurônio único na posição 32

CN_Y = NUM_TAXELS * 2

ax2.set_title("Raster Plot - RA, SA e CN")
ax2.set_xlabel("Tempo (s)")
ax2.set_ylabel("População neuronal")

ax2.set_ylim(-1, CN_Y + 1)
ax2.set_xlim(0, RASTER_WINDOW)

# Identificação visual dos três grupos
ax2.set_yticks([
    (NUM_TAXELS - 1) / 2,
    NUM_TAXELS + (NUM_TAXELS - 1) / 2,
    CN_Y
])

ax2.set_yticklabels([
    "RA (16 neurônios)",
    "SA (16 neurônios)",
    "CN (1 neurônio)"
])

ax2.axhline(
    NUM_TAXELS - 0.5,
    color="gray",
    linestyle="--",
    linewidth=0.8
)

ax2.axhline(
    NUM_TAXELS * 2 - 0.5,
    color="gray",
    linestyle="--",
    linewidth=0.8
)

scatter_RA = ax2.scatter(
    [], [], marker=".", s=18,
    color="red", label="RA - Adaptação rápida"
)

scatter_SA = ax2.scatter(
    [], [], marker=".", s=18,
    color="blue", label="SA - Adaptação lenta"
)

scatter_CN = ax2.scatter(
    [], [], marker="|", s=140,
    color="green", linewidths=2,
    label="CN - Neurônio cuneato"
)

ax2.legend(loc="upper left", fontsize=8)
ax2.grid(axis="x", alpha=0.25)

# ================= UPDATE ===================

def update(frame):

    global voltage_matrix
    global current_time
    global serial_buffer

    # ===== LEITURA SERIAL =====

    if ser.in_waiting:
        received = ser.read(ser.in_waiting).decode(
            "utf-8", errors="ignore"
        )

        serial_buffer += received

        # Processa somente linhas completas
        lines = serial_buffer.split("\n")
        serial_buffer = lines.pop()

        for line in lines:

            line = line.strip()

            if not line:
                continue

            # -------- ADC / TENSÃO --------
            # Formato:
            # DATA,idx=0,adc=1234,t=100000

            if line.startswith("DATA"):

                match = re.search(
                    r"idx=(\d+),adc=(\d+),t=(\d+)",
                    line
                )

                if match:
                    idx = int(match.group(1))
                    adc = int(match.group(2))
                    tstamp = int(match.group(3)) / 1e6

                    current_time = max(current_time, tstamp)

                    if 0 <= idx < NUM_TAXELS:
                        row, col = divmod(idx, COLS)

                        # Mantém a conversão do seu código original
                        voltage_matrix[row, col] = (
                            adc * VREF / ADC_MAX
                        )

            # -------- RA --------
            # Formato:
            # RA,idx=0,adc=1234,t=100000

            elif line.startswith("RA,"):

                match = re.search(
                    r"idx=(\d+),adc=\d+,t=(\d+)",
                    line
                )

                if match:
                    idx = int(match.group(1))
                    tstamp = int(match.group(2)) / 1e6

                    current_time = max(current_time, tstamp)

                    if 0 <= idx < NUM_TAXELS:
                        spike_times_RA[idx].append(tstamp)

            # -------- SA --------
            # Formato:
            # SA,idx=0,adc=1234,t=100000

            elif line.startswith("SA,"):

                match = re.search(
                    r"idx=(\d+),adc=\d+,t=(\d+)",
                    line
                )

                if match:
                    idx = int(match.group(1))
                    tstamp = int(match.group(2)) / 1e6

                    current_time = max(current_time, tstamp)

                    if 0 <= idx < NUM_TAXELS:
                        spike_times_SA[idx].append(tstamp)

            # -------- CN --------
            # Formato do seu firmware:
            # POST,t=100000,Ifinal=0.123456

            elif line.startswith("POST,"):

                match = re.search(
                    r"t=(\d+),Ifinal=([-+0-9.eE]+)",
                    line
                )

                if match:
                    tstamp = int(match.group(1)) / 1e6

                    current_time = max(current_time, tstamp)

                    # Um único neurônio cuneato
                    spike_times_CN.append(tstamp)

    # ===== ATUALIZA HEATMAP =====

    im_volt.set_data(voltage_matrix)
    im_volt.set_clim(0, VREF)

    for r in range(ROWS):
        for c in range(COLS):
            texts_volt[r][c].set_text(
                f"{voltage_matrix[r, c]:.2f}"
            )

    # ===== RASTER RA =====

    raster_x_RA = []
    raster_y_RA = []

    for neuron in range(NUM_TAXELS):

        spike_times_RA[neuron] = [
            t for t in spike_times_RA[neuron]
            if 0 <= current_time - t <= RASTER_WINDOW
        ]

        raster_x_RA.extend(spike_times_RA[neuron])
        raster_y_RA.extend(
            [neuron] * len(spike_times_RA[neuron])
        )

    # ===== RASTER SA =====

    raster_x_SA = []
    raster_y_SA = []

    for neuron in range(NUM_TAXELS):

        spike_times_SA[neuron] = [
            t for t in spike_times_SA[neuron]
            if 0 <= current_time - t <= RASTER_WINDOW
        ]

        raster_x_SA.extend(spike_times_SA[neuron])
        raster_y_SA.extend(
            [neuron + NUM_TAXELS]
            * len(spike_times_SA[neuron])
        )

    # ===== RASTER CN =====

    spike_times_CN[:] = [
        t for t in spike_times_CN
        if 0 <= current_time - t <= RASTER_WINDOW
    ]

    raster_x_CN = spike_times_CN
    raster_y_CN = [CN_Y] * len(spike_times_CN)

    # ===== ATUALIZA OS PONTOS =====

    scatter_RA.set_offsets(
        np.column_stack((raster_x_RA, raster_y_RA))
        if raster_x_RA else np.empty((0, 2))
    )

    scatter_SA.set_offsets(
        np.column_stack((raster_x_SA, raster_y_SA))
        if raster_x_SA else np.empty((0, 2))
    )

    scatter_CN.set_offsets(
        np.column_stack((raster_x_CN, raster_y_CN))
        if raster_x_CN else np.empty((0, 2))
    )

    # Janela temporal móvel
    right = max(RASTER_WINDOW, current_time)
    ax2.set_xlim(
        max(0, right - RASTER_WINDOW),
        right
    )

    return [
        im_volt,
        scatter_RA,
        scatter_SA,
        scatter_CN,
        *[txt for row in texts_volt for txt in row]
    ]


# ================= ANIMAÇÃO =================

ani = FuncAnimation(
    fig,
    update,
    interval=50,
    blit=False,
    cache_frame_data=False
)

plt.tight_layout()
plt.show()

ser.close()
