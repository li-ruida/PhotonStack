import Foundation

enum WorkspaceExportProtection {
    enum Failure: LocalizedError {
        case protectedSource(String)
        var errorDescription: String? {
            switch self {
            case let .protectedSource(path):
                "不能覆盖项目原片或处理依赖，请选择新的导出位置。Export cannot overwrite a project source or processing dependency: \(path)"
            }
        }
    }

    static func validate(_ destination: URL, sources: [URL]) throws {
        let target = destination.standardizedFileURL.resolvingSymlinksInPath()
        let targetIdentity = fileIdentity(target)
        for source in sources {
            let resolved = source.standardizedFileURL.resolvingSymlinksInPath()
            if target.path == resolved.path || destination.standardizedFileURL.path == source.standardizedFileURL.path {
                throw Failure.protectedSource(source.path)
            }
            if let targetIdentity, fileIdentity(resolved) == targetIdentity {
                throw Failure.protectedSource(source.path)
            }
        }
    }

    private struct FileIdentity: Equatable {
        let device: UInt64
        let inode: UInt64
    }

    private static func fileIdentity(_ url: URL) -> FileIdentity? {
        guard let attributes = try? FileManager.default.attributesOfItem(atPath: url.path),
              let device = attributes[.systemNumber] as? NSNumber,
              let inode = attributes[.systemFileNumber] as? NSNumber else { return nil }
        return FileIdentity(device: device.uint64Value, inode: inode.uint64Value)
    }
}
