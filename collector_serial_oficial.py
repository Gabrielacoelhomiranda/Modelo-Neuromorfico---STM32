GABRIELA COELHO
import serial
import t
ime
import csv
import re

print(">>> Script iniciado")

# ================= CONFIGURAÇÃO UART =================
UART_PORT = "COM4"
BAUD_RATE = 115200
TIMEOUT = 1

ROWS = 4
COLS = 4

# ============ TAXEL SELECIONADO ============
ROW_SEL = 0
COL_SEL = 0

# ================= REGEX =================
adc_pattern = re.compile(
    r"t=(\d+)\s*\|\s*Row\s*(\d+),\s*Col\s*(\d+).*I=([0-9.]+)"
)

spike_pattern = re.compile(
    r"Spike:\s*ch=(\d+)\s*t=(\d+)"
)

# ================= ARQUIVO CSV =================
timestamp_str = time.strftime("%Y%m%d_%H%M%S")
csv_file = f"C:\\Users\\Gabi\\OneDrive\\Área de Trabalho\\dados_7canal_R{ROW_SEL}C{COL_SEL}.csv"

with open(csv_file, mode='w', newline='') as csvfile:
    csv_writer = csv.writer(csvfile)
    csv_writer.writerow(["timestamp_us", "row", "col", "I", "spike"])

    try:
        ser = serial.Serial(UART_PORT, BAUD_RATE, timeout=TIMEOUT)
        print(f">>> Lendo dados da {UART_PORT}")
        print(f">>> Salvando em {csv_file}")
        print(f">>> Taxel selecionado: R{ROW_SEL} C{COL_SEL}\n")

        while True:
            raw = ser.readline()

            if not raw:
                continue

            # Remove lixo binário
            try:
                line = raw.decode("utf-8", errors="ignore")
            except:
                continue

            # ================= ADC =================
            m = adc_pattern.search(line)
            if m:
                timestamp = int(m.group(1))
                row = int(m.group(2))
                col = int(m.group(3))
                I = float(m.group(4))

                if row == ROW_SEL and col == COL_SEL:
                    csv_writer.writerow([timestamp, row, col, I, 0])
                    csvfile.flush()
                    print(f"{timestamp} | R{row} C{col} | I={I:.3f}")
                continue

            # ================= SPIKE =================
            m = spike_pattern.search(line)
            if m:
                ch = int(m.group(1))
                timestamp = int(m.group(2))

                row = ch // COLS
                col = ch % COLS

                if row == ROW_SEL and col == COL_SEL:
                    csv_writer.writerow([timestamp, row, col, 0.0, 1])
                    csvfile.flush()
                    print(f"{timestamp} | SPIKE | R{row} C{col}")
                continue

            # Ignora qualquer outra coisa silenciosamente

    except serial.SerialException as e:
        print("Erro ao abrir a porta serial:", e)

    except KeyboardInterrupt:
        print("\n>>> Programa interrompido pelo usuário.")