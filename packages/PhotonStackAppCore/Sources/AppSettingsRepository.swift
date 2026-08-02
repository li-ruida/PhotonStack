import Foundation

public protocol AppSettingsStoring: Sendable {
    func load() async throws -> AppSettings
    func save(_ settings: AppSettings) async throws
}

public actor AppSettingsRepository: AppSettingsStoring {
    public static let settingsFileName = "settings.json"
    public static let maximumDocumentBytes: UInt64 = 16 * 1_024 * 1_024

    private let encoder: JSONEncoder
    private let decoder: JSONDecoder
    private let settingsURL: URL
    private let fileManager: FileManager
    private let maximumDocumentBytes: UInt64

    public init(
        settingsURL: URL? = nil,
        fileManager: FileManager = .default,
        maximumDocumentBytes: UInt64 = AppSettingsRepository.maximumDocumentBytes
    ) {
        self.fileManager = fileManager
        self.maximumDocumentBytes = maximumDocumentBytes
        self.encoder = JSONEncoder()
        self.decoder = JSONDecoder()
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        encoder.dateEncodingStrategy = .iso8601
        decoder.dateDecodingStrategy = .iso8601

        if let settingsURL {
            self.settingsURL = settingsURL
        } else {
            let supportDirectory = fileManager
                .urls(for: .applicationSupportDirectory, in: .userDomainMask)
                .first?
                .appendingPathComponent("PhotonStack", isDirectory: true)
                ?? fileManager.temporaryDirectory.appendingPathComponent("PhotonStack", isDirectory: true)
            self.settingsURL = supportDirectory.appendingPathComponent(Self.settingsFileName)
        }
    }

    public func load() throws -> AppSettings {
        guard fileManager.fileExists(atPath: settingsURL.path) else {
            return AppSettings()
        }
        let data = try BoundedDocumentIO.read(
            from: settingsURL,
            maximumBytes: maximumDocumentBytes,
            fileManager: fileManager
        )
        return try decoder.decode(AppSettings.self, from: data)
    }

    public func save(_ settings: AppSettings) throws {
        try fileManager.createDirectory(at: settingsURL.deletingLastPathComponent(), withIntermediateDirectories: true)
        let data = try encoder.encode(settings)
        try BoundedDocumentIO.requireFits(data, maximumBytes: maximumDocumentBytes)
        try data.write(to: settingsURL, options: .atomic)
    }
}
