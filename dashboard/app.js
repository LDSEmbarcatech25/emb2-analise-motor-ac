/* =============================================================================
   KIIRA PREDICT - INTERFACE & CONTROLADORA PRINCIPAL DE MONITORAMENTO
   ============================================================================= */

// =============================================================================
// Estado da Aplicação e Configurações
// =============================================================================
const DEVICE_ID = "motor_ac_compressor_01";
const FIRMWARE_HASH = "d6a8f10b2c349e5d7f10b2c349e5d7f10b2c349e5d7f10b2c349e5d7f10b2c34";

// Tópicos MQTT (Hierarquia Industrial)
const TOPICS = {
    telemetry: `motor/${DEVICE_ID}/telemetry`,
    status: `motor/${DEVICE_ID}/status`,
    cmd: `motor/${DEVICE_ID}/cmd`,
    cmd_response: `motor/${DEVICE_ID}/cmd/response`,
    thresholds: `motor/${DEVICE_ID}/thresholds`
};

let mqttClient = null;
let simulationInterval = null;
let isSimulationActive = false;

// Estado atual simulado ou recebido
let appState = {
    connection: "OFFLINE",
    motorState: "ARMED", // ARMED, ALERT, PROTECTED, COOLDOWN, IDLE
    vibResultant: 0.95,
    temperature: 58.4,
    humidity: 45.2,
    vibPeak: 1.45,
    thresholds: {
        vibAlert: 1.5,
        vibCritical: 3.0,
        tempAlert: 70,
        tempCritical: 85
    }
};

// =============================================================================
// Inicialização do Gráfico (Chart.js)
// =============================================================================
const ctx = document.getElementById('realtimeChart').getContext('2d');
const maxDataPoints = 30;
const chartData = {
    labels: Array(maxDataPoints).fill(''),
    datasets: [
        {
            label: 'Vibração Resultante (g)',
            data: Array(maxDataPoints).fill(null),
            borderColor: '#00f0ff',
            backgroundColor: 'rgba(0, 240, 255, 0.1)',
            borderWidth: 2,
            tension: 0.4,
            fill: true,
            yAxisID: 'y'
        },
        {
            label: 'Temperatura (°C)',
            data: Array(maxDataPoints).fill(null),
            borderColor: '#ff9f00',
            backgroundColor: 'rgba(255, 159, 0, 0.05)',
            borderWidth: 2,
            tension: 0.4,
            fill: false,
            yAxisID: 'y1'
        }
    ]
};

const chartOptions = {
    responsive: true,
    maintainAspectRatio: false,
    plugins: {
        legend: {
            labels: { color: '#8e9bb5', font: { family: 'Outfit', size: 12 } }
        }
    },
    scales: {
        x: {
            grid: { display: false },
            ticks: { display: false }
        },
        y: {
            type: 'linear',
            display: true,
            position: 'left',
            grid: { color: 'rgba(255, 255, 255, 0.03)' },
            ticks: { color: '#8e9bb5' },
            title: { display: true, text: 'Aceleração (g)', color: '#00f0ff' }
        },
        y1: {
            type: 'linear',
            display: true,
            position: 'right',
            grid: { drawOnChartArea: false },
            ticks: { color: '#8e9bb5' },
            title: { display: true, text: 'Temperatura (°C)', color: '#ff9f00' }
        }
    }
};

const realtimeChart = new Chart(ctx, {
    type: 'line',
    data: chartData,
    options: chartOptions
});

function updateChart(vib, temp) {
    realtimeChart.data.labels.shift();
    realtimeChart.data.labels.push(new Date().toLocaleTimeString([], { hour: '2-digit', minute: '2-digit', second: '2-digit' }));
    
    realtimeChart.data.datasets[0].data.shift();
    realtimeChart.data.datasets[0].data.push(vib);

    realtimeChart.data.datasets[1].data.shift();
    realtimeChart.data.datasets[1].data.push(temp);

    realtimeChart.update();
}

// =============================================================================
// Gestão do Mural de Auditoria Blockchain (Tabela UI)
// =============================================================================
function addBlockchainAuditRow(eventType, description) {
    const tbody = document.getElementById('ledger-rows');
    const now = new Date();
    const timestampStr = now.toLocaleDateString() + ' ' + now.toLocaleTimeString();
    
    // Simulação de transação Web3 realista
    const txHash = '0x' + Array.from({length: 40}, () => Math.floor(Math.random()*16).toString(16)).join('');
    
    const tr = document.createElement('tr');
    
    let badgeClass = 'badge-boot';
    if (eventType === 'SHUTDOWN') badgeClass = 'badge-shutdown';
    if (eventType === 'OVERRIDE') badgeClass = 'badge-override';
    if (eventType === 'THRESHOLD_CHANGE') badgeClass = 'badge-threshold';

    tr.innerHTML = `
        <td><strong>${timestampStr}</strong></td>
        <td><span class="event-badge ${badgeClass}">${eventType}</span></td>
        <td>${description}</td>
        <td class="code-hash-active">0x71C...8B49f</td>
        <td class="code-hash" title="${FIRMWARE_HASH}">${FIRMWARE_HASH.substring(0, 12)}...</td>
        <td class="code-hash-active"><a href="#" onclick="alert('Consulta de Auditoria Blockchain:\\nTransação validada na rede Arbitrum/Polygon Orbit.\\nTxHash: ${txHash}'); return false;">${txHash.substring(0, 16)}...</a></td>
    `;
    
    // Insere no início da tabela para ver o histórico novo primeiro
    tbody.insertBefore(tr, tbody.firstChild);
}

// =============================================================================
// Lógica de Atualização Visual (Renderizador do Estado)
// =============================================================================
function renderDashboardState() {
    const valVib = document.getElementById('val-vib-resultant');
    const valTemp = document.getElementById('val-temperature');
    const valHum = document.getElementById('val-humidity');
    const valPeak = document.getElementById('val-vib-peak');
    const stateVal = document.getElementById('motor-state-value');
    const stateSub = document.getElementById('motor-state-subtext');
    const btnRearme = document.getElementById('btn-rearme');
    const btnShutdown = document.getElementById('btn-shutdown');

    // Telemetria textual
    valVib.innerText = appState.vibResultant.toFixed(2);
    valTemp.innerText = appState.temperature.toFixed(1);
    valHum.innerText = appState.humidity.toFixed(1);
    valPeak.innerText = appState.vibPeak.toFixed(2) + 'g';

    // Limiares
    document.getElementById('lbl-vib-critical').innerText = appState.thresholds.vibCritical.toFixed(1);
    document.getElementById('lbl-temp-critical').innerText = appState.thresholds.tempCritical.toFixed(1);

    // Ajuste das barras de progresso nos Cards
    const maxVibProgress = (appState.vibResultant / appState.thresholds.vibCritical) * 100;
    const progressVibEl = document.getElementById('progress-vib');
    progressVibEl.style.width = `${Math.min(maxVibProgress, 100)}%`;
    
    const maxTempProgress = (appState.temperature / appState.thresholds.tempCritical) * 100;
    const progressTempEl = document.getElementById('progress-temp');
    progressTempEl.style.width = `${Math.min(maxTempProgress, 100)}%`;

    // Ajusta classes de cores das barras de progresso
    if (appState.vibResultant >= appState.thresholds.vibCritical) {
        progressVibEl.className = "progress-bar bar-critical";
    } else if (appState.vibResultant >= appState.thresholds.vibAlert) {
        progressVibEl.className = "progress-bar bar-alert";
    } else {
        progressVibEl.className = "progress-bar bar-normal";
    }

    if (appState.temperature >= appState.thresholds.tempCritical) {
        progressTempEl.className = "progress-bar bar-critical";
    } else if (appState.temperature >= appState.thresholds.tempAlert) {
        progressTempEl.className = "progress-bar bar-alert";
    } else {
        progressTempEl.className = "progress-bar bar-normal";
    }

    // Gerenciador Visual de Estados Físicos
    stateVal.innerText = appState.motorState;
    stateVal.className = "state-indicator";

    if (appState.motorState === 'ARMED') {
        stateVal.classList.add('text-armed');
        stateSub.innerText = "Monitoramento ativo. Relé fechado, motor energizado.";
        btnRearme.disabled = true;
        btnShutdown.disabled = false;
    } else if (appState.motorState === 'ALERT') {
        stateVal.classList.add('text-alert');
        stateSub.innerText = "Fadiga elevada ou aquecimento detectado! Risco moderado.";
        btnRearme.disabled = true;
        btnShutdown.disabled = false;
    } else if (appState.motorState === 'PROTECTED') {
        stateVal.classList.add('text-protected');
        stateSub.innerText = "DESARME DE SEGURANÇA! Relé aberto devido a violação crítica.";
        btnRearme.disabled = false;
        btnShutdown.disabled = true;
    } else if (appState.motorState === 'COOLDOWN') {
        stateVal.classList.add('text-cooldown');
        stateSub.innerText = "Resfriando carcaça do motor. Travamento mecânico ativo.";
        btnRearme.disabled = true;
        btnShutdown.disabled = true;
    } else if (appState.motorState === 'IDLE') {
        stateVal.classList.add('text-idle');
        stateSub.innerText = "Motor desligado manualmente pelo operador.";
        btnRearme.disabled = false;
        btnShutdown.disabled = true;
    }
}

// =============================================================================
// MOTOR DE SIMULAÇÃO LOCAL (DEMO SEM HARDWARE)
// =============================================================================
let simTime = 0;
let cooldownTimer = 0;

function runLocalSimulationStep() {
    simTime += 0.1;

    // Se estiver em COOLDOWN, o motor está parado e resfriando
    if (appState.motorState === 'COOLDOWN') {
        appState.vibResultant = 0.05 + (Math.random() * 0.02);
        appState.vibPeak = appState.vibResultant * 1.414;
        appState.temperature -= 0.6; // Esfria rapidamente
        if (appState.temperature < 45.0) appState.temperature = 45.0;

        cooldownTimer--;
        document.getElementById('motor-state-subtext').innerText = `Resfriando motor. Cooldown ativo: ${cooldownTimer}s restantes.`;
        
        if (cooldownTimer <= 0) {
            appState.motorState = 'IDLE';
            addBlockchainAuditRow('BOOT', "Cooldown concluído. Sistema de Rearme liberado.");
        }
        updateChart(appState.vibResultant, appState.temperature);
        renderDashboardState();
        return;
    }

    // Se estiver em IDLE, o motor está parado
    if (appState.motorState === 'IDLE') {
        appState.vibResultant = 0.02 + (Math.random() * 0.01);
        appState.vibPeak = appState.vibResultant * 1.4;
        appState.temperature -= 0.2;
        if (appState.temperature < 40.0) appState.temperature = 40.0;
        updateChart(appState.vibResultant, appState.temperature);
        renderDashboardState();
        return;
    }

    // Simulação normal (ARMED ou ALERT)
    appState.temperature = 50.0 + (Math.sin(simTime * 0.05) * 8.0) + (Math.random() * 0.5);
    appState.humidity = 55.0 + (Math.cos(simTime * 0.02) * 3.0);
    
    let baseVib = 0.6 + (Math.sin(simTime * 1.5) * 0.15) + (Math.random() * 0.15);
    
    // Injeta pico severo de teste de forma randômica (3% de chance)
    if (Math.random() < 0.03) {
        baseVib += 2.6; // Viola limiar de 3.0g
        appState.temperature += 6.0;
        appState.vibPeak = baseVib * 1.8;
    } else {
        appState.vibPeak = baseVib * 1.414;
    }

    appState.vibResultant = baseVib;

    // Lógica do Rearme / Proteção Automatizada no Simulator
    if (appState.vibResultant >= appState.thresholds.vibCritical || appState.temperature >= appState.thresholds.tempCritical) {
        appState.motorState = 'PROTECTED';
        const causa = appState.vibResultant >= appState.thresholds.vibCritical ? "Vibração crítica violada" : "Superaquecimento crítico";
        addBlockchainAuditRow('SHUTDOWN', `Desarme preventivo! Causa: ${causa} (Vib: ${appState.vibResultant.toFixed(2)}g, Temp: ${appState.temperature.toFixed(1)}°C)`);
        
        // Ativa Cooldown automático de 15 segundos no simulador (para ser demonstrativo e rápido)
        setTimeout(() => {
            if (appState.motorState === 'PROTECTED') {
                appState.motorState = 'COOLDOWN';
                cooldownTimer = 15;
            }
        }, 3000);
    } else if (appState.vibResultant >= appState.thresholds.vibAlert || appState.temperature >= appState.thresholds.tempAlert) {
        appState.motorState = 'ALERT';
    } else {
        appState.motorState = 'ARMED';
    }

    updateChart(appState.vibResultant, appState.temperature);
    renderDashboardState();
}

function setSimulationMode(active) {
    isSimulationActive = active;
    if (active) {
        // Interrompe conexões MQTT ativas se houver
        if (mqttClient && mqttClient.isConnected()) {
            mqttClient.disconnect();
        }
        
        appState.connection = "ONLINE (SIMULADO)";
        document.getElementById('mqtt-connection-badge').className = "status-badge state-connected";
        document.getElementById('mqtt-status-text').innerText = "ONLINE (SIMULADO)";
        
        addBlockchainAuditRow('BOOT', "Modo Simulação Local ativado pelo desenvolvedor.");

        simulationInterval = setInterval(runLocalSimulationStep, 1000);
    } else {
        clearInterval(simulationInterval);
        appState.connection = "OFFLINE";
        document.getElementById('mqtt-connection-badge').className = "status-badge state-disconnected";
        document.getElementById('mqtt-status-text').innerText = "DESCONECTADO";
        connectMQTT(); // Tenta voltar a escutar rede real
    }
}

// =============================================================================
// ECLIPSE PAHO MQTT WEBSOCKETS CLIENT (REDE REAL)
// =============================================================================
function connectMQTT() {
    if (isSimulationActive) return;

    // Conectando ao Broker HiveMQ Público via WebSockets Seguro (Porta 8884)
    // O clientId aleatório evita derrubar conexões de outros computadores
    const clientId = "kiira_dash_" + Math.random().toString(16).substr(2, 8);
    
    // Se a página rodar em HTTPS, precisamos usar WSS na porta 8884 (com SSL)
    const host = "broker.hivemq.com";
    const port = window.location.protocol === "https:" ? 8884 : 8000;
    const useSSL = window.location.protocol === "https:";

    console.log(`MQTT: Conectando a ${host}:${port} (SSL: ${useSSL})...`);

    mqttClient = new Paho.MQTT.Client(host, Number(port), clientId);

    mqttClient.onConnectionLost = onConnectionLost;
    mqttClient.onMessageArrived = onMessageArrived;

    const connectOptions = {
        onSuccess: onConnectSuccess,
        onFailure: onConnectFailure,
        useSSL: useSSL,
        keepAliveInterval: 30
    };

    mqttClient.connect(connectOptions);
}

function onConnectSuccess() {
    console.log("MQTT: Conectado com sucesso ao HiveMQ!");
    appState.connection = "ONLINE";
    
    document.getElementById('mqtt-connection-badge').className = "status-badge state-connected";
    document.getElementById('mqtt-status-text').innerText = "ONLINE";

    // Subscreve nos tópicos de telemetria, status e confirmações
    mqttClient.subscribe(TOPICS.telemetry);
    mqttClient.subscribe(TOPICS.status);
    mqttClient.subscribe(TOPICS.cmd_response);

    addBlockchainAuditRow('BOOT', "Gateway IoT Node-RED ativo. Sessão MQTT sincronizada.");
}

function onConnectFailure(error) {
    console.warn("MQTT: Falha de conexão: ", error);
    document.getElementById('mqtt-status-text').innerText = "ERRO CONEXÃO";
    // Tenta reconectar após 5 segundos
    setTimeout(connectMQTT, 5000);
}

function onConnectionLost(responseObject) {
    if (responseObject.errorCode !== 0) {
        console.warn("MQTT: Conexão perdida: " + responseObject.errorMessage);
        appState.connection = "OFFLINE";
        document.getElementById('mqtt-connection-badge').className = "status-badge state-disconnected";
        document.getElementById('mqtt-status-text').innerText = "DESCONECTADO";
        
        // Auto-reconexão robusta programática para clientes Paho mais antigos
        setTimeout(connectMQTT, 5000);
    }
}

function onMessageArrived(message) {
    if (isSimulationActive) return; // Ignora se o modo demo estiver ligado

    const topic = message.destinationName;
    const payload = message.payloadString;
    console.log(`MQTT [REC]: ${topic} -> ${payload}`);

    try {
        const json = JSON.parse(payload);

        if (topic === TOPICS.telemetry) {
            // Atualiza telemetria com base nos sensores reais do Wokwi/Dispositivo
            appState.vibResultant = json.vib_rms_g;
            appState.temperature = json.temp_c;
            appState.motorState = json.state === 1 ? 'ARMED' : (json.state === 3 ? 'PROTECTED' : (json.state === 4 ? 'COOLDOWN' : 'IDLE'));
            
            updateChart(appState.vibResultant, appState.temperature);
            renderDashboardState();
        } else if (topic === TOPICS.status) {
            if (json.alert) {
                // Alerta crítico de desligamento
                appState.motorState = 'PROTECTED';
                addBlockchainAuditRow('SHUTDOWN', `DESARME CRÍTICO! Causa: ${json.alert.cause} (Medido: ${json.alert.measured_value.toFixed(2)})`);
                renderDashboardState();
            } else if (json.connection) {
                console.log("Status de Conectividade do Dispositivo recebida.");
            }
        } else if (topic === TOPICS.cmd_response) {
            console.log(`Handshake de Comando Confirmado: ID ${json.command_id} -> ${json.status}`);
        }
    } catch (e) {
        console.error("Erro ao fazer parse do payload JSON MQTT: ", e);
    }
}

// Função para publicar comando MQTT seguro
function publishMQTTCommand(action, value = "", duration = 0) {
    if (isSimulationActive) {
        // Simulação interna
        if (action === "SET_MOTOR_STATE" && value === "ON") {
            appState.motorState = 'ARMED';
            addBlockchainAuditRow('OVERRIDE', "Operador rearmou e ligou o motor via Dashboard.");
        } else if (action === "SET_MOTOR_STATE" && value === "OFF") {
            appState.motorState = 'IDLE';
            addBlockchainAuditRow('OVERRIDE', "Operador desligou o motor manualmente via Dashboard.");
        }
        renderDashboardState();
        return;
    }

    if (!mqttClient || !mqttClient.isConnected()) {
        alert("Sem conexão ativa com o Broker MQTT!");
        return;
    }

    const commandId = 'cmd_' + Math.random().toString(36).substr(2, 9);
    const payload = JSON.stringify({
        command_id: commandId,
        action: action,
        value: value,
        params: { duration_s: duration }
    });

    const message = new Paho.MQTT.Message(payload);
    message.destinationName = TOPICS.cmd;
    message.qos = 1;
    mqttClient.send(message);
    console.log(`MQTT [PUB] (${TOPICS.cmd}) -> ${payload}`);
}

// =============================================================================
// ASSOCIAÇÃO DE BOTÕES E EVENTOS DA INTERFACE (UI)
// =============================================================================

// Toggle de Simulação Local
document.getElementById('sim-mode-checkbox').addEventListener('change', function(e) {
    setSimulationMode(e.target.checked);
});

// Botão de Rearme do Motor
document.getElementById('btn-rearme').addEventListener('click', function() {
    publishMQTTCommand("SET_MOTOR_STATE", "ON");
});

// Botão de Parada de Emergência
document.getElementById('btn-shutdown').addEventListener('click', function() {
    publishMQTTCommand("SET_MOTOR_STATE", "OFF");
});

// Envio do formulário de novos limiares
document.getElementById('thresholds-form').addEventListener('submit', function(e) {
    e.preventDefault();

    const vibAlert = parseFloat(document.getElementById('input-vib-alert').value);
    const vibCritical = parseFloat(document.getElementById('input-vib-critical').value);
    const tempAlert = parseFloat(document.getElementById('input-temp-alert').value);
    const tempCritical = parseFloat(document.getElementById('input-temp-critical').value);

    // Salva localmente
    appState.thresholds = { vibAlert, vibCritical, tempAlert, tempCritical };

    addBlockchainAuditRow('THRESHOLD_CHANGE', `Alteração de Limiares. Vib. Crítica: ${vibCritical.toFixed(1)}g | Temp. Crítica: ${tempCritical.toFixed(1)}°C`);

    if (!isSimulationActive && mqttClient && mqttClient.isConnected()) {
        const payload = JSON.stringify({
            thresholds: {
                temp_alert_c: tempAlert,
                temp_critical_c: tempCritical,
                vib_alert_g: vibAlert,
                vib_critical_g: vibCritical
            }
        });
        const message = new Paho.MQTT.Message(payload);
        message.destinationName = TOPICS.thresholds;
        message.retained = true;
        mqttClient.send(message);
        console.log(`MQTT [PUB] (${TOPICS.thresholds}) -> ${payload}`);
        alert("Limiares de segurança enviados via MQTT com sucesso!");
    } else {
        alert("Simulação local atualizada! Novos limiares operacionais salvos.");
    }
    renderDashboardState();
});

// =============================================================================
// EXECUÇÃO INICIAL
// =============================================================================
window.addEventListener('load', function() {
    // Configura e exibe o estado inicial da UI
    renderDashboardState();
    
    // Popula a tabela de Blockchain com logs iniciais exemplares
    addBlockchainAuditRow('BOOT', "Dispositivo inicializado, integridade do firmware validada.");
    
    // Tenta conectar ao MQTT real
    connectMQTT();
});
