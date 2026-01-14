import serial
import time
import numpy as np
import matplotlib.pyplot as plt
from matplotlib import colors

# ================= CONFIGURAÇÃO DA PORTA SERIAL =================
ser = serial.Serial(
    port='COM4',      # Ajuste para a porta correta
    baudrate=115200,
    timeout=0.1
)
time.sleep(2)  # Aguarda inicialização da UART

ROWS, COLS = 4, 4
matrix = np.zeros((ROWS, COLS))  # Matriz de spikes

# ================= CONFIGURAÇÃO DA FIGURA =======================
fig, ax = plt.subplots()
cmap = colors.ListedColormap(['white', 'red'])
bounds = [0, 0.5, 1]
norm = colors.BoundaryNorm(bounds, cmap.N)
im = ax.imshow(matrix, cmap=cmap, norm=norm)

ax.set_title("Spikes Matriz 4x4 (Vermelho = Spike)")
plt.ion()
plt.show()

# ================= LOOP PRINCIPAL ==============================
try:
    while True:
        if ser.in_waiting > 0:
            line = ser.readline().decode('utf-8', errors='ignore').strip()
            if line.startswith("Spike:"):
                # Parse do channel
                parts = line.split()
                ch_part = parts[1]  # ch=5
                ch = int(ch_part.split('=')[1])
                row = ch // COLS
                col = ch % COLS

                # Marca spike na matriz
                matrix[row, col] = 1

                # Atualiza plot
                im.set_data(matrix)
                plt.draw()
                plt.pause(0.001)

                # Reseta matriz após 50ms (para efeito de blink)
                time.sleep(0.05)
                matrix[row, col] = 0
except KeyboardInterrupt:
    print("Encerrando leitura UART...")
finally:
    ser.close()

