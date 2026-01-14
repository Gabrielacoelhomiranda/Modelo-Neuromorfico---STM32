import pandas as pd
import numpy as np
import matplotlib.pyplot as plt

print(">>> Script iniciado")

# ====================== CONFIGURAÇÕES ======================
csv_file = r"C:\Users\Gabi\OneDrive\Área de Trabalho\dados_7canal_R0C0.csv"

ROWS, COLS = 4, 4
NUM_TAXELS = ROWS * COLS


# ====================== LEITURA DO CSV ======================
print(">>> Lendo CSV")
df = pd.read_csv(csv_file)

# Conversão de tipos
print(">>> Convertendo tipos")
df["timestamp_us"] = df["timestamp_us"].astype(np.int64)   # µs (TIM2)
df["row"] = df["row"].astype(int)
df["col"] = df["col"].astype(int)
df["I"] = df["I"].astype(float)
df["spike"] = df["spike"].astype(int)

# Canal linear
df["ch"] = df["row"] * COLS + df["col"]

# Cria coluna da corrente com ganho
df["I_gain"] = df["I"] 
print(">>> CSV carregado com sucesso")

# ====================== CÁLCULO DO dt ======================
print(">>> Calculando dt")
df = df.sort_values("timestamp_us")
df["dt"] = df.groupby(["row", "col"])["timestamp_us"].diff()
df_dt = df.dropna(subset=["dt"])

# Converte dt para ms
df_dt["dt_ms"] = df_dt["dt"] / 1000.0

print(">>> dt calculado")




# ====================== dt AO LONGO DO TEMPO (1 TAXEL) ======================
print(">>> Mostrando dt ao longo do tempo (1 taxel)")
row_sel, col_sel = 0, 0
taxel = df_dt[(df_dt.row == row_sel) & (df_dt.col == col_sel)]

plt.figure(figsize=(10,4))
plt.plot(taxel["timestamp_us"]/1e6, taxel["dt_ms"])
plt.title(f"dt ao longo do tempo — Taxel R{row_sel}C{col_sel}")
plt.xlabel("Tempo (s)")
plt.ylabel("dt (ms)")
plt.grid(True)


# ====================== CORRENTE × dt ======================
print(">>> Corrente em função do dt (scatter global em ms)")
plt.figure(figsize=(8,6))
plt.scatter(df_dt["dt_ms"], df_dt["I_gain"], s=5, alpha=0.3)
plt.title("Corrente I*G em função do dt (todos os taxels)")
plt.xlabel("dt (ms)")
plt.ylabel("Corrente I*G")
plt.grid(True, linestyle="--", alpha=0.4)

print(">>> Corrente em função do dt — uma curva por taxel (em ms)")
plt.figure(figsize=(12,6))
plt.title("Corrente I*G em função do dt (uma curva por taxel)")
plt.xlabel("dt (ms)")
plt.ylabel("Corrente I*G")

for (row, col), taxel in df_dt.groupby(["row", "col"]):
    taxel = taxel.sort_values("dt_ms")  # Ordena por dt
    plt.plot(taxel["dt_ms"], taxel["I_gain"], linewidth=1, alpha=0.8, label=f"R{row}C{col}")

plt.legend(ncol=4, fontsize=7)
plt.grid(True, linestyle="--", alpha=0.4)
plt.tight_layout()
plt.show()

print(">>> SCRIPT FINALIZADO COM SUCESSO")