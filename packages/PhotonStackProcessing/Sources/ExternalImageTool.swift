import Foundation
import CryptoKit
import Darwin

/// Versioned local image-tool adapter. Selecting/importing a definition never
/// executes it. Every job runs in its own working folder with staged output.
public struct ExternalImageTool: Codable, Equatable, Sendable {
    public var version: Int = 1
    public var name: String
    public var executable: URL
    public var arguments: [String]
    /// Explicit adapter contract: normalized signed Float32 FITS in/out.
    public var linearFITS: Bool? = nil
    public var licenseFile: URL? = nil
    public var acceptedLicenseSHA256: String? = nil

    public init(name: String, executable: URL, arguments: [String], linearFITS: Bool? = nil) {
        self.name = name
        self.executable = executable
        self.arguments = arguments
        self.linearFITS = linearFITS
    }

    public static func license(at url: URL) throws -> (text: String, sha256: String) {
        let info = try url.resourceValues(forKeys: [.isRegularFileKey, .fileSizeKey])
        guard info.isRegularFile == true, let size = info.fileSize, size > 0, size <= 131_072 else {
            throw ProcessingServiceError.invalidArguments("Choose the complete license supplied with this tool version.")
        }
        let bytes = try Data(contentsOf: url)
        guard bytes.count <= 131_072, let text = String(data: bytes, encoding: .utf8) else {
            throw ProcessingServiceError.invalidArguments("The tool license must be a readable UTF-8 text file.")
        }
        return (text, SHA256.hash(data: bytes).map { String(format: "%02x", $0) }.joined())
    }

    public func validate() throws {
        if (linearFITS == true && name.localizedCaseInsensitiveContains("starnet")) || licenseFile != nil || acceptedLicenseSHA256 != nil {
            guard let licenseFile, let acceptedLicenseSHA256,
                  try Self.license(at: licenseFile).sha256 == acceptedLicenseSHA256 else {
                throw ProcessingServiceError.invalidArguments("Review and accept the license supplied with this tool version before running it. Changed terms require renewed acceptance.")
            }
        }
        guard version == 1, !name.isEmpty, name.count <= 100, executable.isFileURL,
              executable.path.hasPrefix("/"), arguments.count <= 64,
              arguments.filter({ $0 == "{input}" }).count == 1,
              arguments.filter({ $0 == "{output}" }).count == 1,
              arguments.allSatisfy({ !$0.contains("\0") && $0.count < 4096 }),
              FileManager.default.isExecutableFile(atPath: executable.path)
        else { throw ProcessingServiceError.invalidArguments("Choose an installed executable and one {input} / {output} argument.") }
    }

    public static func load(from url: URL) throws -> Self {
        let limit = 64 * 1024
        let descriptor = url.withUnsafeFileSystemRepresentation { path in
            path.map { Darwin.open($0, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK) } ?? -1
        }
        guard descriptor >= 0 else { throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO) }
        let handle = FileHandle(fileDescriptor: descriptor, closeOnDealloc: true)
        defer { try? handle.close() }
        var info = stat()
        guard fstat(descriptor, &info) == 0, info.st_mode & S_IFMT == S_IFREG else {
            throw ProcessingServiceError.invalidArguments("Choose a regular external-tool definition file.")
        }
        guard info.st_size <= limit else {
            throw ProcessingServiceError.invalidArguments("External-tool definition exceeds 64 KiB.")
        }
        var data = Data()
        while let chunk = try handle.read(upToCount: limit + 1 - data.count), !chunk.isEmpty {
            data.append(chunk)
            guard data.count <= limit else {
                throw ProcessingServiceError.invalidArguments("External-tool definition exceeds 64 KiB.")
            }
        }
        let value = try JSONDecoder().decode(Self.self, from: data)
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
