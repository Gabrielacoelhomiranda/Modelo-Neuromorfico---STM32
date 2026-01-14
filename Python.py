import numpy as np
import matplotlib.pyplot as plt
from matplotlib.animation import FuncAnimation
import time
import csv

# ================= CONFIGURAÇÃO =================
ROWS, COLS = 4, 4
NUM_TAXELS = ROWS * COLS
WINDOW = 1.0       # janela de tempo para cálculo da taxa de spikes
G = 10.0           # ganho máximo da corrente
HISTORY_LEN = 20   # histórico de tensão para média móvel

# ================= LEITURA CSV =================
csv_file = r"C:\Users\Gabi\OneDrive\Área de Trabalho\dados_5.csv"
with open(csv_file, newline='') as f:
    reader = csv.DictReader(f)
    csv_lines = list(reader)  # lista de dicionários com timestamp, row, col, I, spike

# Converte timestamps para float em segundos
for line in csv_lines:
    line["timestamp"] = float(line["timestamp"]) / 1000.0
    line["row"] = int(line["row"])
    line["col"] = int(line["col"])
    line["I"] = float(line["I"])
    line["spike"] = int(line["spike"])

# ================= BUFFERS =================
spike_times = [[] for _ in range(NUM_TAXELS)]
scatter_data = []
voltage_matrix = np.zeros((ROWS, COLS))
current_matrix = np.zeros((ROWS, COLS))
voltage_history = [[[] for _ in range(COLS)] for _ in range(ROWS)]
current_data = [[] for _ in range(NUM_TAXELS)]
time_data = []

start_time = time.time()

# ================= CRIAÇÃO DOS PLOTS =================
fig = plt.figure(figsize=(20, 12))
gs = fig.add_gridspec(3, 2, height_ratios=[1, 1, 1.2], wspace=0.35, hspace=0.4)

# --- Heatmap de spikes ---
ax1 = fig.add_subplot(gs[0,0])
spike_matrix = np.zeros((ROWS, COLS))
im_spike = ax1.imshow(spike_matrix, cmap="jet", interpolation="bicubic")
cbar_spike = plt.colorbar(im_spike, ax=ax1)
cbar_spike.set_label("Firing rate (spikes/s)")
ax1.set_title(f"Taxa de Spikes (últimos {WINDOW:.1f}s)")
ax1.set_xticks(range(COLS))
ax1.set_yticks(range(ROWS))
texts_spike = [[ax1.text(c, r, "0", ha="center", va="center", fontsize=8)
                for c in range(COLS)] for r in range(ROWS)]

# --- Heatmap de tensão ---
ax2 = fig.add_subplot(gs[0,1])
im_volt = ax2.imshow(voltage_matrix, cmap="jet", interpolation="bicubic")
cbar_volt = plt.colorbar(im_volt, ax=ax2)
cbar_volt.set_label("Tensão (V)")
ax2.set_title("Tensão (0–3.3 V)")
ax2.set_xticks(range(COLS))
ax2.set_yticks(range(ROWS))
texts_volt = [[ax2.text(c, r, "0", ha="center", va="center", fontsize=8)
               for c in range(COLS)] for r in range(ROWS)]

# --- Heatmap de sensibilidade ---
ax3 = fig.add_subplot(gs[1,0])
sensitivity_matrix = np.zeros((ROWS, COLS))
im_sens = ax3.imshow(sensitivity_matrix, cmap="jet", interpolation="bicubic")
cbar_sens = plt.colorbar(im_sens, ax=ax3)
cbar_sens.set_label("Sensibilidade (spikes / tensão média)")
ax3.set_title(f"Sensibilidade (últimos {WINDOW:.1f}s)")
ax3.set_xticks(range(COLS))
ax3.set_yticks(range(ROWS))
texts_sens = [[ax3.text(c, r, "0", ha="center", va="center", fontsize=8)
               for c in range(COLS)] for r in range(ROWS)]

# --- Raster plot ---
ax4 = fig.add_subplot(gs[1,1])
ax4.set_title("Raster plot dos spikes (taxel 4x4)")
ax4.set_xlabel("Tempo (s, relativo)")
ax4.set_ylabel("Taxel (row,col)")
scatter_plot = ax4.scatter([], [], s=15, marker="|")
ax4.set_yticks(range(NUM_TAXELS))
ax4.set_yticklabels([f"({r},{c})" for r in range(ROWS) for c in range(COLS)])

# --- Corrente ---
ax5 = fig.add_subplot(gs[2,:])
ax5.set_title("Corrente por taxel (todas as linhas)")
ax5.set_xlabel("Tempo (s)")
ax5.set_ylabel("Corrente (uA)")
lines_current = []
for r in range(ROWS):
    for c in range(COLS):
        line, = ax5.plot([], [], label=f"({r},{c})")
        lines_current.append(line)
ax5.set_ylim(0, G*1.1)
ax5.legend(loc='upper right', fontsize=7, ncol=4)

# ================= FUNÇÃO DE ATUALIZAÇÃO =================
def update(frame):
    global spike_matrix, scatter_data, voltage_matrix, current_matrix, time_data

    now = time.time() - start_time

    if not hasattr(update, "index"):
        update.index = 0

    # Processa linhas do CSV cujo timestamp <= tempo atual
    while update.index < len(csv_lines) and csv_lines[update.index]["timestamp"] <= now:
        row = csv_lines[update.index]["row"]
        col = csv_lines[update.index]["col"]
        I = csv_lines[update.index]["I"]
        spike = csv_lines[update.index]["spike"]
        ts = csv_lines[update.index]["timestamp"]

        current_matrix[row, col] = I
        voltage_matrix[row, col] = voltage_matrix[row, col]  # se CSV tiver tensão, atualize aqui

        if spike == 1:
            ch = row*COLS + col
            spike_times[ch].append(now)
            scatter_data.append((now, row, col))

        update.index += 1

    # --- Heatmap spikes ---
    for ch in range(NUM_TAXELS):
        spike_times[ch] = [t for t in spike_times[ch] if now - t <= WINDOW]
        row, col = divmod(ch, COLS)
        spike_matrix[row, col] = len(spike_times[ch]) / WINDOW
        texts_spike[row][col].set_text(f"{spike_matrix[row,col]:.1f}")
    im_spike.set_data(spike_matrix)
    im_spike.set_clim(vmin=0, vmax=max(np.max(spike_matrix),1.0))

    # --- Raster plot ---
    scatter_data[:] = [(t,r,c) for (t,r,c) in scatter_data if now - t <= 5.0]
    if scatter_data:
        times, rows, cols = zip(*scatter_data)
        y_coords = [r*COLS+c for r,c in zip(rows,cols)]
        scatter_plot.set_offsets(np.column_stack((times, y_coords)))
    ax4.set_xlim(max(0, now-5), now)
    ax4.set_ylim(-0.5, NUM_TAXELS-0.5)

    # --- Heatmap tensão ---
    im_volt.set_data(voltage_matrix)
    im_volt.set_clim(0, 3.3)
    for r in range(ROWS):
        for c in range(COLS):
            texts_volt[r][c].set_text(f"{voltage_matrix[r,c]:.2f}")

    # --- Heatmap sensibilidade ---
    voltage_mean = np.zeros((ROWS,COLS))
    for r in range(ROWS):
        for c in range(COLS):
            voltage_mean[r,c] = np.mean(voltage_history[r][c]) if voltage_history[r][c] else 1.0
    sensitivity_matrix = spike_matrix / (voltage_mean+1e-3)
    im_sens.set_data(sensitivity_matrix)
    im_sens.set_clim(vmin=0, vmax=max(np.max(sensitivity_matrix),1))
    for r in range(ROWS):
        for c in range(COLS):
            texts_sens[r][c].set_text(f"{sensitivity_matrix[r,c]:.2f}")

    # --- Corrente ---
    time_data.append(now)
    for r in range(ROWS):
        for c in range(COLS):
            idx = r*COLS+c
            current_data[idx].append(current_matrix[r,c])
            lines_current[idx].set_data(time_data, current_data[idx])
    ax5.set_xlim(max(0, now-5), now)

    return [im_spike, im_volt, im_sens, scatter_plot] + lines_current

# ================= ANIMAÇÃO =================
ani = FuncAnimation(fig, update, interval=50, blit=False)
plt.show()
