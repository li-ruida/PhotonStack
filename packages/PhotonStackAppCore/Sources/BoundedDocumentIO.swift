import Foundation

public enum StructuredDocumentError: LocalizedError, Equatable {
    case fileTooLarge(maximumBytes: UInt64)

    public var errorDescription: String? {
        switch self {
        case let .fileTooLarge(maximumBytes):
            return "Document exceeds the \(Self.formattedByteCount(maximumBytes)) size limit"
        }
    }

    private static func formattedByteCount(_ bytes: UInt64) -> String {
        guard bytes.isMultiple(of: 1_024 * 1_024) else {
        return "\(bytes) bytes"
        }
        return "\(bytes / (1_024 * 1_024)) MiB"
    }
}

enum BoundedDocumentIO {
    private static let readChunkBytes = 64 * 1_024

    static func read(
        from url: URL,
        maximumBytes: UInt64,
        fileManager: FileManager
    ) throws -> Data {
        let attributes = try fileManager.attributesOfItem(atPath: url.path)
        let reportedSize = (attributes[.size] as? NSNumber)?.uint64Value ?? 0
        guard reportedSize <= maximumBytes else {
            throw StructuredDocumentError.fileTooLarge(maximumBytes: maximumBytes)
        }

        let handle = try FileHandle(forReadingFrom: url)
        defer {
            try? handle.close()
        }

        var data = Data()
        if reportedSize > 0, reportedSize <= UInt64(Int.max) {
            data.reserveCapacity(Int(reportedSize))
        }
        while true {
            let remaining = maximumBytes - UInt64(data.count)
            let requestedBytes = remaining >= UInt64(readChunkBytes)
                ? readChunkBytes
                : Int(remaining) + 1
            guard requestedBytes > 0,
                  let chunk = try handle.read(upToCount: requestedBytes),
                  chunk.isEmpty == false
            else {
                break
            }
            data.append(chunk)
            guard UInt64(data.count) <= maximumBytes else {
                throw StructuredDocumentError.fileTooLarge(maximumBytes: maximumBytes)
            }
        }
        return data
    }

    static func requireFits(_ data: Data, maximumBytes: UInt64) throws {
        guard UInt64(data.count) <= maximumBytes else {
            throw StructuredDocumentError.fileTooLarge(maximumBytes: maximumBytes)
        }
    }
}
