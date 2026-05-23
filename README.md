# KIIRA Motor Guard — Monitoramento Preditivo de Motores AC

![Platform](https://img.shields.io/badge/platform-KIIRA%20%7C%20ESP32--S3-blue)
![Language](https://img.shields.io/badge/language-C%2FC%2B%2B-lightgrey)
![Framework](https://img.shields.io/badge/framework-ESP--IDF%20%7C%20PlatformIO-green)
![Architecture](https://img.shields.io/badge/architecture-Dual--Core-teal)
![Protocol](https://img.shields.io/badge/protocol-MQTT%20%2B%20HTTP-orange)

Sistema IoT embarcado para **manutenção preditiva e proteção automática de motores elétricos AC**, utilizando a plataforma **KIIRA (ESP32-S3 Dual-Core)**. O dispositivo é fixado à carcaça do motor e monitora continuamente vibração, temperatura, umidade e áudio, processando métricas localmente antes de transmitir telemetria para a nuvem.

A motivação principal é reduzir paradas não programadas em linhas de produção. Dois mecanismos críticos sustentam a operação:

- **Proteção automática por relé:** desarme imediato quando temperatura ou vibração ultrapassam limiares críticos, com bloqueio temporário de segurança;
- **Análise acústica remota:** gravação sob demanda e envio de áudio ao servidor para análise de ruídos anormais.

---

## Objetivo

Detectar precocemente falhas mecânicas em motores industriais, como:

- Desalinhamento
- Desgaste de rolamentos
- Cavitação
- Vibração excessiva
- Superaquecimento

O sistema:

- Monitora vibração RMS, resultante tridimensional e picos com MPU6050
- Mede temperatura e umidade com DHT22
- Realiza gravação acústica sob demanda via INMP441 (I2S)
- Exibe diagnósticos em OLED SSD1306
- Publica telemetria e status via MQTT (QoS 1) com LWT
- Aciona relé de proteção automaticamente em menos de 100 ms
- Recebe comandos remotos de operação, reconfiguração e captura de áudio
- Salva backup local em cartão MicroSD (CSV)
- Permite controles físicos locais para reset e silenciamento do buzzer

---

## Hardware utilizado

| Componente | Modelo | Interface / GPIO | Função |
|---|---|---|---|
| Microcontrolador | ESP32-S3 (KIIRA) | — | Processamento dual-core, Wi-Fi, FreeRTOS |
| Acelerômetro/Giroscópio | MPU6050 | I2C0 — GPIO14 (SDA) / GPIO21 (SCL) | Vibração 3 eixos |
| Temperatura e umidade | DHT22 / AM2302 | GPIO36 | Monitoramento térmico e ambiental |
| Microfone digital | INMP441 | I2S0 — GPIO2 (SCK) / GPIO37 (WS) / GPIO35 (SD) | Captura acústica |
| Display | OLED SSD1306 128×64 | I2C0 — GPIO14 / GPIO21 | Interface local de diagnóstico |
| Buzzer | Ativo / PWM | GPIO3 (LEDC) | Alertas sonoros locais |
| Relé | Módulo optoacoplado | GPIO47 | Proteção elétrica do motor |
| Armazenamento | MicroSD | SPI — CS:38 / SCK:40 / MOSI:39 / MISO:41 | Backup CSV e arquivos WAV |
| Botão Reset | Táctil | GPIO46 | Reset da proteção |
| Botão Silenciar | Táctil | GPIO45 | Silenciar buzzer |

---

## Arquitetura do Sistema

```text
DHT22 ──┐
MPU6050─┤                            ┌── OLED SSD1306
INMP441─┤── ESP32-S3 (KIIRA) ────────┼── Relé / Buzzer
Botões──┘   Dual-Core Xtensa LX7    ├── MicroSD
                    │                └── (local)
                    │ Wi-Fi
                    ├── MQTT (HiveMQ Cloud) ──► Node-RED / InfluxDB / Grafana
                    └── HTTP POST ──────────► Servidor de áudio (Flask / Node.js)
```

### Tasks FreeRTOS

| Task | Core | Frequência | Responsabilidade |
|---|---|---|---|
| `task_sensors` | 0 | 100 ms | Leitura MPU6050 + DHT22; cálculo local de RMS e resultante |
| `task_protection` | 0 | Alta prioridade | Verificação de limiares e acionamento do relé em < 100 ms |
| `task_telemetry` | 0 | 5 s | Publicação MQTT, recepção de comandos e backup no MicroSD |
| `task_display` | 1 | 500 ms | Atualização do OLED sem flicker |
| `task_buzzer` | 1 | 100 ms | Controle sonoro por PWM em alerta e proteção |
| `task_audio_record` | 1 | Sob demanda | Captura I2S, montagem WAV e envio HTTP POST |

Tarefas críticas de segurança (sensores e proteção) ficam isoladas no Core 0, garantindo que instabilidades de rede não interfiram no desarme do motor.

---

## Estados do Sistema

```text
IDLE ──MOTOR_ON──► ARMED ──alerta──► ALERT ──crítico──► PROTECTED
                                        │                    │
                                  normalização            cooldown 60 s
                                        │                    │
                                     ARMED ◄─── RESET ───────┘
```

### Limiares de proteção

| Parâmetro | Alerta | Crítico |
|---|---|---|
| Temperatura | 70 °C | 85 °C |
| Vibração RMS composta | 1.5 g | 3.0 g |
| Cooldown pós-proteção | — | 60 s |
| Tempo máx. de resposta | — | < 100 ms |

---

## Comunicação IoT

| Canal | Protocolo | Detalhes |
|---|---|---|
| Telemetria e comandos | MQTT QoS 1 | Bidirecional, suporte a múltiplos motores |
| Status e disponibilidade | MQTT Retained + LWT | Dashboard conhece estado atual e recebe indicação de offline |
| Upload de áudio | HTTP POST multipart | Arquivo WAV de 5 s por demanda |
| Backup offline | CSV em MicroSD | Tolerância a falhas de rede |

### Tópicos MQTT

| Tópico | Direção | Função |
|---|---|---|
| `motor/{id}/telemetry` | Dispositivo → Nuvem | Leituras periódicas de sensores |
| `motor/{id}/status` | Dispositivo → Nuvem | Estado, alertas e LWT |
| `motor/{id}/cmd` | Nuvem → Dispositivo | Comandos remotos (ligar, desligar, resetar, gravar áudio) |
| `motor/{id}/cmd/response` | Dispositivo → Nuvem | Confirmação com `command_id` |
| `motor/{id}/thresholds` | Nuvem → Dispositivo | Reconfiguração remota de limiares |

### Exemplo de telemetria

```json
{
  "timestamp": 1779361642,
  "device_id": "motor_01",
  "sensors": {
    "temp_c": 58.4,
    "humidity_pct": 45.2,
    "vibration": {
      "rms_x_g": 0.42,
      "rms_y_g": 0.35,
      "rms_z_g": 0.78,
      "resultant_rms_g": 0.95,
      "peak_g": 1.45
    }
  },
  "diagnostics": {
    "wifi_rssi_dbm": -65,
    "free_heap_bytes": 184520,
    "uptime_s": 3600
  }
}
```

---

## Dashboard e Nuvem

| Camada | Ferramenta | Função |
|---|---|---|
| Broker MQTT | HiveMQ Cloud | Roteamento de telemetria, status e comandos |
| Backend de fluxo | Node-RED | Processamento, automação e integração entre serviços |
| Banco temporal | InfluxDB | Histórico de telemetria para manutenção preditiva |
| Visualização | Grafana | Monitoramento em tempo real e análise operacional |
| Servidor de áudio | Flask / Node.js | Recebimento e armazenamento de WAV |
| Auditoria | Blockchain / Smart Contract | Registro imutável de eventos críticos (CRITICAL_SHUTDOWN, BOOT, THRESHOLD_CHANGE) |

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

## Créditos

| Ferramenta | Uso |
|---|---|
| ESP-IDF / PlatformIO | Firmware |
| Autodesk Inventor | Modelagem 3D do gabinete |
| HiveMQ Cloud | MQTT Broker |
| Node-RED | Dashboard e automação |
| InfluxDB + Grafana | Histórico e visualização |
| Claude Sonnet 4.6 | Apoio na arquitetura e documentação |

---

**Autores:** Klysmann G. F. Barbosa · Luciano R. de Morais  
**Curso:** EmbarcaTech — Sistemas Embarcados  
**Plataforma:** KIIRA — ESP32-S3  
**Data:** Maio de 2026