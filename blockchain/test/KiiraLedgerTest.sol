// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

import {Test} from "forge-std/Test.sol";
import {KiiraLedger} from "../KiiraLedger.sol";

contract KiiraLedgerTest is Test {
    KiiraLedger public kiiraLedger;
    address public alice = address(1);
    address public bob = address(2);

    function setUp() public {
        kiiraLedger = new KiiraLedger();
        // O criador do contrato (address(this) no Foundry) é o owner por padrão.
    }

    function testLogEvent() public {
        // Autoriza a Alice para poder logar eventos
        kiiraLedger.authorizeOperator(alice);

        vm.prank(alice);
        kiiraLedger.logEvent(
            "SHUTDOWN",
            "Motor overheated",
            "0x1234567890abcdef1234567890abcdef1234567890abcdef1234567890abcdef"
        );

        (uint256 timestamp, string memory eventType, string memory description, string memory firmwareHash, address operator) =
            kiiraLedger.getEvent(0);

        assertEq(timestamp, block.timestamp, "Timestamp mismatch");
        assertEq(eventType, "SHUTDOWN", "Event type mismatch");
        assertEq(description, "Motor overheated", "Description mismatch");
        assertEq(firmwareHash, "0x1234567890abcdef1234567890abcdef1234567890abcdef1234567890abcdef", "Firmware hash mismatch");
        assertEq(operator, alice, "Operator mismatch");
    }

    function testLogEventRevertUnauthorized() public {
        // Bob não foi autorizado
        vm.prank(bob);
        vm.expectRevert(KiiraLedger.KiiraLedger__UnauthorizedOperator.selector);
        kiiraLedger.logEvent(
            "SHUTDOWN",
            "Motor overheated",
            "0x1234567890abcdef1234567890abcdef1234567890abcdef1234567890abcdef"
        );
    }

    function testGetEventsCount() public {
        kiiraLedger.authorizeOperator(alice);
        kiiraLedger.authorizeOperator(bob);

        vm.prank(alice);
        kiiraLedger.logEvent(
            "SHUTDOWN",
            "Motor overheated",
            "0x1234567890abcdef1234567890abcdef1234567890abcdef1234567890abcdef"
        );

        vm.prank(bob);
        kiiraLedger.logEvent(
            "THRESHOLD_CHANGE",
            "Voltage threshold updated",
            "0xabcdef1234567890abcdef1234567890abcdef1234567890abcdef1234567890"
        );

        assertEq(kiiraLedger.getEventsCount(), 2, "Event count mismatch");
    }
}
