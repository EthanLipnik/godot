import FoveatedStreaming
import SwiftUI

@available(visionOS 27.0, *)
@main
struct RemoteXRCompanionApp: App {
	@State private var session = FoveatedStreamingSession()
	@State private var errorMessage: String?

	var body: some Scene {
		WindowGroup {
			VStack(spacing: 16) {
				Text("Remote XR")
				Text(session.status.description)
				if let errorMessage { Text(errorMessage) }
				Button("Connect") {
					Task { await connect() }
				}
				Button("Disconnect") {
					Task { await session.disconnect() }
				}
			}
			.padding()
		}
		ImmersiveSpace(foveatedStreaming: session)
	}

	private func connect() async {
		session.requestedInputCapabilities = [.handTracking]
		let authorization = await session.requestAuthorization()
		guard authorization[.handTracking] == .authorized else {
			errorMessage = "Hand tracking authorization is required."
			return
		}
		do {
			try await session.connect(endpoint: .systemDiscovered)
			errorMessage = nil
		} catch {
			errorMessage = error.localizedDescription
		}
	}
}
