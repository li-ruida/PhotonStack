import Foundation

public enum ProjectRepositoryError: LocalizedError, Equatable {
    case duplicateIdentifier(collection: String)

    public var errorDescription: String? {
        switch self {
        case let .duplicateIdentifier(collection):
            return "Project contains duplicate identifiers in \(collection)"
        }
    }
}

public actor ProjectRepository {
    public static let projectFileName = "project.json"
    public static let maximumDocumentBytes: UInt64 = 256 * 1_024 * 1_024

    private let encoder: JSONEncoder
    private let decoder: JSONDecoder
    private let fileManager: FileManager
    private let maximumDocumentBytes: UInt64

    public init(
        fileManager: FileManager = .default,
        maximumDocumentBytes: UInt64 = ProjectRepository.maximumDocumentBytes
    ) {
        self.fileManager = fileManager
        self.maximumDocumentBytes = maximumDocumentBytes
        self.encoder = JSONEncoder()
        self.decoder = JSONDecoder()
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        encoder.dateEncodingStrategy = .iso8601
        decoder.dateDecodingStrategy = .iso8601
    }

    public func save(_ project: PhotonStackProject, to directory: URL) throws {
        try Self.validateIdentityUniqueness(in: project)
        try fileManager.createDirectory(at: directory, withIntermediateDirectories: true)
        let data = try encoder.encode(project)
        try BoundedDocumentIO.requireFits(data, maximumBytes: maximumDocumentBytes)
        try data.write(to: directory.appendingPathComponent(Self.projectFileName), options: .atomic)
    }

    public func load(from directory: URL) throws -> PhotonStackProject {
        let data = try BoundedDocumentIO.read(
            from: directory.appendingPathComponent(Self.projectFileName),
            maximumBytes: maximumDocumentBytes,
            fileManager: fileManager
        )
        let project = try decoder.decode(PhotonStackProject.self, from: data)
        try Self.validateIdentityUniqueness(in: project)
        return project
    }

    private static func validateIdentityUniqueness(in project: PhotonStackProject) throws {
        try requireUnique(project.assets.map(\.id), collection: "assets")
        try requireUnique(
            project.assets.map { PhotonStackProject.assetIdentityPath(for: $0.originalURL) },
            collection: "asset locations"
        )
        try requireUnique(project.editGraph.operations.map(\.id), collection: "edit operations")
        try requireUnique(project.artifacts.map(\.id), collection: "artifacts")
        try requireUnique(project.layers.map(\.id), collection: "layers")
        try requireUnique(project.batchQueue.map(\.id), collection: "batch queue items")
        for artifact in project.artifacts {
            try requireUnique(
                artifact.frames.map(\.id),
                collection: "frames of artifact \(artifact.name)"
            )
        }
    }

    private static func requireUnique<ID: Hashable>(
        _ identifiers: [ID],
        collection: String
    ) throws {
        guard Set(identifiers).count == identifiers.count else {
            throw ProjectRepositoryError.duplicateIdentifier(collection: collection)
        }
    }
}
