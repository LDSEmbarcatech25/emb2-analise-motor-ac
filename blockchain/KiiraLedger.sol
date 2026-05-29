// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

import {Ownable} from "@openzeppelin/contracts/access/Ownable.sol";
import {ReentrancyGuard} from "@openzeppelin/contracts/utils/ReentrancyGuard.sol";

/**
 * @title KiiraLedger
 * @dev Smart Contract for immutable auditing of industrial events (AC motors).
 * @custom:security-contact admin@kiira.com
 */
contract KiiraLedger is Ownable, ReentrancyGuard {
    struct AuditEvent {
        uint256 timestamp;
        string eventType;
        string description;
        string firmwareHash;
        address operator;
    }

    AuditEvent[] private _events;
    uint256 private _eventCount;

    // Otimizações: Índices para consultas eficientes
    mapping(string => uint256[]) private _firmwareHashToEventIndices;
    
    // Extensões: Permissões granulares de operadores
    mapping(address => bool) private _authorizedOperators;

    event EventLogged(
        uint256 indexed id,
        uint256 timestamp,
        string eventType,
        string firmwareHash,
        address indexed operator
    );

    error KiiraLedger__InvalidIndex(uint256 index, uint256 maxIndex);
    error KiiraLedger__UnauthorizedOperator();
    error KiiraLedger__InvalidInput();

    constructor() Ownable(msg.sender) {}

    /*//////////////////////////////////////////////////////////////
                        ADMIN FUNCTIONS
    //////////////////////////////////////////////////////////////*/

    /**
     * @dev Authorizes an operator (like a specific engineer's wallet or the motor's wallet) to log events.
     */
    function authorizeOperator(address _operator) external onlyOwner {
        _authorizedOperators[_operator] = true;
    }

    /**
     * @dev Revokes an operator's authorization.
     */
    function revokeOperator(address _operator) external onlyOwner {
        _authorizedOperators[_operator] = false;
    }

    /**
     * @dev Checks if an operator is authorized.
     */
    function isAuthorizedOperator(address _operator) external view returns (bool) {
        return _authorizedOperators[_operator];
    }

    /*//////////////////////////////////////////////////////////////
                    USER-FACING STATE-CHANGING FUNCTIONS
    //////////////////////////////////////////////////////////////*/
    
    /**
     * @dev Registers a new event of anomaly, shutdown, or configuration in the motor.
     */
    function logEvent(
        string calldata _eventType,
        string calldata _description,
        string calldata _firmwareHash
    ) external nonReentrant {
        if (!_authorizedOperators[msg.sender] && msg.sender != owner()) {
            revert KiiraLedger__UnauthorizedOperator();
        }

        if (bytes(_eventType).length == 0 || bytes(_firmwareHash).length == 0) {
            revert KiiraLedger__InvalidInput();
        }

        _events.push(
            AuditEvent({
                timestamp: block.timestamp,
                eventType: _eventType,
                description: _description,
                firmwareHash: _firmwareHash,
                operator: msg.sender
            })
        );

        _firmwareHashToEventIndices[_firmwareHash].push(_eventCount);

        emit EventLogged(_eventCount, block.timestamp, _eventType, _firmwareHash, msg.sender);
        _eventCount++;
    }

    /*//////////////////////////////////////////////////////////////
                    USER-FACING READ-ONLY FUNCTIONS
    //////////////////////////////////////////////////////////////*/
    
    function getEventsCount() external view returns (uint256) {
        return _eventCount;
    }

    function getEvent(uint256 _index) external view returns (
        uint256 timestamp,
        string memory eventType,
        string memory description,
        string memory firmwareHash,
        address operator
    ) {
        if (_index >= _eventCount) {
            revert KiiraLedger__InvalidIndex(_index, _eventCount - 1);
        }
        AuditEvent memory e = _events[_index];
        return (e.timestamp, e.eventType, e.description, e.firmwareHash, e.operator);
    }

    /**
     * @dev Returns an array of event IDs that match a specific firmware hash.
     */
    function getEventsByFirmwareHash(string calldata _firmwareHash) external view returns (uint256[] memory) {
        return _firmwareHashToEventIndices[_firmwareHash];
    }
}
