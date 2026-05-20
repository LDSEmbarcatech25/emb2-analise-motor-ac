# BitDogLab — Monitoramento Preditivo de Motores AC

![Platform](https://img.shields.io/badge/platform-KIIRA%20%7C%20ESP32--S3-blue)
![Language](https://img.shields.io/badge/language-C%2FC%2B%2B-lightgrey)
![Framework](https://img.shields.io/badge/framework-ESP--IDF%20%7C%20PlatformIO-green)
![Architecture](https://img.shields.io/badge/architecture-Dual--Core-teal)
![Protocol](https://img.shields.io/badge/protocol-MQTT%20%2B%20HTTP-orange)

Sistema IoT embarcado para **manutenção preditiva de motores elétricos AC**, utilizando a plataforma **KIIRA (ESP32-S3 Dual-Core)**. O dispositivo monitora continuamente vibração, temperatura, umidade e áudio, detectando condições anormais e acionando proteção automática via relé.

---

## Objetivo

Detectar precocemente falhas mecânicas em motores industriais, como:

- Desalinhamento
- Desgaste de rolamentos
- Cavitação
- Vibração excessiva
- Superaquecimento

O sistema:

- Monitora vibração RMS e picos com MPU6050
- Mede temperatura e umidade com DHT22
- Realiza gravação acústica sob demanda via microfone PDM
- Exibe diagnósticos em OLED SSD1306
- Publica telemetria via MQTT
- Aciona relé de proteção automaticamente
- Salva backup local em cartão SD

---

## Hardware utilizado

| Componente | GPIO / Interface | Função |
|---|---|---|
| ESP32-S3 (KIIRA) | Dual-Core Xtensa LX7 | Processamento principal |
| MPU6050 | I2C0 — GPIO14/21 | Vibração 3 eixos |
| DHT22 | GPIO36 | Temperatura e umidade |
| Microfone PDM | GPIO2/37 (I2S0) | Captura acústica |
| OLED SSD1306 | I2C0 — GPIO14/21 | Interface local |
| Buzzer PWM | GPIO3 | Alertas sonoros |
| Relé | GPIO47 | Proteção/desarme do motor |
| SD Card | GPIO38/39/40 | Backup CSV |
| Botão Reset | GPIO45 | Reset da proteção |
| Botão Silenciar | GPIO46 | Silenciar buzzer |

---

## Arquitetura do Sistema

```text
Sensores ──► ESP32-S3 ──► OLED / Relé / Buzzer / SD
                  │
                  ├── MQTT (HiveMQ)
                  └── HTTP POST (upload de áudio)
```

### Tasks FreeRTOS

| Task | Core | Função |
|---|---|---|
| `task_sensors` | 0 | Leitura MPU6050 + DHT22 |
| `task_protection` | 0 | Proteção e acionamento do relé |
| `task_telemetry` | 0 | MQTT + gravação SD |
| `task_display` | 1 | Atualização do OLED |
| `task_buzzer` | 1 | Alarmes sonoros |
| `task_audio_record` | 1 | Gravação e envio de áudio |

---

## Comunicação IoT

| Canal | Protocolo |
|---|---|
| Telemetria e comandos | MQTT (QoS 1) |
| Upload de áudio | HTTP POST |
| Backup offline | CSV em SD Card |

### Tópicos MQTT

```text
motor/{id}/telemetry
motor/{id}/status
motor/{id}/cmd
```

### Exemplo de telemetria

```json
{
  "device_id": "motor_01",
  "temp_c": 42.5,
  "humidity_pct": 65.2,
  "accel_rms_g": 0.87,
  "motor_state": "ARMED"
}
```

---

## Estados do Sistema

```text
IDLE → ARMED → ALERT → PROTECTED → COOLDOWN → IDLE
```

### Limiares

| Parâmetro | Alerta | Crítico |
|---|---|---|
| Temperatura | 70 °C | 85 °C |
| Vibração RMS | 1.5 g | 3.0 g |

Após proteção, o sistema mantém bloqueio de 60 s antes do religamento.

---

## Compilação

### Dependências

- ESP-IDF / PlatformIO
- Python 3
- VSCode + PlatformIO

### Build

```bash
pio run
```

### Upload

```bash
pio run --target upload
```

### Monitor serial

```bash
pio device monitor
```

---

## Estrutura do Projeto

```text
src/
├── main.cpp
├── sensors/
├── connectivity/
├── display/
├── protection/
├── audio/
└── storage/
```

---

## Dashboard e Nuvem

| Serviço | Função |
|---|---|
| HiveMQ Cloud | Broker MQTT |
| Node-RED | Dashboard e automação |
| InfluxDB | Histórico temporal |
| Grafana | Visualização |
| Flask / Node.js | Recebimento de áudio |

---

## Créditos

| Ferramenta | Uso |
|---|---|
| ESP-IDF / PlatformIO | Firmware |
| Autodesk Inventor | Modelagem 3D |
| HiveMQ Cloud | MQTT Broker |
| Node-RED | Dashboard |
| Claude Sonnet 4.6 | Apoio na arquitetura/documentação |

---

**Autor:** Klysmann G. F. Barbosa  
**Curso:** EmbarcaTech — Sistemas Embarcados  
**Plataforma:** KIIRA — ESP32-S3  
**Data:** Maio de 2026  
**Github:** https://github.com/kgfbarbosa/EMB-embarcatech