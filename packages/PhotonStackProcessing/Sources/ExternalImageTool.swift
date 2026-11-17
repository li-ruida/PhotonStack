import Foundation

/// Versioned local image-tool adapter. Selecting/importing a definition never
/// executes it. Every job runs in its own working folder with staged output.
public struct ExternalImageTool: Codable, Equatable, Sendable {
    public var version: Int = 1
    public var name: String
    public var executable: URL
    public var arguments: [String]

    public init(name: String, executable: URL, arguments: [String]) {
        self.name = name
        self.executable = executable
        self.arguments = arguments
    }

    public func validate() throws {
        guard version == 1, !name.isEmpty, name.count <= 100, executable.isFileURL,
              executable.path.hasPrefix("/"), arguments.count <= 64,
              arguments.filter({ $0 == "{input}" }).count == 1,
              arguments.filter({ $0 == "{output}" }).count == 1,
              arguments.allSatisfy({ !$0.contains("\0") && $0.count < 4096 }),
              FileManager.default.isExecutableFile(atPath: executable.path)
        else { throw ProcessingServiceError.invalidArguments("Choose an installed executable and one {input} / {output} argument.") }
    }

    public static func load(from url: URL) throws -> Self {
        let attributes = try FileManager.default.attributesOfItem(atPath: url.path)
        guard (attributes[.size] as? NSNumber)?.intValue ?? Int.max <= 65_536 else {
            throw ProcessingServiceError.invalidArguments("External-tool definition exceeds 64 KiB.")
        }
        let value = try JSONDecoder().decode(Self.self, from: Data(contentsOf: url))
        try value.validate()
        return value
    }

    public func run(input: URL, output: URL, workingDirectory: URL) async throws -> ProcessingCommandResult {
        try validate()
        guard input.standardizedFileURL.resolvingSymlinksInPath() != output.standardizedFileURL.resolvingSymlinksInPath(),
              !FileManager.default.fileExists(atPath: output.path),
              let index = arguments.firstIndex(of: "{output}") else {
            throw ProcessingServiceError.invalidArguments("Choose a new external-tool output.")
        }
        let expanded = arguments.map { $0 == "{input}" ? input.path : $0 == "{output}" ? output.path : $0 }
        return try await CLIProcessingService(executableURL: executable, workingDirectoryURL: workingDirectory)
            .runExternalImage(arguments: expanded, outputIndex: index)
    }
}
