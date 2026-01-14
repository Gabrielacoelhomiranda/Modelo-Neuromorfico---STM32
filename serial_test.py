import serial          # Importa a biblioteca para comunicação serial (UART)
import numpy as np     # Importa a biblioteca NumPy, essencial para operações com arrays e matrizes
import matplotlib.pyplot as plt # Importa o módulo de plotagem Pyplot do Matplotlib
from matplotlib.animation import FuncAnimation # Importa FuncAnimation para criar animações de plotagem em tempo real
import time            # Importa a biblioteca time para medição de tempo
import re              # Importa a biblioteca re para expressões regulares (usadas na análise das strings UART)

# ================= CONFIGURAÇÕES UART =================
# O script original está faltando a variável 'baud_rate'. Assumindo 115200 como no código C.
baud_rate = 115200
uart_port = "COM3"     # Define a porta serial do seu sistema (ex: COM4 no Windows, /dev/ttyUSB0 no Linux)

try:
    # Tenta abrir a porta serial com a taxa de transmissão e timeout definidos
    ser = serial.Serial(uart_port, baud_rate, timeout=0.1)
except Exception as e:
    # Se falhar, imprime a mensagem de erro e sai do script
    print("Não foi possível abrir a porta:", e)
    exit()

ROWS, COLS = 4, 4      # Define o número de linhas e colunas da matriz de taxéis (4x4)
NUM_TAXELS = ROWS * COLS # Calcula o número total de taxéis (16)
WINDOW = 1.0           # Define a janela de tempo em segundos para calcular a taxa de spikes
G = 10.0               # Define o ganho da corrente do neurônio (para configurar o limite do plot de corrente)

# ================= BUFFERS =================
# Lista de listas para armazenar os timestamps de spikes para cada taxel
spike_times = [[] for _ in range(NUM_TAXELS)]
# Lista para armazenar dados de spikes para o raster plot: [(tempo, linha, coluna)]
scatter_data = []
# Matriz NumPy 4x4, inicializada com zeros, para armazenar os valores de tensão atuais
voltage_matrix = np.zeros((ROWS, COLS))
# Matriz NumPy 4x4, inicializada com zeros, para armazenar os valores de corrente atuais
current_matrix = np.zeros((ROWS, COLS))
HISTORY_LEN = 20       # Define o comprimento do histórico de tensão para cálculo da média móvel
# Lista de listas de listas para armazenar o histórico de tensão de cada taxel
voltage_history = [[[] for _ in range(COLS)] for _ in range(ROWS)]

start_time = time.time() # Registra o tempo inicial para calcular tempos relativos nos plots
# Lista de listas para armazenar o histórico de corrente para o plot de séries temporais
current_data = [[] for _ in range(NUM_TAXELS)]
time_data = []         # Lista para armazenar os timestamps relativos correspondentes aos dados de corrente

# ================= CRIAÇÃO DOS PLOTS =================
fig = plt.figure(figsize=(20, 12)) # Cria a figura principal do Matplotlib com tamanho definido
# Cria uma grade de subplots 3x2, ajustando as proporções de altura para o plot de série temporal
gs = fig.add_gridspec(3, 2, height_ratios=[1, 1, 1.2], wspace=0.35, hspace=0.4)

# --- Heatmap de spikes ---
ax1 = fig.add_subplot(gs[0, 0]) # Adiciona o primeiro subplot na posição (0, 0)
spike_matrix = np.zeros((ROWS, COLS)) # Matriz inicial de spikes (taxa de disparos)
# Cria o objeto de imagem (heatmap) para spikes
im_spike = ax1.imshow(spike_matrix, cmap="jet", interpolation="bicubic")
# Adiciona a barra de cores
cbar_spike = plt.colorbar(im_spike, ax=ax1)
cbar_spike.set_label("Firing rate (spikes/s)") # Rótulo da barra de cores
ax1.set_title(f"Taxa de Spikes (últimos {WINDOW:.1f}s)") # Título do plot
ax1.set_xticks(range(COLS))    # Define rótulos do eixo X (colunas)
ax1.set_yticks(range(ROWS))    # Define rótulos do eixo Y (linhas)
# Cria objetos de texto para exibir a taxa numérica de spikes em cada célula
texts_spike = [[ax1.text(c, r, "0", ha="center", va="center", fontsize=8)
                for c in range(COLS)] for r in range(ROWS)]

# --- Heatmap de tensão ---
ax2 = fig.add_subplot(gs[0, 1]) # Adiciona o segundo subplot na posição (0, 1)
# Cria o objeto de imagem (heatmap) para tensão
im_volt = ax2.imshow(voltage_matrix, cmap="jet", interpolation="bicubic")
# Adiciona a barra de cores
cbar_volt = plt.colorbar(im_volt, ax=ax2)
cbar_volt.set_label("Tensão (V)") # Rótulo da barra de cores
ax2.set_title("Tensão (0–3.3 V)") # Título do plot
ax2.set_xticks(range(COLS))    # Define rótulos do eixo X
ax2.set_yticks(range(ROWS))    # Define rótulos do eixo Y
# Cria objetos de texto para exibir o valor numérico de tensão em cada célula
texts_volt = [[ax2.text(c, r, "0", ha="center", va="center", fontsize=8)
               for c in range(COLS)] for r in range(ROWS)]

# --- Heatmap de sensibilidade ---
ax3 = fig.add_subplot(gs[1, 0]) # Adiciona o terceiro subplot na posição (1, 0)
sensitivity_matrix = np.zeros((ROWS, COLS)) # Matriz inicial de sensibilidade
# Cria o objeto de imagem (heatmap) para sensibilidade
im_sens = ax3.imshow(sensitivity_matrix, cmap="jet", interpolation="bicubic")
# Adiciona a barra de cores
cbar_sens = plt.colorbar(im_sens, ax=ax3)
cbar_sens.set_label("Sensibilidade (spikes / tensão média)") # Rótulo da barra de cores
ax3.set_title(f"Sensibilidade (últimos {WINDOW:.1f}s)") # Título do plot
ax3.set_xticks(range(COLS))    # Define rótulos do eixo X
ax3.set_yticks(range(ROWS))    # Define rótulos do eixo Y
# Cria objetos de texto para exibir o valor numérico de sensibilidade em cada célula
texts_sens = [[ax3.text(c, r, "0", ha="center", va="center", fontsize=8)
               for c in range(COLS)] for r in range(ROWS)]

# --- Raster plot ---
ax4 = fig.add_subplot(gs[1, 1]) # Adiciona o quarto subplot na posição (1, 1)
ax4.set_title("Raster plot dos spikes (taxel 4x4)") # Título do plot
ax4.set_xlabel("Tempo (s, relativo)") # Rótulo do eixo X
ax4.set_ylabel("Taxel (row,col)")   # Rótulo do eixo Y
# Cria o objeto scatter (dispersão) que será atualizado no raster plot
scatter_plot = ax4.scatter([], [], s=15, marker="|")
ax4.set_yticks(range(NUM_TAXELS)) # Define as posições dos rótulos Y para cada taxel (0 a 15)
# Define os rótulos do eixo Y como (linha, coluna)
ax4.set_yticklabels([f"({r},{c})" for r in range(ROWS) for c in range(COLS)])

# --- Corrente (todas as linhas) ---
ax5 = fig.add_subplot(gs[2, :]) # Adiciona o quinto subplot, abrangendo as duas colunas inferiores
ax5.set_title("Corrente por taxel (todas as linhas)") # Título do plot
ax5.set_xlabel("Tempo (s)")     # Rótulo do eixo X
ax5.set_ylabel("Corrente (uA)") # Rótulo do eixo Y

lines_current = []              # Lista para armazenar os objetos de linha de plotagem
for r in range(ROWS):
    for c in range(COLS):
        # Cria um objeto de linha vazio para cada taxel e adiciona à lista
        line, = ax5.plot([], [], label=f"({r},{c})")
        lines_current.append(line)

ax5.set_ylim(0, G*1.1)          # Define o limite Y do plot de corrente (G é o ganho máximo)
# Adiciona a legenda
ax5.legend(loc='upper right', fontsize=7, ncol=4)

# ================= FUNÇÃO DE ATUALIZAÇÃO =================
def update(frame):
    # Declara variáveis globais que serão modificadas dentro da função
    global spike_matrix, scatter_data, voltage_matrix, current_matrix, time_data

    now = time.time() # Captura o tempo atual

    # Leitura UART
    try:
        # Loop enquanto houver bytes esperando na porta serial
        while ser.in_waiting:
            # Lê uma linha da serial, decodifica para string e remove espaços em branco
            line = ser.readline().decode("utf-8", errors="ignore").strip()
            
            # --- Processamento de Spikes ---
            if line.startswith("Spike: ch="): # Verifica se a linha é um evento de spike
                # Procura pelo número do canal (\d+) na string
                match = re.search(r"Spike: ch=(\d+)", line)
                if match:
                    ch = int(match.group(1)) # Extrai o número do canal
                    if 0 <= ch < NUM_TAXELS: # Verifica se o canal é válido
                        row, col = divmod(ch, COLS) # Calcula linha e coluna a partir do canal (divmod(15, 4) -> 3, 3)
                        spike_times[ch].append(now) # Adiciona o timestamp atual à lista de spikes do canal
                        # Adiciona o evento para o raster plot: (tempo relativo, linha, coluna)
                        scatter_data.append((now-start_time, row, col))
            
            # --- Processamento de Tensão e Corrente ---
            elif line.startswith("Row"): # Verifica se a linha contém dados de tensão/corrente
                # Procura pelos 4 números: Row, col, V, I
                match = re.search(r"Row (\d+), col (\d+) = ([0-9.]+) V, I=([0-9.]+)", line)
                if match:
                    row = int(match.group(1))   # Extrai o número da linha
                    col = int(match.group(2))   # Extrai o número da coluna
                    val = float(match.group(3)) # Extrai o valor de tensão (V)
                    I = float(match.group(4))   # Extrai o valor de corrente (I)
                    voltage_matrix[row, col] = val # Atualiza a matriz de tensão
                    current_matrix[row, col] = I # Atualiza a matriz de corrente

                    # --- Histórico de Tensão ---
                    hist = voltage_history[row][col] # Pega a lista de histórico para este taxel
                    hist.append(val)                 # Adiciona o novo valor
                    if len(hist) > HISTORY_LEN:      # Se o histórico exceder o limite
                        hist.pop(0)                  # Remove o valor mais antigo (cálculo de média móvel)

    except Exception as e:
        print("Erro:", e) # Imprime erros de leitura ou processamento da serial

    # --- Heatmap spikes ---
    for ch in range(NUM_TAXELS): # Loop por todos os taxéis
        # Filtra os timestamps, mantendo apenas os que ocorreram na WINDOW
        spike_times[ch] = [t for t in spike_times[ch] if now - t <= WINDOW]
        row, col = divmod(ch, COLS) # Calcula linha e coluna
        # Calcula a taxa de disparo (spikes/s)
        spike_matrix[row, col] = len(spike_times[ch]) / WINDOW
        # Atualiza o texto exibido no heatmap
        texts_spike[row][col].set_text(f"{spike_matrix[row,col]:.1f}")
    im_spike.set_data(spike_matrix) # Atualiza os dados do heatmap
    # Define os limites de cor, garantindo um mínimo de 1.0 para v_max (se não houver spikes)
    im_spike.set_clim(vmin=0, vmax=max(np.max(spike_matrix), 1.0))

    # --- Raster ---
    # Filtra os dados do raster, mantendo apenas os que ocorreram nos últimos 5.0 segundos
    scatter_data[:] = [(t,r,c) for (t,r,c) in scatter_data if now-start_time - t <= 5.0]
    if scatter_data: # Se houver dados de spike
        times, rows, cols = zip(*scatter_data) # Desempacota os dados
        # Cria a lista de coordenadas Y (índice do taxel: 0 a 15)
        y_coords = [r*COLS+c for r,c in zip(rows,cols)]
        # Atualiza a posição dos pontos de dispersão (x=tempo, y=taxel)
        scatter_plot.set_offsets(np.column_stack((times, y_coords)))
    # Define o limite X do raster plot (janela deslizante de 5 segundos)
    ax4.set_xlim(max(0, now-start_time-5), now-start_time)
    # Define o limite Y do raster plot
    ax4.set_ylim(-0.5, NUM_TAXELS-0.5)

    # --- Heatmap tensão ---
    im_volt.set_data(voltage_matrix) # Atualiza os dados de tensão do heatmap
    im_volt.set_clim(0, 3.3)         # Define os limites de cor (0V a 3.3V)
    for r in range(ROWS):            # Loop para atualizar o texto da tensão
        for c in range(COLS):
            texts_volt[r][c].set_text(f"{voltage_matrix[r,c]:.2f}")

    # --- Heatmap sensibilidade ---
    voltage_mean = np.zeros((ROWS, COLS)) # Matriz para a tensão média
    for r in range(ROWS):
        for c in range(COLS):
            # Calcula a média do histórico de tensão (ou 1.0 se o histórico for vazio, para evitar divisão por zero)
            voltage_mean[r,c] = np.mean(voltage_history[r][c]) if voltage_history[r][c] else 1.0
    # Calcula a sensibilidade como (Taxa de Spikes) / (Tensão Média)
    # Adiciona um pequeno valor (1e-3) no denominador para evitar divisão por zero estrito
    sensitivity_matrix = spike_matrix / (voltage_mean + 1e-3)
    im_sens.set_data(sensitivity_matrix) # Atualiza os dados de sensibilidade
    im_sens.set_clim(vmin=0, vmax=max(np.max(sensitivity_matrix),1)) # Define os limites de cor
    for r in range(ROWS):
        for c in range(COLS):
            texts_sens[r][c].set_text(f"{sensitivity_matrix[r,c]:.2f}") # Atualiza o texto

    # --- Corrente (todas as linhas) ---
    time_data.append(now-start_time) # Adiciona o timestamp relativo
    for r in range(ROWS):
        for c in range(COLS):
            idx = r * COLS + c # Índice do taxel (0 a 15)
            # Adiciona o valor de corrente na lista de histórico do taxel
            current_data[idx].append(current_matrix[r, c])
            # Atualiza os dados de plotagem da linha (tempo vs. corrente)
            lines_current[idx].set_data(time_data, current_data[idx])
    # Define o limite X do plot de corrente (janela deslizante de 5 segundos)
    ax5.set_xlim(max(0, now-start_time-5), now-start_time)

    # Retorna todos os objetos de plotagem que foram modificados (necessário para blit=True/False)
    return [im_spike, im_volt, im_sens, scatter_plot] + lines_current

# ================= ANIMAÇÃO =================
# Cria o objeto de animação
# fig: figura a ser animada
# update: função de atualização a ser chamada em cada frame
# interval=50: chama update a cada 50 ms (20 frames por segundo)
# blit=False: define como a tela será redesenhada (False é mais robusto para múltiplos subplots)
ani = FuncAnimation(fig, update, interval=50, blit=False)
plt.show() # Exibe a janela de plotagem e inicia a animação
