import serial
import time
import matplotlib.pyplot as plt
import re

# ================= UART =================
PORTA = "COM7"
BAUD = 115200
ser = serial.Serial(PORTA, BAUD, timeout=0.05)
time.sleep(2)

# ================= PARAMETROS =================
ROWS, COLS = 4, 4
NUM_TAXELS = ROWS * COLS
IDX_VALIDACAO = 10  
# ================= BUFFERS =================
time_data = []
current_vals = []

start_time = time.time()

# ================= FIGURA =================
fig, ax = plt.subplots(figsize=(10,4))
line_current, = ax.plot([], [], color='b', label=f"Taxel {IDX_VALIDACAO}")
ax.set_xlabel("Tempo (s)")
ax.set_ylabel("Corrente (I)")
ax.set_title(f"Corrente do Taxel {IDX_VALIDACAO} em tempo real")
ax.legend()
ax.set_xlim(0, 60)  # exibe 60 segundos
ax.set_ylim(0, 10)  
# ================= UPDATE =================
def update(frame):
    now = time.time()
    
    while ser.in_waiting:
        linha = ser.readline().decode(errors="ignore").strip()
        if linha.startswith("DBG,row="):
            m = re.search(r"DBG,row=(\d+),adc0=(\d+),I=([0-9.]+)", linha)
            if m:
                row = int(m.group(1))
                I = float(m.group(3))
                ch = row * COLS  # coluna 0

                if ch == IDX_VALIDACAO:
                    t_s = now - start_time
                    time_data.append(t_s)
                    current_vals.append(I)
                    
                    # atualizar linha do gráfico
                    line_current.set_data(time_data, current_vals)
                    
                    # eixo x dinâmico (exibe últimos 60 s)
                    ax.set_xlim(max(0, t_s-60), t_s)
                    ax.set_ylim(0, max(current_vals)*1.2)
                    
    return [line_current]

# ================= ANIMAÇÃO =================
from matplotlib.animation import FuncAnimation
ani = FuncAnimation(fig, update, interval=50, blit=True)
plt.show()
