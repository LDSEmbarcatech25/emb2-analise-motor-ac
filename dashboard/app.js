/* =============================================================================
   KIIRA PREDICT - INTERFACE & CONTROLADORA PRINCIPAL DE MONITORAMENTO
   ============================================================================= */

// =============================================================================
// Controle de Acesso Baseado em Nível (RBAC) & Autenticação
// =============================================================================
const applyRbac = (role) => {
    if (role === 'operator') {
        const btnInject = document.getElementById('btn-inject-anomaly');
        if (btnInject) btnInject.style.display = 'none';

        const btnSaveThreshold = document.querySelector('#form-thresholds button[type="submit"]');
        if (btnSaveThreshold) {
            btnSaveThreshold.disabled = true;
            btnSaveThreshold.innerHTML = '<i class="fa-solid fa-lock"></i> Acesso Negado';
        }
    } else if (role === 'viewer') {
        const controlsToHide = [
            'btn-inject-anomaly', 'btn-rearm', 'btn-force-stop', 'btn-start-simulation',
            'btn-rearme', 'btn-shutdown', 'btn-test-buzzer', 'btn-save-sd', 'btn-read-sd'
        ];
        controlsToHide.forEach(id => {
            const el = document.getElementById(id);
            if (el) el.style.display = 'none';
        });
        const formThreshold = document.getElementById('form-thresholds');
        if (formThreshold) formThreshold.style.opacity = '0.5';
        const btnSaveThreshold = document.querySelector('#form-thresholds button[type="submit"]');
        if (btnSaveThreshold) btnSaveThreshold.disabled = true;
    }
};

const getRoleLabel = (role) => {
    if (role === 'admin') return 'Engenheiro (Admin)';
    if (role === 'operator') return 'Operador de Máquina';
    return 'Visualizador (Read-Only)';
};

const checkAuth = async () => {
    const token = localStorage.getItem('kiira_token');
    if (!token) {
        globalThis.location.href = 'login.html';
        return;
    }

    try {
        const response = await fetch(`${CONFIG.API_URL}/api/auth/me`, {
            headers: { 'Authorization': `Bearer ${token}` }
        });

        if (!response.ok) throw new Error('Sessão expirada ou inválida');

        const data = await response.json();
        const { role, email } = data.user;

        document.getElementById('user-email-display').textContent = email;
        document.getElementById('user-role-display').textContent = getRoleLabel(role);

        // 🔒 Aplica o RBAC (Role-Based Access Control) escondendo botões não autorizados
        applyRbac(role);
    } catch (error) {
        console.error('Erro de autenticação:', error);
        localStorage.clear();
        globalThis.location.href = 'login.html';
    }
};

// Verifica a autenticação imediatamente antes de montar os gráficos
checkAuth();

document.addEventListener('DOMContentLoaded', () => {
    const btnLogout = document.getElementById('btn-logout');
    if (btnLogout) {
        btnLogout.addEventListener('click', () => {
            localStorage.clear();
            globalThis.location.href = 'login.html';
        });
    }
});

// =============================================================================
// Estado da Aplicação e Configurações
// =============================================================================
const DEVICE_ID = "motor_ac_compressor_01";
let FIRMWARE_HASH = "Aguardando Conexão...";

// Tópicos MQTT (Hierarquia Industrial)
const TOPICS = {
    telemetry: `motor/${DEVICE_ID}/telemetry`,
    status: `motor/${DEVICE_ID}/status`,
    cmd: `motor/${DEVICE_ID}/cmd`,
    cmd_response: `motor/${DEVICE_ID}/cmd/response`,
    thresholds: `motor/${DEVICE_ID}/thresholds`,
    audit: `motor/${DEVICE_ID}/audit`,
    sd_data: `motor/${DEVICE_ID}/sd_data`
};

let mqttClient = null;
let simulationInterval = null;
let isSimulationActive = false;
let isSimulatedAnomalyActive = false;
let lastGetSdDataCmdId = null;
let sdReadTimeout = null;

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
        vibCritical: 3,
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
    labels: new Array(maxDataPoints).fill(''),
    datasets: [
        {
            label: 'Vibração Resultante (g)',
            data: new Array(maxDataPoints).fill(null),
            borderColor: '#00f0ff',
            backgroundColor: 'rgba(0, 240, 255, 0.1)',
            borderWidth: 2,
            tension: 0.4,
            fill: true,
            yAxisID: 'y'
        },
        {
            label: 'Temperatura (°C)',
            data: new Array(maxDataPoints).fill(null),
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
// Gestão do Mural de Auditoria Blockchain (Web3 Real)
// =============================================================================
const BADGE_CLASS_MAP = {
    SHUTDOWN: 'badge-shutdown',
    OVERRIDE: 'badge-override',
    THRESHOLD_CHANGE: 'badge-threshold',
    BOOT: 'badge-boot'
};

// Configurações do Smart Contract
const CONTRACT_ADDRESS = "0x331be5d12B642FD1f25378EC01b0E9ff353f86fc";
const CONTRACT_ABI = [
    "function logEvent(string _eventType, string _description, string _firmwareHash) public",
    "event EventLogged(uint256 indexed id, uint256 timestamp, string eventType, string firmwareHash, address indexed operator)"
];

let provider = null;
let signer = null;
let contract = null;
let walletAddress = null;

async function connectWallet() {
    if (globalThis.ethereum === undefined) {
        alert('MetaMask não encontrada! Instale a extensão do MetaMask para registrar eventos na Blockchain.');
        return;
    }

    try {
        await globalThis.ethereum.request({ method: 'eth_requestAccounts' });
        provider = new ethers.providers.Web3Provider(globalThis.ethereum);
        signer = provider.getSigner();
        walletAddress = await signer.getAddress();

        contract = new ethers.Contract(CONTRACT_ADDRESS, CONTRACT_ABI, signer);

        const shortAddr = walletAddress.slice(0, 6) + '...' + walletAddress.slice(-4);
        document.getElementById('wallet-address-display').innerText = `Conectado: ${shortAddr}`;
        document.getElementById('wallet-address-display').style.color = '#10b981';
        document.getElementById('btn-connect-wallet').style.background = '#10b981';
        document.getElementById('btn-connect-wallet').innerHTML = '<i class="fa-solid fa-link"></i> Web3 Online';

        console.log("Web3 Conectado:", walletAddress);
    } catch (err) {
        console.error("Erro ao conectar MetaMask:", err);
    }
}

async function addBlockchainAuditRow(eventType, description) {
    // Se não estiver conectado à Web3 ou endereço não configurado, ignora a auditoria
    if (!signer || !contract || CONTRACT_ADDRESS === "0x0000000000000000000000000000000000000000") {
        console.warn(`[Auditoria] Evento ignorado (Web3 não conectado): ${eventType}`);
        return;
    }

    const tbody = document.getElementById('ledger-rows');
    const now = new Date();
    const timestampStr = `${now.toLocaleDateString()} ${now.toLocaleTimeString()}`;
    const badgeClass = BADGE_CLASS_MAP[eventType] ?? 'badge-boot';
    const firmShort = FIRMWARE_HASH.length > 15 ? FIRMWARE_HASH.slice(0, 12) : FIRMWARE_HASH;

    let txHash = "";
    let txShort = "";
    let txStatusText = "";
    let operatorAddr = walletAddress.slice(0, 6) + '...' + walletAddress.slice(-4);

    try {
        console.log(`Web3: Enviando transação para ${eventType}...`);
        const tx = await contract.logEvent(eventType, description, FIRMWARE_HASH);
        txHash = tx.hash;
        txShort = txHash.slice(0, 16) + '...';
        txStatusText = `Consulta Web3 Explorer:\nTransação enviada para a rede!\nTxHash: ${txHash}`;
    } catch (err) {
        console.error("Erro ao registrar no Smart Contract:", err);
        alert("A transação falhou ou foi rejeitada pelo usuário.");
        return;
    }

    const tr = document.createElement('tr');
    tr.innerHTML = String.raw`
        <td><strong>${timestampStr}</strong></td>
        <td><span class="event-badge ${badgeClass}">${eventType}</span></td>
        <td>${description}</td>
        <td class="code-hash-active">${operatorAddr}</td>
        <td class="code-hash" title="${FIRMWARE_HASH}">${firmShort}...</td>
        <td class="code-hash-active tx-success">
            <a href="#" onclick="alert('${txStatusText}'); return false;">${txShort}</a>
        </td>
    `;

    tbody.insertBefore(tr, tbody.firstChild);
}

// =============================================================================
// Lógica de Atualização Visual (Renderizador do Estado)
// =============================================================================
const STATE_CONFIG = {
    ARMED: { cls: 'text-armed', sub: 'Monitoramento ativo. Relé fechado, motor energizado.', rearme: true, shutdown: false, inject: false, testBuzzer: false, saveSd: false, readSd: false },
    ALERT: { cls: 'text-alert', sub: 'Fadiga elevada ou aquecimento detectado! Risco moderado.', rearme: true, shutdown: false, inject: false, testBuzzer: false, saveSd: false, readSd: false },
    PROTECTED: { cls: 'text-protected', sub: 'DESARME DE SEGURANÇA! Relé aberto devido a violação crítica.', rearme: false, shutdown: true, inject: true, testBuzzer: true, saveSd: false, readSd: false },
    COOLDOWN: { cls: 'text-cooldown', sub: 'Resfriando carcaça do motor. Travamento mecânico ativo.', rearme: true, shutdown: true, inject: true, testBuzzer: true, saveSd: false, readSd: false },
    IDLE: { cls: 'text-idle', sub: 'Motor desligado manualmente pelo operador.', rearme: false, shutdown: true, inject: true, testBuzzer: false, saveSd: false, readSd: false }
};

function renderDashboardState() {
    const valVib = document.getElementById('val-vib-resultant');
    const valTemp = document.getElementById('val-temperature');
    const valHum = document.getElementById('val-humidity');
    const valPeak = document.getElementById('val-vib-peak');
    const stateVal = document.getElementById('motor-state-value');
    const stateSub = document.getElementById('motor-state-subtext');
    const btnRearme = document.getElementById('btn-rearme');
    const btnShutdown = document.getElementById('btn-shutdown');
    const btnInject = document.getElementById('btn-inject-anomaly');
    const btnTestBuzzer = document.getElementById('btn-test-buzzer');
    const btnSaveSd = document.getElementById('btn-save-sd');
    const btnReadSd = document.getElementById('btn-read-sd');

    // Telemetria textual
    valVib.innerText = appState.vibResultant.toFixed(2);
    valTemp.innerText = appState.temperature.toFixed(1);
    valHum.innerText = appState.humidity.toFixed(1);
    valPeak.innerText = `${appState.vibPeak.toFixed(2)}g`;

    // Limiares
    document.getElementById('lbl-vib-critical').innerText = appState.thresholds.vibCritical.toFixed(1);
    document.getElementById('lbl-temp-critical').innerText = appState.thresholds.tempCritical.toFixed(1);

    // Barras de progresso
    const maxVibProgress = (appState.vibResultant / appState.thresholds.vibCritical) * 100;
    const progressVibEl = document.getElementById('progress-vib');
    progressVibEl.style.width = `${Math.min(maxVibProgress, 100)}%`;

    const maxTempProgress = (appState.temperature / appState.thresholds.tempCritical) * 100;
    const progressTempEl = document.getElementById('progress-temp');
    progressTempEl.style.width = `${Math.min(maxTempProgress, 100)}%`;

    // Ajusta classes de cores das barras
    progressVibEl.className = getBarClass(appState.vibResultant, appState.thresholds.vibAlert, appState.thresholds.vibCritical);
    progressTempEl.className = getBarClass(appState.temperature, appState.thresholds.tempAlert, appState.thresholds.tempCritical);

    // Estado do motor
    stateVal.innerText = appState.motorState;
    stateVal.className = 'state-indicator';

    const cfg = STATE_CONFIG[appState.motorState];
    if (cfg) {
        stateVal.classList.add(cfg.cls);
        stateSub.innerText = cfg.sub;
        btnRearme.disabled = cfg.rearme;
        btnShutdown.disabled = cfg.shutdown;
        btnInject.disabled = cfg.inject;
        if (btnTestBuzzer) btnTestBuzzer.disabled = cfg.testBuzzer;
        if (btnSaveSd) btnSaveSd.disabled = cfg.saveSd;
        if (btnReadSd) btnReadSd.disabled = cfg.readSd;
    }
}

function getBarClass(value, alertThreshold, criticalThreshold) {
    if (value >= criticalThreshold) return 'progress-bar bar-critical';
    if (value >= alertThreshold) return 'progress-bar bar-alert';
    return 'progress-bar bar-normal';
}

// =============================================================================
// MOTOR DE SIMULAÇÃO LOCAL (DEMO SEM HARDWARE)
// =============================================================================
let simTime = 0;
let cooldownTimer = 0;

function runCooldownStep() {
    appState.vibResultant = 0.05 + (Math.random() * 0.02);
    appState.vibPeak = appState.vibResultant * 1.414;
    appState.temperature -= 0.6;
    if (appState.temperature < 45) appState.temperature = 45;

    cooldownTimer--;
    document.getElementById('motor-state-subtext').innerText = `Resfriando motor. Cooldown ativo: ${cooldownTimer}s restantes.`;

    if (cooldownTimer <= 0) {
        appState.motorState = 'IDLE';
        addBlockchainAuditRow('BOOT', 'Cooldown concluído. Sistema de Rearme liberado.');
    }
}

function runIdleStep() {
    appState.vibResultant = 0.02 + (Math.random() * 0.01);
    appState.vibPeak = appState.vibResultant * 1.4;
    appState.temperature -= 0.2;
    if (appState.temperature < 40) appState.temperature = 40;
}

function computeBaseVib() {
    let baseVib = 0.6 + (Math.sin(simTime * 1.5) * 0.15) + (Math.random() * 0.15);

    if (isSimulatedAnomalyActive) {
        baseVib += 2.8;
        appState.temperature += 15;
        appState.vibPeak = baseVib * 1.8;
        isSimulatedAnomalyActive = false;
    } else if (Math.random() < 0.03) {
        baseVib += 2.6;
        appState.temperature += 6;
        appState.vibPeak = baseVib * 1.8;
    } else {
        appState.vibPeak = baseVib * 1.414;
    }

    return baseVib;
}

function evaluateMotorProtection(baseVib) {
    const { vibCritical, tempCritical, vibAlert, tempAlert } = appState.thresholds;

    if (baseVib >= vibCritical || appState.temperature >= tempCritical) {
        appState.motorState = 'PROTECTED';
        const causa = baseVib >= vibCritical ? 'Vibração crítica violada' : 'Superaquecimento crítico';
        addBlockchainAuditRow('SHUTDOWN', `Desarme preventivo! Causa: ${causa} (Vib: ${baseVib.toFixed(2)}g, Temp: ${appState.temperature.toFixed(1)}°C)`);

        setTimeout(() => {
            if (appState.motorState === 'PROTECTED') {
                appState.motorState = 'COOLDOWN';
                cooldownTimer = 15;
            }
        }, 3000);
    } else if (baseVib >= vibAlert || appState.temperature >= tempAlert) {
        appState.motorState = 'ALERT';
    } else {
        appState.motorState = 'ARMED';
    }
}

function runLocalSimulationStep() {
    simTime += 0.1;

    if (appState.motorState === 'COOLDOWN') {
        runCooldownStep();
        updateChart(appState.vibResultant, appState.temperature);
        renderDashboardState();
        return;
    }

    if (appState.motorState === 'IDLE') {
        runIdleStep();
        updateChart(appState.vibResultant, appState.temperature);
        renderDashboardState();
        return;
    }

    // Simulação normal (ARMED ou ALERT)
    appState.temperature = 50 + (Math.sin(simTime * 0.05) * 8) + (Math.random() * 0.5);
    appState.humidity = 55 + (Math.cos(simTime * 0.02) * 3);

    const baseVib = computeBaseVib();
    appState.vibResultant = baseVib;

    evaluateMotorProtection(baseVib);
    updateChart(appState.vibResultant, appState.temperature);
    renderDashboardState();
}

function setSimulationMode(active) {
    isSimulationActive = active;
    if (active) {
        if (mqttClient?.isConnected()) {
            mqttClient.disconnect();
        }

        appState.connection = 'ONLINE (SIMULADO)';
        document.getElementById('mqtt-connection-badge').className = 'status-badge state-connected';
        document.getElementById('mqtt-status-text').innerText = 'ONLINE (SIMULADO)';

        addBlockchainAuditRow('BOOT', 'Modo Simulação Local ativado pelo desenvolvedor.');
        simulationInterval = setInterval(runLocalSimulationStep, 1000);
    } else {
        clearInterval(simulationInterval);
        appState.connection = 'OFFLINE';
        document.getElementById('mqtt-connection-badge').className = 'status-badge state-disconnected';
        document.getElementById('mqtt-status-text').innerText = 'DESCONECTADO';
        connectMQTT();
    }
}

// =============================================================================
// ECLIPSE PAHO MQTT WEBSOCKETS CLIENT (REDE REAL)
// =============================================================================
function connectMQTT() {
    if (isSimulationActive) return;

    const clientId = `kiira_dash_${Math.random().toString(16).slice(2, 10)}`;
    const host = 'broker.hivemq.com';
    // Forçamos o uso de WebSockets Seguros (WSS - Porta 8884) para garantir máxima compatibilidade
    // em navegadores modernos e evitar bloqueios de portas por firewalls locais
    const port = 8884;

    console.log(`MQTT: Conectando a ${host}:${port} (SSL: true)...`);

    mqttClient = new Paho.MQTT.Client(host, Number(port), clientId);
    mqttClient.onConnectionLost = onConnectionLost;
    mqttClient.onMessageArrived = onMessageArrived;

    mqttClient.connect({
        onSuccess: onConnectSuccess,
        onFailure: onConnectFailure,
        useSSL: true,
        keepAliveInterval: 30
    });
}

function onConnectSuccess() {
    console.log('MQTT: Conectado com sucesso ao HiveMQ!');
    appState.connection = 'ONLINE';

    document.getElementById('mqtt-connection-badge').className = 'status-badge state-connected';
    document.getElementById('mqtt-status-text').innerText = 'ONLINE';

    mqttClient.subscribe(TOPICS.telemetry);
    mqttClient.subscribe(TOPICS.status);
    mqttClient.subscribe(TOPICS.cmd_response);
    mqttClient.subscribe(TOPICS.audit);
    mqttClient.subscribe(TOPICS.sd_data);

    addBlockchainAuditRow('BOOT', 'Gateway IoT Node-RED ativo. Sessão MQTT sincronizada.');
}

function onConnectFailure(error) {
    console.warn('MQTT: Falha de conexão: ', error);
    document.getElementById('mqtt-status-text').innerText = 'ERRO CONEXÃO';
    setTimeout(connectMQTT, 5000);
}

function onConnectionLost(responseObject) {
    if (responseObject.errorCode !== 0) {
        console.warn('MQTT: Conexão perdida: ' + responseObject.errorMessage);
        appState.connection = 'OFFLINE';
        document.getElementById('mqtt-connection-badge').className = 'status-badge state-disconnected';
        document.getElementById('mqtt-status-text').innerText = 'DESCONECTADO';
        setTimeout(connectMQTT, 5000);
    }
}

function resolveMotorStateFromJson(state) {
    if (state === 1) return 'ARMED';
    if (state === 3) return 'PROTECTED';
    if (state === 4) return 'COOLDOWN';
    return 'IDLE';
}

function handleTelemetry(json) {
    appState.vibResultant = json.vib_rms_g;
    appState.temperature = json.temp_c;
    appState.motorState = resolveMotorStateFromJson(json.state);
    updateChart(appState.vibResultant, appState.temperature);
    renderDashboardState();
}

function handleStatus(json) {
    if (json.firmware_hash) {
        FIRMWARE_HASH = json.firmware_hash;
        const hashEl = document.getElementById('firmware-hash-display');
        if (hashEl) {
            hashEl.title = FIRMWARE_HASH;
            hashEl.textContent = FIRMWARE_HASH.slice(0, 8) + '...';
        }
    }

    if (json.alert) {
        appState.motorState = 'PROTECTED';
        addBlockchainAuditRow('SHUTDOWN', `DESARME CRÍTICO! Causa: ${json.alert.cause} (Medido: ${json.alert.measured_value.toFixed(2)})`);
        renderDashboardState();
    } else if (json.connection) {
        console.log('Status de Conectividade do Dispositivo recebida.');
    }
}

function handleCmdResponse(json) {
    console.log(`Handshake de Comando Confirmado: ID ${json.command_id} -> ${json.status}`);
    if (json.command_id === lastGetSdDataCmdId && json.status === 'ERROR') {
        displaySdData(`Erro ao carregar logs do Cartão SD: ${json.error || 'Erro desconhecido'}`);
    }
}

function onMessageArrived(message) {
    if (isSimulationActive) return;

    const topic = message.destinationName;
    const payload = message.payloadString;
    console.log(`MQTT [REC]: ${topic} -> ${payload}`);

    if (topic === TOPICS.sd_data) {
        displaySdData(payload);
        return;
    }

    try {
        const json = JSON.parse(payload);

        switch (topic) {
            case TOPICS.telemetry:
                handleTelemetry(json);
                break;
            case TOPICS.status:
                handleStatus(json);
                break;
            case TOPICS.cmd_response:
                handleCmdResponse(json);
                break;
            case TOPICS.audit:
                addBlockchainAuditRow(json.event_type, json.description);
                break;
            default:
                break;
        }
    } catch (e) {
        console.error('Erro ao fazer parse do payload JSON MQTT: ', e);
    }
}

// Função para publicar comando MQTT seguro
function publishMQTTCommand(action, value = '', duration = 0) {
    if (isSimulationActive) {
        if (action === 'SET_MOTOR_STATE' && value === 'ON') {
            appState.motorState = 'ARMED';
            addBlockchainAuditRow('OVERRIDE', 'Operador rearmou e ligou o motor via Dashboard.');
        } else if (action === 'SET_MOTOR_STATE' && value === 'OFF') {
            appState.motorState = 'IDLE';
            addBlockchainAuditRow('OVERRIDE', 'Operador desligou o motor manualmente via Dashboard.');
        } else if (action === 'INJECT_ANOMALY') {
            isSimulatedAnomalyActive = true;
            addBlockchainAuditRow('OVERRIDE', 'Injeção de anomalia manual disparada via painel.');
        } else if (action === 'SAVE_SD') {
            addBlockchainAuditRow('OVERRIDE', 'Simulando gravação manual de dados no Cartão SD.');
            alert('Dados gravados no cartão SD (Simulado)!');
        } else if (action === 'GET_SD_DATA') {
            addBlockchainAuditRow('OVERRIDE', 'Simulando leitura de dados do Cartão SD.');
            const simulatedCsv = "timestamp_ms,temp_c,humidity,vib_rms_g,vib_peak_g,state\n1000,45.21,60.12,0.92,1.30,1 (SIMULADO)\n2000,45.82,60.10,0.95,1.34,1 (SIMULADO)\n3000,46.12,60.08,0.94,1.33,1 (SIMULADO)\n4000,46.50,60.05,0.98,1.38,1 (SIMULADO)";
            setTimeout(() => displaySdData(simulatedCsv), 500);
        }
        renderDashboardState();
        return;
    }

    if (!mqttClient?.isConnected()) {
        alert('Sem conexão ativa com o Broker MQTT!');
        return;
    }

    const commandId = `cmd_${Math.random().toString(36).slice(2, 11)}`;
    const payload = JSON.stringify({
        command_id: commandId,
        action,
        value,
        params: { duration_s: duration }
    });

    const message = new Paho.MQTT.Message(payload);
    message.destinationName = TOPICS.cmd;
    message.qos = 1;
    mqttClient.send(message);
    console.log(`MQTT [PUB] (${TOPICS.cmd}) -> ${payload}`);
    return commandId;
}

// =============================================================================
// ASSOCIAÇÃO DE BOTÕES E EVENTOS DA INTERFACE (UI)
// =============================================================================

document.getElementById('sim-mode-checkbox').addEventListener('change', function (e) {
    setSimulationMode(e.target.checked);
});

document.getElementById('btn-rearme').addEventListener('click', function () {
    publishMQTTCommand('SET_MOTOR_STATE', 'ON');
});

document.getElementById('btn-shutdown').addEventListener('click', function () {
    publishMQTTCommand('SET_MOTOR_STATE', 'OFF');
});

document.getElementById('btn-inject-anomaly').addEventListener('click', function () {
    publishMQTTCommand('INJECT_ANOMALY');
});

document.getElementById('btn-test-buzzer')?.addEventListener('click', function () {
    publishMQTTCommand('TEST_BUZZER');
});

document.getElementById('btn-save-sd')?.addEventListener('click', function () {
    publishMQTTCommand('SAVE_SD');
});

document.getElementById('btn-read-sd')?.addEventListener('click', function () {
    const contentEl = document.getElementById('sd-data-content');
    if (contentEl) contentEl.textContent = "Carregando logs do Cartão SD via MQTT...";
    document.getElementById('sd-modal').style.display = 'flex';
    
    lastGetSdDataCmdId = publishMQTTCommand('GET_SD_DATA');

    // Limpa timeout anterior se houver
    if (sdReadTimeout) clearTimeout(sdReadTimeout);

    // Timeout de 10 segundos caso a placa não responda
    sdReadTimeout = setTimeout(() => {
        const text = document.getElementById('sd-data-content')?.textContent;
        if (text?.startsWith("Carregando")) {
            displaySdData("Erro: O dispositivo não respondeu dentro do limite de tempo.\n\nVerifique se:\n1. A placa ESP32-S3 está ligada e conectada ao Wi-Fi.\n2. A dashboard e o firmware estão usando o mesmo ID do dispositivo.\n3. O Cartão SD está inserido corretamente na placa.");
        }
    }, 10000);
});

const closeSdModal = () => {
    document.getElementById('sd-modal').style.display = 'none';
    if (sdReadTimeout) {
        clearTimeout(sdReadTimeout);
        sdReadTimeout = null;
    }
};
document.getElementById('btn-close-sd-modal')?.addEventListener('click', closeSdModal);
document.getElementById('btn-close-sd-modal-ok')?.addEventListener('click', closeSdModal);

document.getElementById('btn-download-csv')?.addEventListener('click', function () {
    const text = document.getElementById('sd-data-content')?.textContent;
    if (!text || text.startsWith("Carregando") || text.startsWith("Erro")) return;
    
    const blob = new Blob([text], { type: 'text/csv' });
    const url = globalThis.URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.setAttribute('href', url);
    a.setAttribute('download', `telemetry_sd_${DEVICE_ID}.csv`);
    a.click();
    globalThis.URL.revokeObjectURL(url);
});

function displaySdData(csvContent) {
    if (sdReadTimeout) {
        clearTimeout(sdReadTimeout);
        sdReadTimeout = null;
    }
    const contentEl = document.getElementById('sd-data-content');
    if (contentEl) {
        contentEl.textContent = csvContent;
    }
}

document.getElementById('thresholds-form').addEventListener('submit', function (e) {
    e.preventDefault();

    const vibAlert = Number.parseFloat(document.getElementById('input-vib-alert').value);
    const vibCritical = Number.parseFloat(document.getElementById('input-vib-critical').value);
    const tempAlert = Number.parseFloat(document.getElementById('input-temp-alert').value);
    const tempCritical = Number.parseFloat(document.getElementById('input-temp-critical').value);

    appState.thresholds = { vibAlert, vibCritical, tempAlert, tempCritical };

    addBlockchainAuditRow('THRESHOLD_CHANGE', `Alteração de Limiares. Vib. Crítica: ${vibCritical.toFixed(1)}g | Temp. Crítica: ${tempCritical.toFixed(1)}°C`);

    if (!isSimulationActive && mqttClient?.isConnected()) {
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
        alert('Limiares de segurança enviados via MQTT com sucesso!');
    } else {
        alert('Simulação local atualizada! Novos limiares operacionais salvos.');
    }
    renderDashboardState();
});

// =============================================================================
// Controle do Tema Claro / Escuro
// =============================================================================
const btnThemeToggle = document.getElementById('btn-theme-toggle');

function updateChartTheme(isLight) {
    if (!realtimeChart) return;
    const textColor = isLight ? '#64748b' : '#8e9bb5';
    const gridColor = isLight ? 'rgba(0, 0, 0, 0.05)' : 'rgba(255, 255, 255, 0.03)';
    
    realtimeChart.options.plugins.legend.labels.color = textColor;
    realtimeChart.options.scales.y.grid.color = gridColor;
    realtimeChart.options.scales.y.ticks.color = textColor;
    realtimeChart.options.scales.y.title.color = isLight ? '#0284c7' : '#00f0ff';
    realtimeChart.options.scales.y1.ticks.color = textColor;
    realtimeChart.options.scales.y1.title.color = isLight ? '#f59e0b' : '#ff9f00';
    
    realtimeChart.update();
}

function setTheme(isLight) {
    const iconSun = document.querySelector('.btn-theme-toggle .icon-sun');
    const iconMoon = document.querySelector('.btn-theme-toggle .icon-moon');
    
    if (isLight) {
        document.body.classList.add('light-theme');
        localStorage.setItem('theme', 'light');
        if (iconSun) iconSun.style.display = 'inline-block';
        if (iconMoon) iconMoon.style.display = 'none';
    } else {
        document.body.classList.remove('light-theme');
        localStorage.setItem('theme', 'dark');
        if (iconSun) iconSun.style.display = 'none';
        if (iconMoon) iconMoon.style.display = 'inline-block';
    }
    updateChartTheme(isLight);
}

btnThemeToggle?.addEventListener('click', function () {
    const isLight = document.body.classList.contains('light-theme');
    setTheme(!isLight);
});

// Aplica o tema salvo no localStorage na inicialização
const savedTheme = localStorage.getItem('theme');
if (savedTheme === 'light') {
    setTheme(true);
} else {
    setTheme(false);
}

// =============================================================================
// EXECUÇÃO INICIAL
// =============================================================================
globalThis.addEventListener('load', function () {
    const btnReport = document.getElementById('btn-report');
    btnReport?.addEventListener('click', function () {
        const dashboardEl = document.querySelector('.main-dashboard');
        if (dashboardEl) {
            dashboardEl.dataset.date = new Date().toLocaleString('pt-BR');
        }
        globalThis.print();
    });

    // Conexão Web3 MetaMask
    const btnWallet = document.getElementById('btn-connect-wallet');
    btnWallet?.addEventListener('click', connectWallet);

    renderDashboardState();
    addBlockchainAuditRow('BOOT', 'Dispositivo inicializado, integridade do firmware validada.');
    connectMQTT();
});

