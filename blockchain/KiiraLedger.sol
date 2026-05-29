// SPDX-License-Identifier: MIT
pragma solidity ^0.8.19;

/**
 * @title KiiraLedger
 * @dev Smart Contract para auditoria imutável de eventos industriais (motores AC).
 */
contract KiiraLedger {
    
    struct AuditEvent {
        uint256 timestamp;
        string eventType;
        string description;
        string firmwareHash;
        address operator;
    }

    // Array que armazena todos os eventos do histórico
    AuditEvent[] public events;

    // Evento emitido na blockchain toda vez que um log é registrado
    event EventLogged(
        uint256 indexed id,
        uint256 timestamp,
        string eventType,
        string firmwareHash,
        address indexed operator
    );

    /**
     * @dev Registra um novo evento de anomalia, desarme ou configuração no motor.
     * @param _eventType Tipo do evento (ex: "SHUTDOWN", "THRESHOLD_CHANGE").
     * @param _description Descrição detalhada do que ocorreu.
     * @param _firmwareHash O hash de compilação do firmware operando no momento.
     */
    function logEvent(
        string memory _eventType,
        string memory _description,
        string memory _firmwareHash
    ) public {
        AuditEvent memory newEvent = AuditEvent({
            timestamp: block.timestamp,
            eventType: _eventType,
            description: _description,
            firmwareHash: _firmwareHash,
            operator: msg.sender
        });

        events.push(newEvent);

        uint256 eventId = events.length - 1;
        emit EventLogged(eventId, block.timestamp, _eventType, _firmwareHash, msg.sender);
    }

    /**
     * @dev Retorna o número total de eventos registrados.
     */
    function getEventsCount() public view returns (uint256) {
        return events.length;
    }

    /**
     * @dev Retorna um evento específico pelo ID.
     */
    function getEvent(uint256 _index) public view returns (
        uint256 timestamp,
        string memory eventType,
        string memory description,
        string memory firmwareHash,
        address operator
    ) {
        require(_index < events.length, "Indice fora dos limites");
        AuditEvent memory e = events[_index];
        return (e.timestamp, e.eventType, e.description, e.firmwareHash, e.operator);
    }
}
