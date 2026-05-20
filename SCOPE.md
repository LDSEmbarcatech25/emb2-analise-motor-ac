# PROJETO DE ENCERRAMENTO DE RESIDÊNCIA
### DISPOSITIVO IoT PARA ANÁLISE DE VIBRAÇÃO E PREDITIVA DE MOTORES AC

## 1. O Problema 

Sou Gerente de Manutenção Industrial em uma fábrica de grande porte. Nossa linha de produção depende criticamente de vários motores elétricos AC. Quando um desses motores falha sem aviso prévio, a linha inteira para, gerando um prejuízo financeiro gigantesco por hora ociosa. Engenheiros de manutenção experientes conseguem "ouvir" e "sentir" quando um motor está prestes a falhar devido ao aumento de vibração e ruídos estranhos, mas não podemos ter técnicos em tempo integral ao lado de cada máquina.

Precisamos de uma solução IoT de monitoramento preditivo para ser fixada na carcaça de cada motor. O sistema deve monitorar constantemente a temperatura e o nível de vibração (usando um acelerômetro). Essas informações de diagnóstico e o status atual do motor devem ser exibidos localmente em um display para os operadores de chão de fábrica. Todos os dados de telemetria precisam ser enviados para uma Dashboard centralizada via internet.

Há dois pontos críticos de atuação:

* **Segurança Automatizada:** Se a vibração ou a temperatura atingirem níveis críticos de colapso iminente, o dispositivo deve acionar um relé para desligar o motor imediatamente por proteção. Após um tempo de resfriamento e segurança, o sistema deve permitir religá-lo.
* **Análise Acústica:** Além dos sensores, precisamos coletar amostras de áudio do motor. Quero poder enviar um comando remoto pela Dashboard para que o dispositivo grave 5 segundos do som do motor em funcionamento (usando um microfone digital) e envie esse arquivo de áudio para o nosso servidor. Assim, nossa equipe especializada poderá fazer análises acústicas de falhas ocultas. Também preciso conseguir ligar e desligar o motor manualmente pela Dashboard.

## 2. Recomendações de Firmware e Conectividade

* **Conectividade e Telemetria:** O sistema deve se conectar à rede local e transmitir periodicamente as leituras de temperatura, os eixos de vibração do acelerômetro e o status de funcionamento do motor (Ligado/Desligado/Alerta) para a Dashboard.
* **Comunicação Bidirecional e Assíncrona:** O firmware precisa escutar comandos em tempo real vindos do servidor para: ligar/desligar o motor manualmente e disparar a rotina de gravação de áudio.
* **Aquisição de Dados do Acelerômetro:** Para capturar anomalias mecânicas, o firmware deve ler os dados de aceleração. Recomenda-se calcular métricas como a raiz quadrada média (RMS) da aceleração ou monitorar picos de deslocamento para enviar à Dashboard, evitando saturar a rede com dados brutos contínuos.
* **Processamento de Áudio I2S:** Ao receber o comando de gravação, o firmware deve inicializar o barramento I2S para coletar os dados do microfone digital por exatamente 5 segundos. O áudio deve ser estruturado em uma taxa de amostragem adequada e transmitido para o servidor (pode ser enviado em blocos binários ou bufferizado).
* **Sinalização Local de Alerta (Buzzer e Display):** O display deve atualizar os índices de temperatura e vibração em tempo real. Em caso de anomalia ou desligamento por segurança, o Buzzer deve emitir um sinal sonoro intermitente de alerta.
* **Lógica de Proteção do Relé:** Se os limites de segurança forem violados, o relé desarma o motor. O sistema deve implementar uma trava de tempo (ex: bloqueado por 1 minuto para resfriamento) antes de aceitar uma ordem de religamento.

## 3. Requisitos de Modelagem 3D 

O modelo 3D deste gabinete exige cuidados mecânicos específicos para o ambiente industrial:

* **Acoplamento Mecânico de Vibração:** O case deve ser projetado de forma que o acelerômetro interno fique firmemente integrado ou solidário à base da caixa, e esta base deve ter um sistema de fixação rígido (ímãs industriais, abas para parafusos ou abraçadeiras) para garantir que a vibração do motor seja fielmente transferida para o sensor sem amortecimentos gerados por um case frouxo.
* **Isolamento e Janelas de Interface:** Aberturas precisas para o display e para a passagem dos cabos de potência do relé.
* **Acústica do Microfone:** O case deve possuir um pequeno duto ou furação estrategicamente posicionada logo acima do microfone I2S para permitir a captação limpa do som externo, sem abafar o áudio.
* **Dissipação Térmica Interna:** Janelas de ventilação posicionadas de forma que o calor do ESP32-S3 e do relé não fiquem confinados, impedindo que a leitura do sensor de temperatura ambiental do motor seja mascarada pelo calor interno do próprio circuito.

## 4. Entregável Inicial: Documento de Arquitetura e Viabilidade Técnico

Antes de iniciar o desenvolvimento prático do firmware ou a modelagem 3D, a equipe deverá entregar um relatório técnico de arquitetura contendo obrigatoriamente:

* **4.1. Mapeamento de Hardware e Componentes:** A equipe deve identificar e documentar os periféricos internos e externos utilizados:
    * **Identificação:** Modelos exatos do sensor de temperatura, do acelerômetro (ex: MPU6050, LIS3DH, etc.), do microfone I2S (ex: INMP441), do display, do buzzer e do relé.
    * **Pinout (Tabela de GPIOs):** Tabela detalhada especificando a conexão física de cada um desses periféricos com os pinos do ESP32-S3.
    * **Barramentos Internos:** Identificação dos protocolos de comunicação interna que serão configurados no firmware (ex: I2C para o acelerômetro/display, I2S para o microfone, GPIO Digital para o relé/buzzer).

* **4.2. Diagrama de Blocos do Sistema:** Apresentar um esquema gráfico modular do hardware. No centro deve figurar o microcontrolador ESP32-S3 e, ao redor, blocos conectados por setas indicando o fluxo de sinal:
    * **Setas de entrada (In):** Sensores de temperatura, Acelerômetro e Microfone I2S direcionadas ao ESP32-S3.
    * **Setas de saída (Out):** ESP32-S3 direcionadas ao Display, Buzzer e Relé.
    * **Fluxo de rede:** Indicação de comunicação sem fio bidirecional entre o ESP32-S3 e a Nuvem/Dashboard.

* **4.3. Especificação do Protocolo de Comunicação e IoT:**
    * **Protocolo de Aplicação:** Escolha e justificativa do protocolo de comunicação (ex: MQTT para envio de telemetria rápida e recebimento de comandos; ou uso misto de MQTT para dados e HTTP POST/FTP para o envio do arquivo de áudio de 5 segundos, caso o payload do áudio seja muito grande para o broker).
    * **Estrutura de Dados (Payloads):** Exemplos de pacotes de dados em formato JSON para envio das métricas de vibração/temperatura e exemplos dos comandos de acionamento do relé e gravação.
    * **Arquitetura de Nuvem/Dashboard:** Descrição das ferramentas que serão usadas para construir o painel de monitoramento e controle.
