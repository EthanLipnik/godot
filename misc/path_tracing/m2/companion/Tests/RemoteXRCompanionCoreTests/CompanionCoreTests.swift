import Foundation
import Testing
@testable import RemoteXRCompanionCore

@Test func lifecycleAndReconnectAreStrict() {
	var machine = CompanionStateMachine()
	var accepted = machine.apply(.connected)
	#expect(!accepted)
	accepted = machine.apply(.beginDiscovery)
	#expect(accepted)
	accepted = machine.apply(.pairingRequired)
	#expect(accepted)
	accepted = machine.apply(.pairingAccepted)
	#expect(accepted)
	accepted = machine.apply(.connected)
	#expect(accepted)
	#expect(machine.phase == .streaming)
	accepted = machine.apply(.connectionLost)
	#expect(accepted)
	accepted = machine.apply(.retry)
	#expect(accepted)
	#expect(machine.reconnectAttempt == 1)
	accepted = machine.apply(.endpointFound)
	#expect(accepted)
	accepted = machine.apply(.connected)
	#expect(accepted)
	#expect(machine.phase == .streaming)
}

@Test func protocolEncodingIsDeterministicAndSequenceIsMonotonic() throws {
	let message = EndpointMessage(sequence: 7, kind: "media_ready", timestampNanoseconds: 123, payload: ["renderer": "vulkan", "commit": "0123"])
	try message.validate(after: 6)
	#expect(throws: CompanionProtocolError.nonMonotonicSequence) { try message.validate(after: 7) }
	let encoder = JSONEncoder()
	encoder.outputFormatting = [.sortedKeys]
	let first = try encoder.encode(message)
	let second = try encoder.encode(message)
	#expect(first == second)
	#expect(try JSONDecoder().decode(EndpointMessage.self, from: first) == message)
}

@Test func telemetryRejectsInvalidMeasurements() {
	let valid = StreamTelemetry(timestampNanoseconds: 1, renderMilliseconds: 4, encodeMilliseconds: 2, networkMilliseconds: 5, decodeMilliseconds: 2, trackingAgeMilliseconds: 8, bitrateMegabitsPerSecond: 80, packetLossPercent: 0.2)
	#expect(valid.validate())
	let invalid = StreamTelemetry(timestampNanoseconds: 1, renderMilliseconds: -.infinity, encodeMilliseconds: 2, networkMilliseconds: 5, decodeMilliseconds: 2, trackingAgeMilliseconds: 8, bitrateMegabitsPerSecond: 80, packetLossPercent: 101)
	#expect(!invalid.validate())
}
