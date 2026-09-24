import Foundation
import CoreGraphics
import ImageIO
import PhotonStackAppCore
import Testing
@testable import PhotonStackProcessing

@Test func displayGridSettingsValidateRecordedModeAndAmount() {
    var settings = DisplayGridSettings()
    #expect(DisplayGridSettings(operationParameters: settings.operationParameters) == settings)
    for (key,value) in [("mode","unknown"),("amount","nan"),("amount","1.1"),("amount","-0.1")] {
        var parameters = settings.operationParameters; parameters[key] = value
        #expect(DisplayGridSettings(operationParameters: parameters) == nil)
    }
    #expect(DisplayGridSettings(operationParameters: ["mode":"display-grid-v1"]) == nil)
    settings.amount = 0
    #expect(settings.isValid)
    settings.amount = .infinity
    #expect(!settings.isValid)
}

@Test func displayGridServicePreservesExistingAndConcurrentOutputs() async throws {
    let directory = FileManager.default.temporaryDirectory.appendingPathComponent("PhotonStackGridOutput-\(UUID().uuidString)")
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer { try? FileManager.default.removeItem(at: directory) }
    let input = directory.appendingPathComponent("input.png")
    let context = try #require(CGContext(data: nil, width: 2, height: 2, bitsPerComponent: 8,
        bytesPerRow: 8, space: CGColorSpaceCreateDeviceRGB(), bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue))
    context.setFillColor(CGColor(red: 0.2, green: 0.4, blue: 0.6, alpha: 1))
    context.fill(CGRect(x: 0, y: 0, width: 2, height: 2))
    let destination = try #require(CGImageDestinationCreateWithURL(input as CFURL, "public.png" as CFString, 1, nil))
    CGImageDestinationAddImage(destination, try #require(context.makeImage()), nil)
    #expect(CGImageDestinationFinalize(destination))
    let original = try Data(contentsOf: input)
    let executable = directory.appendingPathComponent("photonstack")
    let output = directory.appendingPathComponent("output.png")
    let symbolic = directory.appendingPathComponent("link.png")
    let hard = directory.appendingPathComponent("hard.png")
    let dangling = directory.appendingPathComponent("dangling.png")
    try FileManager.default.createSymbolicLink(at: symbolic, withDestinationURL: input)
    try FileManager.default.linkItem(at: input, to: hard)
    try FileManager.default.createSymbolicLink(at: dangling, withDestinationURL: output)
    let service = CLIProcessingService(executableURL: executable)
    for url in [input,symbolic,hard,dangling] {
        do {
            _ = try await service.suppressDisplayGrid(input: input, output: url, settings: DisplayGridSettings())
            Issue.record("Existing output accepted")
        } catch let error as DisplayGridError { #expect(error == .outputAlreadyExists) }
    }
    // Write a destination after staging begins. Commit must refuse to replace it.
    let script = """
    #!/bin/sh
    while [ "$1" != "--output" ]; do shift; done
    shift
    cp '\(input.path)' "$1"
    printf 'concurrent output' > '\(output.path)'
    """
    try script.write(to: executable, atomically: true, encoding: .utf8)
    try FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: executable.path)
    do {
        _ = try await service.suppressDisplayGrid(input: input, output: output, settings: DisplayGridSettings())
        Issue.record("Concurrent output was overwritten")
    } catch let error as ProcessingServiceError {
        guard case .outputCommitFailed(let path, _) = error else { throw error }
        #expect(path == output.path)
    }
    #expect(try String(contentsOf: output, encoding: .utf8) == "concurrent output")
    #expect(try Data(contentsOf: input) == original)
    #expect(try FileManager.default.contentsOfDirectory(atPath: directory.path).allSatisfy { !$0.contains("photonstack-output-") })
}

@Test func displayGridServiceValidatesBeforeStagingAndInvokesExplicitCommand() async throws {
    let directory = FileManager.default.temporaryDirectory.appendingPathComponent("PhotonStackGrid-\(UUID().uuidString)")
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer { try? FileManager.default.removeItem(at: directory) }
    let executable = directory.appendingPathComponent("photonstack")
    let arguments = directory.appendingPathComponent("arguments.txt")
    try "#!/bin/sh\nprintf '%s\\n' \"$@\" > '\(arguments.path)'\nexit 0\n".write(to: executable, atomically: true, encoding: .utf8)
    try FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: executable.path)
    let service = CLIProcessingService(executableURL: executable)
    var bad = DisplayGridSettings(); bad.amount = .nan
    do {
        _ = try await service.suppressDisplayGrid(input: directory.appendingPathComponent("input.tiff"),
            output: directory.appendingPathComponent("out.tiff"), settings: bad)
        Issue.record("Invalid amount accepted")
    } catch let error as DisplayGridError { #expect(error == .invalidSettings) }
    for (i,o) in [("raw.FITS","out.tiff"),("input.tiff","out.fit"),("raw.dng","out.png")] {
        do {
            _ = try await service.suppressDisplayGrid(input: directory.appendingPathComponent(i),
                output: directory.appendingPathComponent(o), settings: DisplayGridSettings())
            Issue.record("Non-display path accepted")
        } catch let error as DisplayGridError { #expect(error == .displayImageRequired) }
    }
    #expect(!FileManager.default.fileExists(atPath: arguments.path))
    let output = directory.appendingPathComponent("out.tiff")
    do {
        _ = try await service.suppressDisplayGrid(input: directory.appendingPathComponent("input.tiff"),
            output: output, settings: DisplayGridSettings())
        Issue.record("Missing output accepted")
    } catch let error as ProcessingServiceError { #expect(error == .outputMissing(path: output.path)) }
    let args = try String(contentsOf: arguments, encoding: .utf8).split(separator: "\n").map(String.init)
    #expect(args.first == "suppress-grid")
    let index = try #require(args.firstIndex(of: "--amount"))
    #expect(args[index+1] == "1.0")
}
