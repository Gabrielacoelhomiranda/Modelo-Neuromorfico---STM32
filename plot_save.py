import pandas as pd
import matplotlib.pyplot as plt

# ================= CONFIGURAÇÕES =================
# Caminho do arquivo CSV gerado pelo STM32
csv_file = "C:\\Users\\Gabi\\Documents\\dados.csv"

# Número de linhas e colunas da matriz de taxels
ROWS = 4
COLS = 4

# ================= LEITURA DO CSV =================
try:
    data = pd.read_csv(csv_file)
except FileNotFoundError:
    print(f"Arquivo {csv_file} não encontrado.")
    exit()

# Conferir as primeiras linhas
print(data.head())

# ================= PLOTAGEM =================
fig, axes = plt.subplots(ROWS, COLS, figsize=(20, 12), sharex=True, sharey=True)
fig.suptitle('Corrente e Spikes dos Taxels', fontsize=16)

for row in range(ROWS):
    for col in range(COLS):
        ax = axes[row, col]
        taxel_data = data[(data['row'] == row) & (data['col'] == col)]
        if taxel_data.empty:
            continue

        # Plot da corrente
        ax.plot(taxel_data['timestamp'], taxel_data['I'], label='Corrente (I)')

        # Plot dos spikes em vermelho
        spikes = taxel_data[taxel_data['spike'] == 1]
        ax.scatter(spikes['timestamp'], spikes['I'], color='red', label='Spike', s=20)

        ax.set_title(f'Taxel ({row},{col})')
        ax.grid(True)
        if row == ROWS - 1:
            ax.set_xlabel('Timestamp (us)')
        if col == 0:
            ax.set_ylabel('Corrente (I)')

plt.tight_layout(rect=[0, 0, 1, 0.95])
plt.legend(loc='upper right')
plt.show()
