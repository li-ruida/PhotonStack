import Foundation
#if canImport(Darwin)
import Darwin
#endif

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
    private let compactEncoder: JSONEncoder
    private let decoder: JSONDecoder
    private let fileManager: FileManager
    private let maximumDocumentBytes: UInt64

    #if canImport(Darwin)
    private struct AssetIdentity {
        let parent: String
        let inode: ino_t
        let device: dev_t
        let changed: timespec
        let path: String
    }
    private var lastAssets: [String: AssetIdentity] = [:]

    private func autosaveIdentities(_ assets: [PhotonStackAsset]) -> [String] {
        var parents: [String: String] = [:], next: [String: AssetIdentity] = [:]
        let paths = assets.map { asset -> String in
            let url = asset.originalURL.standardizedFileURL
            let path = url.path
            var st = stat()
            // Resolve aliases and missing leaves every time. A symlink further
            // down a link chain may change without changing the first link.
            guard lstat(path, &st) == 0, st.st_mode & S_IFMT == S_IFREG else {
                return PhotonStackProject.assetIdentityPath(for: url)
            }
            let directory = url.deletingLastPathComponent()
            let parent = parents[directory.path] ?? PhotonStackProject.assetIdentityPath(for: directory)
            parents[directory.path] = parent
            if let entry = lastAssets[path], entry.parent == parent, entry.inode == st.st_ino, entry.device == st.st_dev,
               entry.changed.tv_sec == st.st_ctimespec.tv_sec, entry.changed.tv_nsec == st.st_ctimespec.tv_nsec {
                next[path] = entry
                return entry.path
            }
            let canonical = PhotonStackProject.assetIdentityPath(for: url)
            next[path] = AssetIdentity(parent: parent, inode: st.st_ino, device: st.st_dev, changed: st.st_ctimespec, path: canonical)
            return canonical
        }
        lastAssets = next
        return paths
    }
    #endif

    public init(
        fileManager: FileManager = .default,
        maximumDocumentBytes: UInt64 = ProjectRepository.maximumDocumentBytes
    ) {
        self.fileManager = fileManager
        self.maximumDocumentBytes = maximumDocumentBytes
        self.encoder = JSONEncoder()
        self.compactEncoder = JSONEncoder()
        self.decoder = JSONDecoder()
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        encoder.dateEncodingStrategy = .iso8601
        compactEncoder.dateEncodingStrategy = .iso8601
        decoder.dateDecodingStrategy = .iso8601
    }

    public func save(_ project: PhotonStackProject, to directory: URL) throws {
        try write(project, to: directory, using: encoder)
    }

    public func autosave(_ project: PhotonStackProject, to directory: URL) throws {
        #if canImport(Darwin)
        try write(project, to: directory, using: compactEncoder, paths: autosaveIdentities(project.assets))
        #else
        try write(project, to: directory, using: compactEncoder)
        #endif
    }

    private func write(_ project: PhotonStackProject, to directory: URL, using encoder: JSONEncoder, paths: [String]? = nil) throws {
        try Self.validateIdentityUniqueness(in: project, paths: paths)
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

    private static func validateIdentityUniqueness(in project: PhotonStackProject, paths: [String]? = nil) throws {
        try requireUnique(project.assets.map(\.id), collection: "assets")
        try requireUnique(
            paths ?? project.assets.map { PhotonStackProject.assetIdentityPath(for: $0.originalURL) },
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
