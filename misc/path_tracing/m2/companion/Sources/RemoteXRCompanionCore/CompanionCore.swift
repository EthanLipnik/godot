import Foundation

public enum CompanionPhase: String, Codable, Sendable {
	case idle
	case discovering
	case pairing
	case connecting
	case streaming
	case reconnecting
	case disconnected
	case failed
}

public enum CompanionEvent: Sendable {
	case beginDiscovery
	case endpointFound
	case pairingRequired
	case pairingAccepted
	case connected
	case connectionLost
	case retry
	case disconnect
	case fail(String)
}

public struct CompanionStateMachine: Sendable {
	public private(set) var phase: CompanionPhase = .idle
	public private(set) var failure: String?
	public private(set) var reconnectAttempt = 0

	public init() {}

	@discardableResult
	public mutating func apply(_ event: CompanionEvent) -> Bool {
		if case let .fail(message) = event {
			phase = .failed
			failure = message
			return true
		}
		let next: CompanionPhase?
		switch (phase, event) {
		case (.idle, .beginDiscovery), (.disconnected, .beginDiscovery): next = .discovering
		case (.discovering, .endpointFound): next = .connecting
		case (.discovering, .pairingRequired): next = .pairing
		case (.pairing, .pairingAccepted): next = .connecting
		case (.connecting, .connected), (.reconnecting, .connected): next = .streaming
		case (.streaming, .connectionLost): next = .reconnecting
		case (.reconnecting, .retry):
			reconnectAttempt += 1
			next = .discovering
		case (_, .disconnect): next = .disconnected
		default: next = nil
		}
		guard let next else { return false }
		phase = next
		failure = nil
		return true
	}
}

public struct EndpointMessage: Codable, Equatable, Sendable {
	public static let currentSchema = 1
	public let schema: Int
	public let sequence: UInt64
	public let kind: String
	public let timestampNanoseconds: UInt64
	public let payload: [String: String]

	public init(sequence: UInt64, kind: String, timestampNanoseconds: UInt64, payload: [String: String]) {
		schema = Self.currentSchema
		self.sequence = sequence
		self.kind = kind
		self.timestampNanoseconds = timestampNanoseconds
		self.payload = payload
	}

	public func validate(after previousSequence: UInt64?) throws {
		guard schema == Self.currentSchema else { throw CompanionProtocolError.unsupportedSchema }
		guard !kind.isEmpty else { throw CompanionProtocolError.emptyKind }
		if let previousSequence, sequence <= previousSequence { throw CompanionProtocolError.nonMonotonicSequence }
	}
}

public struct StreamTelemetry: Codable, Equatable, Sendable {
	public let schema: Int
	public let timestampNanoseconds: UInt64
	public let renderMilliseconds: Double
	public let encodeMilliseconds: Double
	public let networkMilliseconds: Double
	public let decodeMilliseconds: Double
	public let trackingAgeMilliseconds: Double
	public let bitrateMegabitsPerSecond: Double
	public let packetLossPercent: Double

	public init(timestampNanoseconds: UInt64, renderMilliseconds: Double, encodeMilliseconds: Double, networkMilliseconds: Double, decodeMilliseconds: Double, trackingAgeMilliseconds: Double, bitrateMegabitsPerSecond: Double, packetLossPercent: Double) {
		schema = 1
		self.timestampNanoseconds = timestampNanoseconds
		self.renderMilliseconds = renderMilliseconds
		self.encodeMilliseconds = encodeMilliseconds
		self.networkMilliseconds = networkMilliseconds
		self.decodeMilliseconds = decodeMilliseconds
		self.trackingAgeMilliseconds = trackingAgeMilliseconds
		self.bitrateMegabitsPerSecond = bitrateMegabitsPerSecond
		self.packetLossPercent = packetLossPercent
	}

	public func validate() -> Bool {
		let values = [renderMilliseconds, encodeMilliseconds, networkMilliseconds, decodeMilliseconds, trackingAgeMilliseconds, bitrateMegabitsPerSecond, packetLossPercent]
		return schema == 1 && values.allSatisfy { $0.isFinite && $0 >= 0 } && packetLossPercent <= 100
	}
}

public enum CompanionProtocolError: Error, Equatable {
	case unsupportedSchema
	case emptyKind
	case nonMonotonicSequence
}
