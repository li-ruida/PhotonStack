import Foundation

public enum WorkspaceCanvasMode: String, Codable, Equatable, Sendable {
    case sourcePreview
    case layerComposite
}

public struct ProjectWorkspaceState: Codable, Equatable, Sendable {
    public var selectedAssetID: UUID?
    public var activeLayerID: UUID?
    public var previewURL: URL?
    public var previewOperationID: UUID?
    public var editGraphNeedsReplay: Bool?
    public var canvasMode: WorkspaceCanvasMode?

    public init(
        selectedAssetID: UUID? = nil,
        activeLayerID: UUID? = nil,
        previewURL: URL? = nil,
        previewOperationID: UUID? = nil,
        editGraphNeedsReplay: Bool? = nil,
        canvasMode: WorkspaceCanvasMode? = nil
    ) {
        self.selectedAssetID = selectedAssetID
        self.activeLayerID = activeLayerID
        self.previewURL = previewURL
        self.previewOperationID = previewOperationID
        self.editGraphNeedsReplay = editGraphNeedsReplay
        self.canvasMode = canvasMode
    }
}

public struct PhotonStackProject: Identifiable, Codable, Equatable, Sendable {
    public var id: UUID
    public var name: String
    public var template: ProjectTemplate?
    public var assets: [PhotonStackAsset]
    public var editGraph: EditGraph
    public var artifacts: [ProcessingArtifact]
    public var layers: [ProcessingLayer]
    public var mosaicPanelOrder: [UUID]
    public var batchQueue: [BatchQueueItem]
    public var batchOutputDirectory: URL?
    public var batchOutputFormat: BatchOutputFormat
    public var batchMaxConcurrentTasks: Int
    public var workspaceState: ProjectWorkspaceState?
    public var createdAt: Date
    public var updatedAt: Date

    public init(
        id: UUID = UUID(),
        name: String = "Untitled PhotonStack Project",
        template: ProjectTemplate? = nil,
        assets: [PhotonStackAsset] = [],
        editGraph: EditGraph = EditGraph(),
        artifacts: [ProcessingArtifact] = [],
        layers: [ProcessingLayer] = [],
        mosaicPanelOrder: [UUID] = [],
        batchQueue: [BatchQueueItem] = [],
        batchOutputDirectory: URL? = nil,
        batchOutputFormat: BatchOutputFormat = .png,
        batchMaxConcurrentTasks: Int = 1,
        workspaceState: ProjectWorkspaceState? = nil,
        createdAt: Date = Date(),
        updatedAt: Date = Date()
    ) {
        self.id = id
        self.name = name
        self.template = template
        self.assets = assets
        self.editGraph = editGraph
        self.artifacts = artifacts
        self.layers = layers
        self.mosaicPanelOrder = mosaicPanelOrder
        self.batchQueue = batchQueue
        self.batchOutputDirectory = batchOutputDirectory
        self.batchOutputFormat = batchOutputFormat
        self.batchMaxConcurrentTasks = max(1, min(batchMaxConcurrentTasks, 4))
        self.workspaceState = workspaceState
        self.createdAt = createdAt
        self.updatedAt = updatedAt
    }

    enum CodingKeys: String, CodingKey {
        case id
        case name
        case template
        case assets
        case editGraph
        case artifacts
        case layers
        case mosaicPanelOrder
        case batchQueue
        case batchOutputDirectory
        case batchOutputFormat
        case batchMaxConcurrentTasks
        case workspaceState
        case createdAt
        case updatedAt
    }

    public init(from decoder: Decoder) throws {
        let container = try decoder.container(keyedBy: CodingKeys.self)
        id = try container.decode(UUID.self, forKey: .id)
        name = try container.decode(String.self, forKey: .name)
        template = try container.decodeIfPresent(ProjectTemplate.self, forKey: .template)
        assets = try container.decode([PhotonStackAsset].self, forKey: .assets)
        editGraph = try container.decodeIfPresent(EditGraph.self, forKey: .editGraph) ?? EditGraph()
        artifacts = try container.decodeIfPresent([ProcessingArtifact].self, forKey: .artifacts) ?? []
        layers = try container.decodeIfPresent([ProcessingLayer].self, forKey: .layers) ?? []
        mosaicPanelOrder = try container.decodeIfPresent([UUID].self, forKey: .mosaicPanelOrder) ?? []
        batchQueue = try container.decodeIfPresent([BatchQueueItem].self, forKey: .batchQueue) ?? []
        batchOutputDirectory = try container.decodeIfPresent(URL.self, forKey: .batchOutputDirectory)
        batchOutputFormat = try container.decodeIfPresent(BatchOutputFormat.self, forKey: .batchOutputFormat) ?? .png
        batchMaxConcurrentTasks = max(1, min(try container.decodeIfPresent(Int.self, forKey: .batchMaxConcurrentTasks) ?? 1, 4))
        workspaceState = try container.decodeIfPresent(ProjectWorkspaceState.self, forKey: .workspaceState)
        createdAt = try container.decode(Date.self, forKey: .createdAt)
        updatedAt = try container.decode(Date.self, forKey: .updatedAt)
    }

    public mutating func addAssets(
        from urls: [URL],
        role: CalibrationFrameRole = .light
    ) {
        var existing = Set(assets.map { Self.assetIdentityPath(for: $0.originalURL) })
        let newAssets = urls.compactMap { url -> PhotonStackAsset? in
            let identityPath = Self.assetIdentityPath(for: url)
            guard existing.insert(identityPath).inserted else {
                return nil
            }
            return PhotonStackAsset(
                originalURL: url,
                kind: AssetKind.detect(from: url),
                role: role
            )
        }

        assets.append(contentsOf: newAssets)
        if role == .mosaic {
            mosaicPanelOrder.append(contentsOf: newAssets.map(\.id))
        }
        updatedAt = Date()
    }

    public static func assetIdentityPath(for url: URL) -> String {
        url.standardizedFileURL.resolvingSymlinksInPath().path
    }

    public mutating func updateMetadata(for assetID: UUID, metadata: AssetMetadata) {
        guard let index = assets.firstIndex(where: { $0.id == assetID }) else {
            return
        }
        assets[index].metadata = metadata
        updatedAt = Date()
    }

    public mutating func removeAsset(_ assetID: UUID) {
        guard let removedAsset = assets.first(where: { $0.id == assetID }) else {
            return
        }
        let assetIDText = assetID.uuidString
        let removedPath = removedAsset.originalURL.path

        var removedOperationIDs: Set<EditOperation.ID> = []
        var invalidatedPaths: Set<String> = [Self.assetIdentityPath(for: removedAsset.originalURL)]
        var foundOperation = true
        while foundOperation {
            foundOperation = false
            for operation in editGraph.operations where removedOperationIDs.contains(operation.id) == false {
                let directlyReferencesAsset = Self.valuesReferenceAsset(
                    operation.parameters,
                    assetID: assetIDText,
                    path: removedPath
                )
                let dependsOnRemovedOutput = operation.parameters.contains { key, value in
                    key != "output" && key != "previewOutput" &&
                        Self.pathSet(invalidatedPaths, containsStoredPath: value)
                }
                guard directlyReferencesAsset || dependsOnRemovedOutput else {
                    continue
                }
                removedOperationIDs.insert(operation.id)
                for key in ["output", "previewOutput"] {
                    if let path = operation.parameters[key] {
                        Self.insertStoredPath(path, into: &invalidatedPaths)
                    }
                }
                foundOperation = true
            }
        }

        var invalidatedArtifactIDs: Set<ProcessingArtifact.ID> = []
        var removedArtifactIDs: Set<ProcessingArtifact.ID> = []
        for index in artifacts.indices {
            if artifacts[index].kind == .rawSequence {
                let previousFrameCount = artifacts[index].frames.count
                let previousOutputCount = artifacts[index].outputURLs.count
                artifacts[index].frames.removeAll { frame in
                    Self.frameReferencesAsset(frame, assetID: assetIDText, url: removedAsset.originalURL)
                }
                artifacts[index].outputURLs.removeAll {
                    Self.urlsReferenceSameFile($0, removedAsset.originalURL)
                }
                if artifacts[index].frames.count != previousFrameCount ||
                    artifacts[index].outputURLs.count != previousOutputCount {
                    invalidatedArtifactIDs.insert(artifacts[index].id)
                    Self.refreshSourceSequenceSummary(&artifacts[index])
                    artifacts[index].updatedAt = Date()
                }
                if artifacts[index].frameCount == 0 {
                    removedArtifactIDs.insert(artifacts[index].id)
                }
                continue
            }

            let referencesAsset = Self.valuesReferenceAsset(
                artifacts[index].parameters,
                assetID: assetIDText,
                path: removedPath
            ) || Self.valuesReferenceAsset(
                artifacts[index].metrics,
                assetID: assetIDText,
                path: removedPath
            ) || artifacts[index].outputURLs.contains(where: {
                Self.urlsReferenceSameFile($0, removedAsset.originalURL)
            }) || artifacts[index].frames.contains {
                Self.frameReferencesAsset($0, assetID: assetIDText, url: removedAsset.originalURL)
            }
            let referencesRemovedOutput = artifacts[index].parameters.values.contains {
                Self.pathSet(invalidatedPaths, containsStoredPath: $0)
            } || artifacts[index].metrics.values.contains {
                Self.pathSet(invalidatedPaths, containsStoredPath: $0)
            } ||
                artifacts[index].frames.contains { frame in
                    frame.sourceURL.map { Self.pathSet(invalidatedPaths, contains: $0) } == true
                }
            if referencesAsset || referencesRemovedOutput {
                removedArtifactIDs.insert(artifacts[index].id)
            }
        }

        for artifact in artifacts where removedArtifactIDs.contains(artifact.id) {
            for url in artifact.outputURLs {
                invalidatedPaths.insert(Self.assetIdentityPath(for: url))
            }
            for frame in artifact.frames {
                if let outputURL = frame.outputURL {
                    invalidatedPaths.insert(Self.assetIdentityPath(for: outputURL))
                }
            }
        }

        var foundArtifact = true
        while foundArtifact {
            foundArtifact = false
            for artifact in artifacts where removedArtifactIDs.contains(artifact.id) == false {
                let referencesRemovedSource = artifact.sourceArtifactIDs.contains(where: removedArtifactIDs.contains)
                let referencesChangedSource = artifact.sourceArtifactIDs.contains(where: invalidatedArtifactIDs.contains)
                let referencesInvalidSource = referencesRemovedSource ||
                    (referencesChangedSource && Self.artifactRecordsSourceAssets(artifact) == false)
                let referencesInvalidPath = artifact.parameters.values.contains {
                    Self.pathSet(invalidatedPaths, containsStoredPath: $0)
                } || artifact.metrics.values.contains {
                    Self.pathSet(invalidatedPaths, containsStoredPath: $0)
                } ||
                    artifact.frames.contains { frame in
                        frame.sourceURL.map { Self.pathSet(invalidatedPaths, contains: $0) } == true
                    }
                guard referencesInvalidSource || referencesInvalidPath else {
                    continue
                }
                removedArtifactIDs.insert(artifact.id)
                for url in artifact.outputURLs {
                    invalidatedPaths.insert(Self.assetIdentityPath(for: url))
                }
                for frame in artifact.frames {
                    if let outputURL = frame.outputURL {
                        invalidatedPaths.insert(Self.assetIdentityPath(for: outputURL))
                    }
                }
                foundArtifact = true
            }
        }
        for artifact in artifacts where removedArtifactIDs.contains(artifact.id) {
            for url in artifact.outputURLs {
                invalidatedPaths.insert(Self.assetIdentityPath(for: url))
            }
            for frame in artifact.frames {
                if let outputURL = frame.outputURL {
                    invalidatedPaths.insert(Self.assetIdentityPath(for: outputURL))
                }
            }
        }

        var removedLayerIDs = Set(layers.compactMap { layer -> ProcessingLayer.ID? in
            let referencesAsset = layer.parameters["assetID"] == assetIDText ||
                layer.inputURL.map { Self.urlsReferenceSameFile($0, removedAsset.originalURL) } == true
            let referencesRemovedOperation = layer.parameters["operationID"]
                .flatMap(UUID.init(uuidString:))
                .map(removedOperationIDs.contains) == true
            let referencesRemovedArtifact = layer.sourceArtifactID.map(removedArtifactIDs.contains) == true
            let referencesRemovedOutput = layer.inputURL.map {
                Self.pathSet(invalidatedPaths, contains: $0)
            } == true
            return referencesAsset || referencesRemovedOperation || referencesRemovedArtifact || referencesRemovedOutput
                ? layer.id
                : nil
        })
        var foundLayer = true
        while foundLayer {
            foundLayer = false
            for layer in layers where removedLayerIDs.contains(layer.id) == false {
                let referencesRemovedLayer = layer.parameters["sourceLayerID"]
                    .flatMap(UUID.init(uuidString:))
                    .map(removedLayerIDs.contains) == true
                let referencesInvalidPath = layer.inputURL.map {
                    Self.pathSet(invalidatedPaths, contains: $0)
                } == true
                guard referencesRemovedLayer || referencesInvalidPath else {
                    continue
                }
                removedLayerIDs.insert(layer.id)
                if let inputURL = layer.inputURL {
                    invalidatedPaths.insert(Self.assetIdentityPath(for: inputURL))
                }
                foundLayer = true
            }
        }
        for layer in layers where removedLayerIDs.contains(layer.id) {
            if let inputURL = layer.inputURL {
                invalidatedPaths.insert(Self.assetIdentityPath(for: inputURL))
            }
        }

        assets.removeAll { $0.id == assetID }
        mosaicPanelOrder.removeAll { $0 == assetID }
        editGraph.operations.removeAll { removedOperationIDs.contains($0.id) }
        artifacts.removeAll { removedArtifactIDs.contains($0.id) }
        layers.removeAll { removedLayerIDs.contains($0.id) }
        for index in layers.indices where layers[index].maskArtifactID.map(removedArtifactIDs.contains) == true {
            layers[index].maskArtifactID = nil
        }
        batchQueue.removeAll { item in
            Self.pathSet(invalidatedPaths, contains: item.inputURL) ||
                item.outputURL.map { Self.pathSet(invalidatedPaths, contains: $0) } == true
        }

        if workspaceState?.selectedAssetID == assetID {
            workspaceState?.selectedAssetID = assets.first?.id
        }
        if let activeLayerID = workspaceState?.activeLayerID,
           removedLayerIDs.contains(activeLayerID) {
            workspaceState?.activeLayerID = nil
        }
        let invalidPreview = workspaceState?.previewURL.map {
            Self.pathSet(invalidatedPaths, contains: $0)
        } == true
        let invalidPreviewOperation = workspaceState?.previewOperationID.map(removedOperationIDs.contains) == true
        if invalidPreview || invalidPreviewOperation {
            let fallbackPreviewURL = workspaceState?.selectedAssetID
                .flatMap { selectedID in assets.first { $0.id == selectedID }?.originalURL }
                ?? assets.first?.originalURL
            workspaceState?.previewURL = fallbackPreviewURL
            workspaceState?.previewOperationID = nil
            workspaceState?.editGraphNeedsReplay = false
        }
        updatedAt = Date()
    }

    public mutating func removeArtifact(_ artifactID: UUID) {
        guard let removedArtifact = artifacts.first(where: { $0.id == artifactID }) else {
            return
        }

        var removedArtifactIDs: Set<ProcessingArtifact.ID> = [removedArtifact.id]
        var invalidatedPaths = Set(removedArtifact.outputURLs.map(Self.assetIdentityPath(for:)))
        invalidatedPaths.formUnion(
            removedArtifact.frames.compactMap { $0.outputURL }.map(Self.assetIdentityPath(for:))
        )

        var foundArtifact = true
        while foundArtifact {
            foundArtifact = false
            for artifact in artifacts where removedArtifactIDs.contains(artifact.id) == false {
                let referencesRemovedArtifact = artifact.sourceArtifactIDs.contains(where: removedArtifactIDs.contains)
                let referencesRemovedOutput = artifact.parameters.values.contains {
                    Self.pathSet(invalidatedPaths, containsStoredPath: $0)
                } || artifact.metrics.values.contains {
                    Self.pathSet(invalidatedPaths, containsStoredPath: $0)
                } ||
                    artifact.frames.contains { frame in
                        frame.sourceURL.map { Self.pathSet(invalidatedPaths, contains: $0) } == true
                    }
                guard referencesRemovedArtifact || referencesRemovedOutput else {
                    continue
                }
                removedArtifactIDs.insert(artifact.id)
                invalidatedPaths.formUnion(artifact.outputURLs.map(Self.assetIdentityPath(for:)))
                invalidatedPaths.formUnion(
                    artifact.frames.compactMap { $0.outputURL }.map(Self.assetIdentityPath(for:))
                )
                foundArtifact = true
            }
        }

        var removedLayerIDs = Set(layers.compactMap { layer -> ProcessingLayer.ID? in
            let referencesRemovedArtifact = layer.sourceArtifactID.map(removedArtifactIDs.contains) == true
            let referencesRemovedOutput = layer.inputURL.map {
                Self.pathSet(invalidatedPaths, contains: $0)
            } == true
            return referencesRemovedArtifact || referencesRemovedOutput ? layer.id : nil
        })
        var foundLayer = true
        while foundLayer {
            foundLayer = false
            for layer in layers where removedLayerIDs.contains(layer.id) == false {
                let referencesRemovedLayer = layer.parameters["sourceLayerID"]
                    .flatMap(UUID.init(uuidString:))
                    .map(removedLayerIDs.contains) == true
                guard referencesRemovedLayer else {
                    continue
                }
                removedLayerIDs.insert(layer.id)
                foundLayer = true
            }
        }

        artifacts.removeAll { removedArtifactIDs.contains($0.id) }
        layers.removeAll { removedLayerIDs.contains($0.id) }
        for index in layers.indices where layers[index].maskArtifactID.map(removedArtifactIDs.contains) == true {
            layers[index].maskArtifactID = nil
        }
        batchQueue.removeAll { item in
            Self.pathSet(invalidatedPaths, contains: item.inputURL) ||
                item.outputURL.map { Self.pathSet(invalidatedPaths, contains: $0) } == true
        }

        if let activeLayerID = workspaceState?.activeLayerID,
           removedLayerIDs.contains(activeLayerID) {
            workspaceState?.activeLayerID = nil
        }
        if workspaceState?.previewURL.map({ Self.pathSet(invalidatedPaths, contains: $0) }) == true {
            let fallbackPreviewURL = workspaceState?.selectedAssetID
                .flatMap { selectedID in assets.first { $0.id == selectedID }?.originalURL }
                ?? assets.first?.originalURL
            workspaceState?.previewURL = fallbackPreviewURL
            workspaceState?.previewOperationID = nil
            workspaceState?.editGraphNeedsReplay = false
        }
        updatedAt = Date()
    }

    private static func valuesReferenceAsset(
        _ values: [String: String],
        assetID: String,
        path: String
    ) -> Bool {
        values.contains { _, value in
            storedPath(value, references: path) || value == assetID || value
                .split(separator: ",", omittingEmptySubsequences: true)
                .contains { $0.trimmingCharacters(in: .whitespacesAndNewlines) == assetID }
        }
    }

    private static func frameReferencesAsset(
        _ frame: ProcessingArtifactFrame,
        assetID: String,
        url: URL
    ) -> Bool {
        frame.sourceURL.map { urlsReferenceSameFile($0, url) } == true ||
            frame.outputURL.map { urlsReferenceSameFile($0, url) } == true ||
            valuesReferenceAsset(frame.metrics, assetID: assetID, path: url.path)
    }

    @discardableResult
    public mutating func relinkAsset(_ assetID: UUID, to replacementURL: URL) -> Bool {
        guard let assetIndex = assets.firstIndex(where: { $0.id == assetID }) else {
            return false
        }

        let oldURL = assets[assetIndex].originalURL
        let replacementIdentityPath = Self.assetIdentityPath(for: replacementURL)
        if Self.assetIdentityPath(for: oldURL) == replacementIdentityPath {
            return true
        }
        guard AssetKind.detect(from: replacementURL) != .unknown,
              assets.contains(where: {
                  $0.id != assetID &&
                      Self.assetIdentityPath(for: $0.originalURL) == replacementIdentityPath
              }) == false
        else {
            return false
        }

        let oldPath = oldURL.path
        let replacementPath = replacementURL.path
        let oldName = oldURL.lastPathComponent
        let replacementName = replacementURL.lastPathComponent

        assets[assetIndex].originalURL = replacementURL
        assets[assetIndex].kind = AssetKind.detect(from: replacementURL)
        assets[assetIndex].metadata = nil
        let replacementKind = assets[assetIndex].kind

        for index in layers.indices {
            if layers[index].inputURL.map({ Self.urlsReferenceSameFile($0, oldURL) }) == true {
                layers[index].inputURL = replacementURL
            }
            if layers[index].name == oldName,
               layers[index].parameters["assetID"] == assetID.uuidString {
                layers[index].name = replacementName
            }
            layers[index].parameters = Self.replacingExactPath(
                in: layers[index].parameters,
                oldPath: oldPath,
                replacementPath: replacementPath
            )
        }

        for index in artifacts.indices {
            let referencesAsset = artifacts[index].outputURLs.contains {
                Self.urlsReferenceSameFile($0, oldURL)
            } || artifacts[index].parameters.values.contains {
                Self.storedPath($0, references: oldPath)
            } || artifacts[index].metrics.values.contains {
                Self.storedPath($0, references: oldPath)
            } ||
                artifacts[index].frames.contains { frame in
                    Self.frameReferencesAsset(frame, assetID: assetID.uuidString, url: oldURL)
                }
            guard referencesAsset else {
                continue
            }
            if artifacts[index].name == oldName {
                artifacts[index].name = replacementName
            }
            artifacts[index].outputURLs = artifacts[index].outputURLs.map { url in
                Self.urlsReferenceSameFile(url, oldURL) ? replacementURL : url
            }
            artifacts[index].parameters = Self.replacingExactPath(
                in: artifacts[index].parameters,
                oldPath: oldPath,
                replacementPath: replacementPath
            )
            artifacts[index].metrics = Self.replacingExactPath(
                in: artifacts[index].metrics,
                oldPath: oldPath,
                replacementPath: replacementPath
            )
            for frameIndex in artifacts[index].frames.indices {
                let frameReferencesAsset = Self.frameReferencesAsset(
                    artifacts[index].frames[frameIndex],
                    assetID: assetID.uuidString,
                    url: oldURL
                )
                if artifacts[index].frames[frameIndex].sourceURL.map({
                    Self.urlsReferenceSameFile($0, oldURL)
                }) == true {
                    artifacts[index].frames[frameIndex].sourceURL = replacementURL
                }
                if artifacts[index].frames[frameIndex].outputURL.map({
                    Self.urlsReferenceSameFile($0, oldURL)
                }) == true {
                    artifacts[index].frames[frameIndex].outputURL = replacementURL
                }
                artifacts[index].frames[frameIndex].metrics = Self.replacingExactPath(
                    in: artifacts[index].frames[frameIndex].metrics,
                    oldPath: oldPath,
                    replacementPath: replacementPath
                )
                if artifacts[index].kind == .rawSequence, frameReferencesAsset {
                    artifacts[index].frames[frameIndex].metrics["kind"] = replacementKind.rawValue
                }
            }
            if artifacts[index].kind == .rawSequence {
                Self.refreshSourceSequenceSummary(&artifacts[index])
            }
            artifacts[index].updatedAt = Date()
        }

        for index in editGraph.operations.indices {
            editGraph.operations[index].parameters = Self.replacingExactPath(
                in: editGraph.operations[index].parameters,
                oldPath: oldPath,
                replacementPath: replacementPath
            )
        }

        for index in batchQueue.indices {
            if Self.urlsReferenceSameFile(batchQueue[index].inputURL, oldURL) {
                batchQueue[index].inputURL = replacementURL
            }
            if batchQueue[index].outputURL.map({ Self.urlsReferenceSameFile($0, oldURL) }) == true {
                batchQueue[index].outputURL = replacementURL
            }
        }

        if workspaceState?.previewURL.map({ Self.urlsReferenceSameFile($0, oldURL) }) == true {
            workspaceState?.previewURL = replacementURL
        }
        updatedAt = Date()
        return true
    }

    private static func replacingExactPath(
        in values: [String: String],
        oldPath: String,
        replacementPath: String
    ) -> [String: String] {
        values.mapValues { value in
            storedPath(value, references: oldPath) ? replacementPath : value
        }
    }

    private static func urlsReferenceSameFile(_ lhs: URL, _ rhs: URL) -> Bool {
        assetIdentityPath(for: lhs) == assetIdentityPath(for: rhs)
    }

    private static func storedPath(_ value: String, references path: String) -> Bool {
        guard value.hasPrefix("/"), path.hasPrefix("/") else {
            return false
        }
        return assetIdentityPath(for: URL(fileURLWithPath: value)) ==
            assetIdentityPath(for: URL(fileURLWithPath: path))
    }

    private static func insertStoredPath(_ path: String, into paths: inout Set<String>) {
        guard path.hasPrefix("/") else {
            return
        }
        paths.insert(assetIdentityPath(for: URL(fileURLWithPath: path)))
    }

    private static func pathSet(_ paths: Set<String>, contains url: URL) -> Bool {
        paths.contains(assetIdentityPath(for: url))
    }

    private static func pathSet(_ paths: Set<String>, containsStoredPath path: String) -> Bool {
        guard path.hasPrefix("/") else {
            return false
        }
        return paths.contains(assetIdentityPath(for: URL(fileURLWithPath: path)))
    }

    private static func refreshSourceSequenceSummary(_ artifact: inout ProcessingArtifact) {
        artifact.metrics["frames"] = String(artifact.frameCount)
        let recordedRoles = artifact.frames.compactMap { $0.metrics["role"] }
        guard recordedRoles.isEmpty == false else {
            return
        }
        let uniqueRoles = Set(recordedRoles)
        artifact.parameters["role"] = recordedRoles.count == artifact.frames.count && uniqueRoles.count == 1
            ? uniqueRoles.first
            : "mixed"
    }

    private static func artifactRecordsSourceAssets(_ artifact: ProcessingArtifact) -> Bool {
        valuesRecordSourceAssets(artifact.parameters) ||
            valuesRecordSourceAssets(artifact.metrics) ||
            artifact.frames.contains { valuesRecordSourceAssets($0.metrics) }
    }

    private static func valuesRecordSourceAssets(_ values: [String: String]) -> Bool {
        let singleKeys = ["assetID", "referenceID", "movingID", "sourceAssetID"]
        var foundIdentity = false
        for key in singleKeys {
            guard let value = values[key] else {
                continue
            }
            guard UUID(uuidString: value) != nil else {
                return false
            }
            foundIdentity = true
        }

        let listKeys = [
            "calibrationBiasIDs",
            "calibrationDarkIDs",
            "calibrationFlatIDs",
            "calibrationLightIDs",
            "lightIDs",
            "movingFrameIDs",
            "panelIDs",
            "selectedMovingFrameIDs",
            "sourceFrameIDs",
            "sourceLightIDs",
        ]
        for key in listKeys {
            guard let value = values[key] else {
                continue
            }
            guard value.isEmpty == false else {
                continue
            }
            let tokens = value.split(separator: ",", omittingEmptySubsequences: false)
            guard tokens.isEmpty == false, tokens.allSatisfy({ token in
                let normalized = token.trimmingCharacters(in: .whitespacesAndNewlines)
                return normalized.isEmpty == false && UUID(uuidString: normalized) != nil
            }) else {
                return false
            }
            foundIdentity = true
        }
        return foundIdentity
    }

    public func layerIDsLinked(to assetID: UUID) -> Set<ProcessingLayer.ID> {
        var linkedIDs = Set(layers.compactMap { layer in
            layer.parameters["assetID"] == assetID.uuidString ? layer.id : nil
        })

        var foundLinkedLayer = true
        while foundLinkedLayer {
            foundLinkedLayer = false
            for layer in layers where linkedIDs.contains(layer.id) == false {
                guard let sourceLayerID = layer.parameters["sourceLayerID"].flatMap(UUID.init(uuidString:)),
                      linkedIDs.contains(sourceLayerID)
                else {
                    continue
                }
                linkedIDs.insert(layer.id)
                foundLinkedLayer = true
            }
        }
        return linkedIDs
    }

    public mutating func updateRole(for assetID: UUID, role: CalibrationFrameRole) {
        guard let index = assets.firstIndex(where: { $0.id == assetID }) else {
            return
        }
        assets[index].role = role
        if role == .mosaic {
            if mosaicPanelOrder.contains(assetID) == false {
                mosaicPanelOrder.append(assetID)
            }
        } else {
            mosaicPanelOrder.removeAll { $0 == assetID }
        }
        updatedAt = Date()
    }

    public mutating func appendOperation(_ operation: EditOperation) {
        editGraph.operations.append(operation)
        updatedAt = Date()
    }

    public mutating func appendArtifact(_ artifact: ProcessingArtifact) {
        artifacts.append(artifact)
        updatedAt = Date()
    }

    public mutating func appendLayer(_ layer: ProcessingLayer) {
        layers.append(layer)
        updatedAt = Date()
    }

    public mutating func replaceBatchQueue(_ queue: [BatchQueueItem]) {
        batchQueue = queue
        updatedAt = Date()
    }

    public mutating func setBatchOutputDirectory(_ directory: URL?) {
        batchOutputDirectory = directory
        updatedAt = Date()
    }

    public mutating func setBatchOutputFormat(_ format: BatchOutputFormat) {
        batchOutputFormat = format
        updatedAt = Date()
    }

    public mutating func setBatchMaxConcurrentTasks(_ count: Int) {
        batchMaxConcurrentTasks = max(1, min(count, 4))
        updatedAt = Date()
    }
}

public enum ProcessingArtifactKind: String, Codable, CaseIterable, Equatable, Hashable, Identifiable, Sendable {
    case rawSequence
    case decodedSequence
    case calibratedSequence
    case registeredSequence
    case cleanedTimelapseSequence
    case stackMaster
    case editedImage
    case mask
    case meteorLayer
    case starLayer

    public var id: String {
        rawValue
    }
}

public struct ProcessingArtifactFrame: Identifiable, Codable, Equatable, Sendable {
    public var id: UUID
    public var sourceURL: URL?
    public var outputURL: URL?
    public var isReference: Bool
    public var metrics: [String: String]

    public init(
        id: UUID = UUID(),
        sourceURL: URL? = nil,
        outputURL: URL? = nil,
        isReference: Bool = false,
        metrics: [String: String] = [:]
    ) {
        self.id = id
        self.sourceURL = sourceURL
        self.outputURL = outputURL
        self.isReference = isReference
        self.metrics = metrics
    }

    private enum CodingKeys: String, CodingKey {
        case id
        case sourceURL
        case outputURL
        case isReference
        case metrics
    }

    public init(from decoder: Decoder) throws {
        let container = try decoder.container(keyedBy: CodingKeys.self)
        id = try container.decode(UUID.self, forKey: .id)
        sourceURL = try container.decodeIfPresent(URL.self, forKey: .sourceURL)
        outputURL = try container.decodeIfPresent(URL.self, forKey: .outputURL)
        isReference = try container.decodeIfPresent(Bool.self, forKey: .isReference) ?? false
        metrics = try container.decodeIfPresent([String: String].self, forKey: .metrics) ?? [:]
    }
}

public struct ProcessingArtifact: Identifiable, Codable, Equatable, Sendable {
    public var id: UUID
    public var kind: ProcessingArtifactKind
    public var name: String
    public var operationKind: ProcessingOperationKind?
    public var sourceArtifactIDs: [UUID]
    public var outputDirectory: URL?
    public var outputURLs: [URL]
    public var frames: [ProcessingArtifactFrame]
    public var parameters: [String: String]
    public var metrics: [String: String]
    public var createdAt: Date
    public var updatedAt: Date

    public init(
        id: UUID = UUID(),
        kind: ProcessingArtifactKind,
        name: String,
        operationKind: ProcessingOperationKind? = nil,
        sourceArtifactIDs: [UUID] = [],
        outputDirectory: URL? = nil,
        outputURLs: [URL] = [],
        frames: [ProcessingArtifactFrame] = [],
        parameters: [String: String] = [:],
        metrics: [String: String] = [:],
        createdAt: Date = Date(),
        updatedAt: Date = Date()
    ) {
        self.id = id
        self.kind = kind
        self.name = name
        self.operationKind = operationKind
        self.sourceArtifactIDs = sourceArtifactIDs
        self.outputDirectory = outputDirectory
        self.outputURLs = outputURLs
        self.frames = frames
        self.parameters = parameters
        self.metrics = metrics
        self.createdAt = createdAt
        self.updatedAt = updatedAt
    }

    private enum CodingKeys: String, CodingKey {
        case id
        case kind
        case name
        case operationKind
        case sourceArtifactIDs
        case outputDirectory
        case outputURLs
        case frames
        case parameters
        case metrics
        case createdAt
        case updatedAt
    }

    public init(from decoder: Decoder) throws {
        let container = try decoder.container(keyedBy: CodingKeys.self)
        id = try container.decode(UUID.self, forKey: .id)
        kind = try container.decode(ProcessingArtifactKind.self, forKey: .kind)
        name = try container.decode(String.self, forKey: .name)
        operationKind = try container.decodeIfPresent(ProcessingOperationKind.self, forKey: .operationKind)
        sourceArtifactIDs = try container.decodeIfPresent([UUID].self, forKey: .sourceArtifactIDs) ?? []
        outputDirectory = try container.decodeIfPresent(URL.self, forKey: .outputDirectory)
        outputURLs = try container.decodeIfPresent([URL].self, forKey: .outputURLs) ?? []
        frames = try container.decodeIfPresent([ProcessingArtifactFrame].self, forKey: .frames) ?? []
        parameters = try container.decodeIfPresent([String: String].self, forKey: .parameters) ?? [:]
        metrics = try container.decodeIfPresent([String: String].self, forKey: .metrics) ?? [:]
        createdAt = try container.decodeIfPresent(Date.self, forKey: .createdAt)
            ?? Date(timeIntervalSince1970: 0)
        updatedAt = try container.decodeIfPresent(Date.self, forKey: .updatedAt) ?? createdAt
    }

    public var frameCount: Int {
        frames.isEmpty ? outputURLs.count : frames.count
    }

    public var previewURL: URL? {
        if let previewPath = parameters["previewPath"]?.trimmingCharacters(in: .whitespacesAndNewlines),
           previewPath.isEmpty == false {
            return URL(fileURLWithPath: previewPath)
        }
        return outputURLs.first ?? frames.first?.outputURL
    }

    public var scientificURL: URL? {
        outputURLs.first(where: { AssetKind.detect(from: $0) == .fits })
            ?? frames.compactMap(\.outputURL).first(where: { AssetKind.detect(from: $0) == .fits })
    }
}

public enum ProcessingLayerKind: String, Codable, CaseIterable, Equatable, Hashable, Identifiable, Sendable {
    case baseImage
    case adjustment
    case mask
    case stars
    case meteors
    case background

    public var id: String {
        rawValue
    }
}

public enum ProcessingLayerBlendMode: String, Codable, CaseIterable, Equatable, Hashable, Identifiable, Sendable {
    case normal
    case screen
    case lighten
    case multiply
    case overlay

    public var id: String {
        rawValue
    }
}

public struct ProcessingLayer: Identifiable, Codable, Equatable, Sendable {
    public var id: UUID
    public var name: String
    public var kind: ProcessingLayerKind
    public var sourceArtifactID: UUID?
    public var inputURL: URL?
    public var maskArtifactID: UUID?
    public var blendMode: ProcessingLayerBlendMode
    public var opacity: Double {
        didSet {
            opacity = Self.normalizedOpacity(opacity)
        }
    }
    public var isVisible: Bool
    public var parameters: [String: String]
    public var createdAt: Date

    public init(
        id: UUID = UUID(),
        name: String,
        kind: ProcessingLayerKind,
        sourceArtifactID: UUID? = nil,
        inputURL: URL? = nil,
        maskArtifactID: UUID? = nil,
        blendMode: ProcessingLayerBlendMode = .normal,
        opacity: Double = 1.0,
        isVisible: Bool = true,
        parameters: [String: String] = [:],
        createdAt: Date = Date()
    ) {
        self.id = id
        self.name = name
        self.kind = kind
        self.sourceArtifactID = sourceArtifactID
        self.inputURL = inputURL
        self.maskArtifactID = maskArtifactID
        self.blendMode = blendMode
        self.opacity = Self.normalizedOpacity(opacity)
        self.isVisible = isVisible
        self.parameters = parameters
        self.createdAt = createdAt
    }

    private enum CodingKeys: String, CodingKey {
        case id
        case name
        case kind
        case sourceArtifactID
        case inputURL
        case maskArtifactID
        case blendMode
        case opacity
        case isVisible
        case parameters
        case createdAt
    }

    public init(from decoder: Decoder) throws {
        let container = try decoder.container(keyedBy: CodingKeys.self)
        id = try container.decode(UUID.self, forKey: .id)
        name = try container.decode(String.self, forKey: .name)
        kind = try container.decode(ProcessingLayerKind.self, forKey: .kind)
        sourceArtifactID = try container.decodeIfPresent(UUID.self, forKey: .sourceArtifactID)
        inputURL = try container.decodeIfPresent(URL.self, forKey: .inputURL)
        maskArtifactID = try container.decodeIfPresent(UUID.self, forKey: .maskArtifactID)
        blendMode = try container.decodeIfPresent(ProcessingLayerBlendMode.self, forKey: .blendMode) ?? .normal
        opacity = Self.normalizedOpacity(try container.decodeIfPresent(Double.self, forKey: .opacity) ?? 1)
        isVisible = try container.decodeIfPresent(Bool.self, forKey: .isVisible) ?? true
        parameters = try container.decodeIfPresent([String: String].self, forKey: .parameters) ?? [:]
        createdAt = try container.decodeIfPresent(Date.self, forKey: .createdAt)
            ?? Date(timeIntervalSince1970: 0)
    }

    private static func normalizedOpacity(_ value: Double) -> Double {
        value.isFinite ? min(max(value, 0.0), 1.0) : 1.0
    }
}

public enum BatchOutputFormat: String, Codable, CaseIterable, Equatable, Identifiable, Sendable {
    case png
    case tiff
    case jpeg

    public var id: String {
        rawValue
    }

    public var fileExtension: String {
        switch self {
        case .png:
            return "png"
        case .tiff:
            return "tiff"
        case .jpeg:
            return "jpg"
        }
    }
}

public enum ExportBitDepth: String, Codable, CaseIterable, Equatable, Identifiable, Sendable {
    case automatic
    case eight
    case sixteen

    public var id: String {
        rawValue
    }

    public var cliValue: String {
        switch self {
        case .automatic:
            return "auto"
        case .eight:
            return "8"
        case .sixteen:
            return "16"
        }
    }
}

public enum ExportColorSpace: String, Codable, CaseIterable, Equatable, Identifiable, Sendable {
    case srgb
    case linearSRGB

    public var id: String {
        rawValue
    }

    public var cliValue: String {
        switch self {
        case .srgb:
            return "srgb"
        case .linearSRGB:
            return "linear-srgb"
        }
    }
}

public enum FITSValueMode: String, Codable, CaseIterable, Equatable, Identifiable, Sendable {
    case display
    case scientific

    public var id: String {
        rawValue
    }

    public var cliValue: String {
        rawValue
    }
}

public struct ExportOptions: Codable, Equatable, Sendable {
    public var bitDepth: ExportBitDepth
    public var colorSpace: ExportColorSpace
    public var fitsValueMode: FITSValueMode
    public var jpegQuality: Double {
        didSet {
            jpegQuality = Self.normalizedJPEGQuality(jpegQuality)
        }
    }

    public init(
        bitDepth: ExportBitDepth = .automatic,
        colorSpace: ExportColorSpace = .srgb,
        fitsValueMode: FITSValueMode = .display,
        jpegQuality: Double = 0.92
    ) {
        self.bitDepth = bitDepth
        self.colorSpace = colorSpace
        self.fitsValueMode = fitsValueMode
        self.jpegQuality = Self.normalizedJPEGQuality(jpegQuality)
    }

    private enum CodingKeys: String, CodingKey {
        case bitDepth
        case colorSpace
        case fitsValueMode
        case jpegQuality
    }

    public init(from decoder: Decoder) throws {
        let container = try decoder.container(keyedBy: CodingKeys.self)
        self.init(
            bitDepth: try container.decodeIfPresent(ExportBitDepth.self, forKey: .bitDepth) ?? .automatic,
            colorSpace: try container.decodeIfPresent(ExportColorSpace.self, forKey: .colorSpace) ?? .srgb,
            fitsValueMode: try container.decodeIfPresent(FITSValueMode.self, forKey: .fitsValueMode) ?? .display,
            jpegQuality: try container.decodeIfPresent(Double.self, forKey: .jpegQuality) ?? 0.92
        )
    }

    private static func normalizedJPEGQuality(_ value: Double) -> Double {
        value.isFinite ? min(max(value, 0), 1) : 0.92
    }
}

public enum RawWhiteBalanceMode: String, Codable, CaseIterable, Equatable, Identifiable, Sendable {
    case camera
    case auto
    case daylight
    case manual

    public var id: String {
        rawValue
    }

    public var cliValue: String {
        rawValue
    }
}

public enum RawBlackLevelMode: String, Codable, CaseIterable, Equatable, Identifiable, Sendable {
    case camera
    case auto
    case manual

    public var id: String {
        rawValue
    }

    public var cliValue: String {
        rawValue
    }
}

public enum RawDemosaicQuality: String, Codable, CaseIterable, Equatable, Identifiable, Sendable {
    case fast
    case balanced
    case high

    public var id: String {
        rawValue
    }

    public var cliValue: String {
        rawValue
    }

    public static var allCases: [RawDemosaicQuality] {
        [.fast, .high]
    }
}

public struct RawProcessingOptions: Codable, Equatable, Sendable {
    public var whiteBalanceMode: RawWhiteBalanceMode
    public var manualWhiteBalanceTemperature: Double
    public var manualWhiteBalanceTint: Double
    public var exposureBias: Double
    public var blackLevelMode: RawBlackLevelMode
    public var manualBlackLevel: Double
    public var demosaicQuality: RawDemosaicQuality
    public var linearOutput: Bool

    public init(
        whiteBalanceMode: RawWhiteBalanceMode = .camera,
        manualWhiteBalanceTemperature: Double = 6500,
        manualWhiteBalanceTint: Double = 0,
        exposureBias: Double = 0.0,
        blackLevelMode: RawBlackLevelMode = .camera,
        manualBlackLevel: Double = 0,
        demosaicQuality: RawDemosaicQuality = .high,
        linearOutput: Bool = true
    ) {
        self.whiteBalanceMode = whiteBalanceMode
        self.manualWhiteBalanceTemperature = manualWhiteBalanceTemperature
        self.manualWhiteBalanceTint = manualWhiteBalanceTint
        self.exposureBias = exposureBias
        self.blackLevelMode = blackLevelMode
        self.manualBlackLevel = manualBlackLevel
        self.demosaicQuality = demosaicQuality
        self.linearOutput = linearOutput
    }

    private enum CodingKeys: String, CodingKey {
        case whiteBalanceMode
        case manualWhiteBalanceTemperature
        case manualWhiteBalanceTint
        case exposureBias
        case blackLevelMode
        case manualBlackLevel
        case demosaicQuality
        case linearOutput
    }

    public init(from decoder: Decoder) throws {
        let container = try decoder.container(keyedBy: CodingKeys.self)
        self.init(
            whiteBalanceMode: try container.decodeIfPresent(RawWhiteBalanceMode.self, forKey: .whiteBalanceMode) ?? .camera,
            manualWhiteBalanceTemperature: try container.decodeIfPresent(Double.self, forKey: .manualWhiteBalanceTemperature) ?? 6500,
            manualWhiteBalanceTint: try container.decodeIfPresent(Double.self, forKey: .manualWhiteBalanceTint) ?? 0,
            exposureBias: try container.decodeIfPresent(Double.self, forKey: .exposureBias) ?? 0,
            blackLevelMode: try container.decodeIfPresent(RawBlackLevelMode.self, forKey: .blackLevelMode) ?? .camera,
            manualBlackLevel: try container.decodeIfPresent(Double.self, forKey: .manualBlackLevel) ?? 0,
            demosaicQuality: try container.decodeIfPresent(RawDemosaicQuality.self, forKey: .demosaicQuality) ?? .high,
            linearOutput: try container.decodeIfPresent(Bool.self, forKey: .linearOutput) ?? true
        )
    }

    public func encode(to encoder: Encoder) throws {
        var container = encoder.container(keyedBy: CodingKeys.self)
        try container.encode(whiteBalanceMode, forKey: .whiteBalanceMode)
        try container.encode(manualWhiteBalanceTemperature, forKey: .manualWhiteBalanceTemperature)
        try container.encode(manualWhiteBalanceTint, forKey: .manualWhiteBalanceTint)
        try container.encode(exposureBias, forKey: .exposureBias)
        try container.encode(blackLevelMode, forKey: .blackLevelMode)
        try container.encode(manualBlackLevel, forKey: .manualBlackLevel)
        try container.encode(demosaicQuality, forKey: .demosaicQuality)
        try container.encode(linearOutput, forKey: .linearOutput)
    }
}

public struct PhotonStackAsset: Identifiable, Codable, Equatable, Sendable {
    public var id: UUID
    public var originalURL: URL
    public var kind: AssetKind
    public var role: CalibrationFrameRole
    public var metadata: AssetMetadata?
    public var importedAt: Date

    public init(
        id: UUID = UUID(),
        originalURL: URL,
        kind: AssetKind,
        role: CalibrationFrameRole = .light,
        metadata: AssetMetadata? = nil,
        importedAt: Date = Date()
    ) {
        self.id = id
        self.originalURL = originalURL
        self.kind = kind
        self.role = role
        self.metadata = metadata
        self.importedAt = importedAt
    }

    enum CodingKeys: String, CodingKey {
        case id
        case originalURL
        case kind
        case role
        case metadata
        case importedAt
    }

    public init(from decoder: Decoder) throws {
        let container = try decoder.container(keyedBy: CodingKeys.self)
        id = try container.decode(UUID.self, forKey: .id)
        originalURL = try container.decode(URL.self, forKey: .originalURL)
        kind = try container.decode(AssetKind.self, forKey: .kind)
        role = try container.decodeIfPresent(CalibrationFrameRole.self, forKey: .role) ?? .light
        metadata = try container.decodeIfPresent(AssetMetadata.self, forKey: .metadata)
        importedAt = try container.decode(Date.self, forKey: .importedAt)
    }

    public var displayName: String {
        originalURL.lastPathComponent
    }
}

public enum AssetKind: String, Codable, Equatable, Sendable {
    case raw
    case fits
    case tiff
    case jpeg
    case heif
    case png
    case unknown

    public static func detect(from url: URL) -> AssetKind {
        switch url.pathExtension.lowercased() {
        case "arw", "cr2", "cr3", "dng", "nef", "orf", "raf", "rw2", "srw":
            return .raw
        case "fit", "fits", "fts":
            return .fits
        case "tif", "tiff":
            return .tiff
        case "jpg", "jpeg":
            return .jpeg
        case "heic", "heif", "hif":
            return .heif
        case "png":
            return .png
        default:
            return .unknown
        }
    }
}

public enum CalibrationFrameRole: String, Codable, CaseIterable, Equatable, Sendable {
    case light
    case dark
    case bias
    case flat
    case mosaic
    case reference
    case other

    public var displayName: String {
        switch self {
        case .light:
            return "Light"
        case .dark:
            return "Dark"
        case .bias:
            return "Bias"
        case .flat:
            return "Flat"
        case .mosaic:
            return "Mosaic"
        case .reference:
            return "Reference"
        case .other:
            return "Other"
        }
    }
}

public enum CalibrationBiasState: String, Codable, CaseIterable, Equatable, Identifiable, Sendable {
    case included
    case removed

    public var id: String {
        rawValue
    }
}

public struct AssetMetadata: Codable, Equatable, Sendable {
    public var width: Int?
    public var height: Int?
    public var channels: Int?
    public var bitsPerChannel: Int?
    public var formatDescription: String?
    public var cameraMake: String?
    public var cameraModel: String?
    public var lensModel: String?
    public var captureDate: String?
    public var colorModel: String?
    public var colorProfile: String?
    public var rawDecoder: String?
    public var whiteBalance: String?
    public var exposureBias: String?
    public var exposureTimeSeconds: Double?
    public var fNumber: Double?
    public var focalLengthMM: Double?
    public var iso: Int?
    public var orientation: Int?

    private enum CodingKeys: String, CodingKey {
        case width
        case height
        case channels
        case bitsPerChannel
        case formatDescription
        case cameraMake
        case cameraModel
        case lensModel
        case captureDate
        case colorModel
        case colorProfile
        case rawDecoder
        case whiteBalance
        case exposureBias
        case exposureTimeSeconds
        case fNumber
        case focalLengthMM
        case iso
        case orientation
    }

    public init(
        width: Int? = nil,
        height: Int? = nil,
        channels: Int? = nil,
        bitsPerChannel: Int? = nil,
        formatDescription: String? = nil,
        cameraMake: String? = nil,
        cameraModel: String? = nil,
        lensModel: String? = nil,
        captureDate: String? = nil,
        colorModel: String? = nil,
        colorProfile: String? = nil,
        rawDecoder: String? = nil,
        whiteBalance: String? = nil,
        exposureBias: String? = nil,
        exposureTimeSeconds: Double? = nil,
        fNumber: Double? = nil,
        focalLengthMM: Double? = nil,
        iso: Int? = nil,
        orientation: Int? = nil
    ) {
        self.width = Self.positiveInteger(width)
        self.height = Self.positiveInteger(height)
        self.channels = Self.positiveInteger(channels)
        self.bitsPerChannel = Self.positiveInteger(bitsPerChannel)
        self.formatDescription = Self.nonemptyString(formatDescription)
        self.cameraMake = Self.nonemptyString(cameraMake)
        self.cameraModel = Self.nonemptyString(cameraModel)
        self.lensModel = Self.nonemptyString(lensModel)
        self.captureDate = Self.nonemptyString(captureDate)
        self.colorModel = Self.nonemptyString(colorModel)
        self.colorProfile = Self.nonemptyString(colorProfile)
        self.rawDecoder = Self.nonemptyString(rawDecoder)
        self.whiteBalance = Self.nonemptyString(whiteBalance)
        self.exposureBias = Self.nonemptyString(exposureBias)
        self.exposureTimeSeconds = Self.positiveFiniteDouble(exposureTimeSeconds)
        self.fNumber = Self.positiveFiniteDouble(fNumber)
        self.focalLengthMM = Self.positiveFiniteDouble(focalLengthMM)
        self.iso = Self.positiveInteger(iso)
        self.orientation = orientation.flatMap { (1...8).contains($0) ? $0 : nil }
    }

    public init(from decoder: Decoder) throws {
        let container = try decoder.container(keyedBy: CodingKeys.self)
        self.init(
            width: try? container.decode(Int.self, forKey: .width),
            height: try? container.decode(Int.self, forKey: .height),
            channels: try? container.decode(Int.self, forKey: .channels),
            bitsPerChannel: try? container.decode(Int.self, forKey: .bitsPerChannel),
            formatDescription: try? container.decode(String.self, forKey: .formatDescription),
            cameraMake: try? container.decode(String.self, forKey: .cameraMake),
            cameraModel: try? container.decode(String.self, forKey: .cameraModel),
            lensModel: try? container.decode(String.self, forKey: .lensModel),
            captureDate: try? container.decode(String.self, forKey: .captureDate),
            colorModel: try? container.decode(String.self, forKey: .colorModel),
            colorProfile: try? container.decode(String.self, forKey: .colorProfile),
            rawDecoder: try? container.decode(String.self, forKey: .rawDecoder),
            whiteBalance: try? container.decode(String.self, forKey: .whiteBalance),
            exposureBias: try? container.decode(String.self, forKey: .exposureBias),
            exposureTimeSeconds: try? container.decode(Double.self, forKey: .exposureTimeSeconds),
            fNumber: try? container.decode(Double.self, forKey: .fNumber),
            focalLengthMM: try? container.decode(Double.self, forKey: .focalLengthMM),
            iso: try? container.decode(Int.self, forKey: .iso),
            orientation: try? container.decode(Int.self, forKey: .orientation)
        )
    }

    public func encode(to encoder: Encoder) throws {
        var container = encoder.container(keyedBy: CodingKeys.self)
        try container.encodeIfPresent(Self.positiveInteger(width), forKey: .width)
        try container.encodeIfPresent(Self.positiveInteger(height), forKey: .height)
        try container.encodeIfPresent(Self.positiveInteger(channels), forKey: .channels)
        try container.encodeIfPresent(Self.positiveInteger(bitsPerChannel), forKey: .bitsPerChannel)
        try container.encodeIfPresent(Self.nonemptyString(formatDescription), forKey: .formatDescription)
        try container.encodeIfPresent(Self.nonemptyString(cameraMake), forKey: .cameraMake)
        try container.encodeIfPresent(Self.nonemptyString(cameraModel), forKey: .cameraModel)
        try container.encodeIfPresent(Self.nonemptyString(lensModel), forKey: .lensModel)
        try container.encodeIfPresent(Self.nonemptyString(captureDate), forKey: .captureDate)
        try container.encodeIfPresent(Self.nonemptyString(colorModel), forKey: .colorModel)
        try container.encodeIfPresent(Self.nonemptyString(colorProfile), forKey: .colorProfile)
        try container.encodeIfPresent(Self.nonemptyString(rawDecoder), forKey: .rawDecoder)
        try container.encodeIfPresent(Self.nonemptyString(whiteBalance), forKey: .whiteBalance)
        try container.encodeIfPresent(Self.nonemptyString(exposureBias), forKey: .exposureBias)
        try container.encodeIfPresent(Self.positiveFiniteDouble(exposureTimeSeconds), forKey: .exposureTimeSeconds)
        try container.encodeIfPresent(Self.positiveFiniteDouble(fNumber), forKey: .fNumber)
        try container.encodeIfPresent(Self.positiveFiniteDouble(focalLengthMM), forKey: .focalLengthMM)
        try container.encodeIfPresent(Self.positiveInteger(iso), forKey: .iso)
        try container.encodeIfPresent(orientation.flatMap { (1...8).contains($0) ? $0 : nil }, forKey: .orientation)
    }

    private static func positiveInteger(_ value: Int?) -> Int? {
        value.flatMap { $0 > 0 ? $0 : nil }
    }

    private static func positiveFiniteDouble(_ value: Double?) -> Double? {
        value.flatMap { $0.isFinite && $0 > 0 ? $0 : nil }
    }

    private static func nonemptyString(_ value: String?) -> String? {
        guard let value else { return nil }
        let trimmed = value.trimmingCharacters(in: .whitespacesAndNewlines)
        return trimmed.isEmpty ? nil : trimmed
    }
}

public struct MosaicPanelPlan: Identifiable, Codable, Equatable, Sendable {
    public var id: UUID
    public var index: Int
    public var name: String
    public var width: Int?
    public var height: Int?

    public init(id: UUID, index: Int, name: String, width: Int?, height: Int?) {
        self.id = id
        self.index = index
        self.name = name
        self.width = width
        self.height = height
    }
}

public struct MosaicPlanSummary: Codable, Equatable, Sendable {
    public var panels: [MosaicPanelPlan]
    public var overlapPixels: Int
    public var layout: MosaicLayoutMode
    public var columns: Int
    public var estimatedWidth: Int?
    public var estimatedHeight: Int?
    public var missingMetadataCount: Int
    public var hasHeightMismatch: Bool
    public var suggestedOverlapPixels: Int?

    public init(
        assets: [PhotonStackAsset],
        overlapPixels: Int,
        layout: MosaicLayoutMode = .horizontal,
        columns: Int = 1
    ) {
        let clampedOverlap = max(0, overlapPixels)
        let clampedColumns = max(1, min(columns, max(assets.count, 1)))
        self.panels = assets.enumerated().map { index, asset in
            MosaicPanelPlan(
                id: asset.id,
                index: index + 1,
                name: asset.displayName,
                width: asset.metadata?.width,
                height: asset.metadata?.height
            )
        }
        self.overlapPixels = clampedOverlap
        self.layout = layout
        self.columns = layout == .horizontal ? max(assets.count, 1) : clampedColumns
        self.missingMetadataCount = panels.filter {
            guard let width = $0.width, let height = $0.height else {
                return true
            }
            return width <= 0 || height <= 0
        }.count

        let widths = panels.compactMap(\.width)
        let heights = panels.compactMap(\.height)
        if panels.isEmpty == false,
           widths.count == panels.count,
           heights.count == panels.count,
           widths.allSatisfy({ $0 > 0 }),
           heights.allSatisfy({ $0 > 0 }) {
            let effectiveOverlap = min(clampedOverlap, max((widths.min() ?? 1) - 1, 0))
            if layout == .grid {
                var rowWidths: [Int] = []
                var rowHeights: [Int] = []
                var rowsAreRepresentable = true
                for rowStart in stride(from: 0, to: panels.count, by: self.columns) {
                    let rowEnd = min(rowStart + self.columns, panels.count)
                    let rowWidthsSlice = widths[rowStart..<rowEnd]
                    let rowHeightsSlice = heights[rowStart..<rowEnd]
                    guard let rowWidth = Self.overlapAdjustedSum(
                        rowWidthsSlice,
                        overlap: effectiveOverlap
                    ) else {
                        rowsAreRepresentable = false
                        break
                    }
                    rowWidths.append(rowWidth)
                    rowHeights.append(rowHeightsSlice.max() ?? 0)
                }
                if rowsAreRepresentable {
                    estimatedWidth = rowWidths.max()
                    estimatedHeight = Self.overlapAdjustedSum(rowHeights, overlap: effectiveOverlap)
                } else {
                    estimatedWidth = nil
                    estimatedHeight = nil
                }
            } else {
                estimatedWidth = Self.overlapAdjustedSum(widths, overlap: effectiveOverlap)
                estimatedHeight = heights.max()
            }
            hasHeightMismatch = Set(heights).count > 1
            let adjacentWidths = zip(widths.dropLast(), widths.dropFirst()).map { min($0, $1) }
            if let narrowestAdjacentWidth = adjacentWidths.min() {
                suggestedOverlapPixels = max(0, Int((Double(narrowestAdjacentWidth) * 0.15).rounded()))
            } else {
                suggestedOverlapPixels = nil
            }
        } else {
            estimatedWidth = nil
            estimatedHeight = nil
            hasHeightMismatch = false
            suggestedOverlapPixels = nil
        }
    }

    private static func overlapAdjustedSum<Values: Collection>(
        _ values: Values,
        overlap: Int
    ) -> Int? where Values.Element == Int {
        var total = 0
        for value in values {
            let addition = total.addingReportingOverflow(value)
            guard addition.overflow == false else {
                return nil
            }
            total = addition.partialValue
        }
        let overlapTotal = overlap.multipliedReportingOverflow(by: max(values.count - 1, 0))
        guard overlapTotal.overflow == false else {
            return nil
        }
        let adjusted = total.subtractingReportingOverflow(overlapTotal.partialValue)
        guard adjusted.overflow == false else {
            return nil
        }
        return max(0, adjusted.partialValue)
    }
}

public struct EditGraph: Codable, Equatable, Sendable {
    public var operations: [EditOperation]

    public init(operations: [EditOperation] = []) {
        self.operations = operations
    }

    private enum CodingKeys: String, CodingKey {
        case operations
    }

    public init(from decoder: Decoder) throws {
        let container = try decoder.container(keyedBy: CodingKeys.self)
        operations = try container.decodeIfPresent([EditOperation].self, forKey: .operations) ?? []
    }
}

public enum ProjectTemplate: String, Codable, CaseIterable, Equatable, Sendable {
    case deepSky
    case lunarPlanetary
    case mosaic
    case singleFrame

    public func makeProject(name: String? = nil) -> PhotonStackProject {
        var project = PhotonStackProject(name: name ?? defaultProjectName, template: self)
        project.editGraph.operations = defaultOperations
        return project
    }

    public var defaultImportedAssetRole: CalibrationFrameRole {
        switch self {
        case .mosaic:
            return .mosaic
        case .deepSky, .lunarPlanetary, .singleFrame:
            return .light
        }
    }

    public var defaultProjectName: String {
        switch self {
        case .deepSky:
            return "Deep Sky Project"
        case .lunarPlanetary:
            return "Lunar Planetary Project"
        case .mosaic:
            return "Mosaic Project"
        case .singleFrame:
            return "Single Frame Project"
        }
    }

    private var defaultOperations: [EditOperation] {
        switch self {
        case .deepSky:
            return [
                EditOperation(kind: .calibrate, parameters: ["template": rawValue]),
                EditOperation(kind: .stack, parameters: ["method": StackMethod.winsorized.rawValue]),
                EditOperation(kind: .stretch),
            ]
        case .lunarPlanetary:
            return [
                EditOperation(kind: .stack, parameters: ["method": StackMethod.weighted.rawValue]),
                EditOperation(kind: .sharpen),
            ]
        case .mosaic:
            return [
                EditOperation(kind: .mosaic),
                EditOperation(kind: .background),
                EditOperation(kind: .stretch),
            ]
        case .singleFrame:
            return [
                EditOperation(kind: .preview),
                EditOperation(kind: .stretch),
            ]
        }
    }
}

public struct EditOperation: Identifiable, Codable, Equatable, Sendable {
    public var id: UUID
    public var kind: ProcessingOperationKind
    public var parameters: [String: String]
    public var isEnabled: Bool

    public init(
        id: UUID = UUID(),
        kind: ProcessingOperationKind,
        parameters: [String: String] = [:],
        isEnabled: Bool = true
    ) {
        self.id = id
        self.kind = kind
        self.parameters = parameters
        self.isEnabled = isEnabled
    }

    private enum CodingKeys: String, CodingKey {
        case id
        case kind
        case parameters
        case isEnabled
    }

    public init(from decoder: Decoder) throws {
        let container = try decoder.container(keyedBy: CodingKeys.self)
        id = try container.decode(UUID.self, forKey: .id)
        kind = try container.decode(ProcessingOperationKind.self, forKey: .kind)
        parameters = try container.decodeIfPresent([String: String].self, forKey: .parameters) ?? [:]
        isEnabled = try container.decodeIfPresent(Bool.self, forKey: .isEnabled) ?? true
    }
}

public enum CurveChannel: String, Codable, CaseIterable, Equatable, Identifiable, Sendable {
    case rgb
    case red
    case green
    case blue
    case luminance

    public var id: String {
        rawValue
    }
}

public struct CurveEditorPoint: Codable, Equatable, Identifiable, Sendable {
    public var id: UUID
    public var input: Double
    public var output: Double

    public init(id: UUID = UUID(), input: Double, output: Double) {
        self.id = id
        self.input = input
        self.output = output
    }
}

public struct ProcessingParameters: Codable, Equatable, Sendable {
    public var previewWidth: Int
    public var stretchTargetBackground: Double
    public var starReductionAmount: Double
    public var profileAwareStarReduction: Bool
    public var edgeAwareStarReduction: Bool
    public var comaReductionAmount: Double
    public var comaReductionRadius: Int
    public var comaReductionEccentricity: Double
    public var edgeAwareComaReduction: Bool
    public var localContrastAmount: Double
    public var localContrastRadius: Int
    public var denoiseAmount: Double
    public var denoiseChromaAmount: Double
    public var denoiseRadius: Int
    public var sharpenAmount: Double
    public var sharpenRadius: Int
    public var cloudRemovalStrength: Double
    public var workflowMasterMethod: StackMethod
    public var workflowDarkBiasState: CalibrationBiasState
    public var workflowFlatBiasState: CalibrationBiasState
    public var workflowStackMethod: StackMethod
    public var workflowAlignment: AlignmentMethod
    public var workflowRestoreMeteors: Bool
    public var curveBlackPoint: Double
    public var curveMidInput: Double
    public var curveMidOutput: Double
    public var curveWhitePoint: Double
    public var curveChannel: CurveChannel
    public var curvePoints: [CurveEditorPoint]
    public var mosaicOverlapPixels: Int
    public var mosaicProjection: MosaicProjection
    public var mosaicLayout: MosaicLayoutMode
    public var mosaicAlignment: MosaicAlignmentMode
    public var mosaicBlendMode: MosaicBlendMode
    public var mosaicExposureMatching: Bool
    public var mosaicColumns: Int
    public var mosaicPreviewWidth: Int
    public var rawProcessingOptions: RawProcessingOptions

    public init(
        previewWidth: Int = 1600,
        stretchTargetBackground: Double = 0.25,
        starReductionAmount: Double = 0.35,
        profileAwareStarReduction: Bool = true,
        edgeAwareStarReduction: Bool = true,
        comaReductionAmount: Double = 0.80,
        comaReductionRadius: Int = 10,
        comaReductionEccentricity: Double = 0.22,
        edgeAwareComaReduction: Bool = true,
        localContrastAmount: Double = 0.25,
        localContrastRadius: Int = 8,
        denoiseAmount: Double = 0.4,
        denoiseChromaAmount: Double = 0.55,
        denoiseRadius: Int = 1,
        sharpenAmount: Double = 0.35,
        sharpenRadius: Int = 1,
        cloudRemovalStrength: Double = 0.65,
        workflowMasterMethod: StackMethod = .median,
        workflowDarkBiasState: CalibrationBiasState = .included,
        workflowFlatBiasState: CalibrationBiasState = .included,
        workflowStackMethod: StackMethod = .winsorized,
        workflowAlignment: AlignmentMethod = .translation,
        workflowRestoreMeteors: Bool = false,
        curveBlackPoint: Double = 0.0,
        curveMidInput: Double = 0.5,
        curveMidOutput: Double = 0.5,
        curveWhitePoint: Double = 1.0,
        curveChannel: CurveChannel = .rgb,
        curvePoints: [CurveEditorPoint]? = nil,
        mosaicOverlapPixels: Int = 0,
        mosaicProjection: MosaicProjection = .planar,
        mosaicLayout: MosaicLayoutMode = .horizontal,
        mosaicAlignment: MosaicAlignmentMode = .manual,
        mosaicBlendMode: MosaicBlendMode = .feather,
        mosaicExposureMatching: Bool = true,
        mosaicColumns: Int = 2,
        mosaicPreviewWidth: Int = 1200,
        rawProcessingOptions: RawProcessingOptions = RawProcessingOptions()
    ) {
        self.previewWidth = previewWidth
        self.stretchTargetBackground = stretchTargetBackground
        self.starReductionAmount = starReductionAmount
        self.profileAwareStarReduction = profileAwareStarReduction
        self.edgeAwareStarReduction = edgeAwareStarReduction
        self.comaReductionAmount = comaReductionAmount
        self.comaReductionRadius = comaReductionRadius
        self.comaReductionEccentricity = comaReductionEccentricity
        self.edgeAwareComaReduction = edgeAwareComaReduction
        self.localContrastAmount = localContrastAmount
        self.localContrastRadius = localContrastRadius
        self.denoiseAmount = denoiseAmount
        self.denoiseChromaAmount = denoiseChromaAmount
        self.denoiseRadius = denoiseRadius
        self.sharpenAmount = sharpenAmount
        self.sharpenRadius = sharpenRadius
        self.cloudRemovalStrength = cloudRemovalStrength
        self.workflowMasterMethod = workflowMasterMethod
        self.workflowDarkBiasState = workflowDarkBiasState
        self.workflowFlatBiasState = workflowFlatBiasState
        self.workflowStackMethod = workflowStackMethod
        self.workflowAlignment = workflowAlignment
        self.workflowRestoreMeteors = workflowRestoreMeteors
        self.curveBlackPoint = curveBlackPoint
        self.curveMidInput = curveMidInput
        self.curveMidOutput = curveMidOutput
        self.curveWhitePoint = curveWhitePoint
        self.curveChannel = curveChannel
        self.curvePoints = curvePoints ?? Self.defaultCurvePoints(
            black: curveBlackPoint,
            midInput: curveMidInput,
            midOutput: curveMidOutput,
            white: curveWhitePoint
        )
        self.mosaicOverlapPixels = mosaicOverlapPixels
        self.mosaicProjection = mosaicProjection
        self.mosaicLayout = mosaicLayout
        self.mosaicAlignment = mosaicAlignment
        self.mosaicBlendMode = mosaicBlendMode
        self.mosaicExposureMatching = mosaicExposureMatching
        self.mosaicColumns = mosaicColumns
        self.mosaicPreviewWidth = mosaicPreviewWidth
        self.rawProcessingOptions = rawProcessingOptions
    }

    private enum CodingKeys: String, CodingKey {
        case previewWidth
        case stretchTargetBackground
        case starReductionAmount
        case profileAwareStarReduction
        case edgeAwareStarReduction
        case comaReductionAmount
        case comaReductionRadius
        case comaReductionEccentricity
        case edgeAwareComaReduction
        case localContrastAmount
        case localContrastRadius
        case denoiseAmount
        case denoiseChromaAmount
        case denoiseRadius
        case sharpenAmount
        case sharpenRadius
        case cloudRemovalStrength
        case workflowMasterMethod
        case workflowDarkBiasState
        case workflowFlatBiasState
        case workflowStackMethod
        case workflowAlignment
        case workflowRestoreMeteors
        case curveBlackPoint
        case curveMidInput
        case curveMidOutput
        case curveWhitePoint
        case curveChannel
        case curvePoints
        case mosaicOverlapPixels
        case mosaicProjection
        case mosaicLayout
        case mosaicAlignment
        case mosaicBlendMode
        case mosaicExposureMatching
        case mosaicColumns
        case mosaicPreviewWidth
        case rawProcessingOptions
    }

    public init(from decoder: Decoder) throws {
        let container = try decoder.container(keyedBy: CodingKeys.self)
        let legacy = ProcessingParameters()
        previewWidth = try container.decodeIfPresent(Int.self, forKey: .previewWidth) ?? legacy.previewWidth
        stretchTargetBackground = try container.decodeIfPresent(Double.self, forKey: .stretchTargetBackground) ?? legacy.stretchTargetBackground
        starReductionAmount = try container.decodeIfPresent(Double.self, forKey: .starReductionAmount) ?? legacy.starReductionAmount
        profileAwareStarReduction = try container.decodeIfPresent(Bool.self, forKey: .profileAwareStarReduction) ?? legacy.profileAwareStarReduction
        edgeAwareStarReduction = try container.decodeIfPresent(Bool.self, forKey: .edgeAwareStarReduction) ?? legacy.edgeAwareStarReduction
        comaReductionAmount = try container.decodeIfPresent(Double.self, forKey: .comaReductionAmount) ?? legacy.comaReductionAmount
        comaReductionRadius = try container.decodeIfPresent(Int.self, forKey: .comaReductionRadius) ?? legacy.comaReductionRadius
        comaReductionEccentricity = try container.decodeIfPresent(Double.self, forKey: .comaReductionEccentricity) ?? legacy.comaReductionEccentricity
        edgeAwareComaReduction = try container.decodeIfPresent(Bool.self, forKey: .edgeAwareComaReduction) ?? legacy.edgeAwareComaReduction
        localContrastAmount = try container.decodeIfPresent(Double.self, forKey: .localContrastAmount) ?? legacy.localContrastAmount
        localContrastRadius = try container.decodeIfPresent(Int.self, forKey: .localContrastRadius) ?? legacy.localContrastRadius
        denoiseAmount = try container.decodeIfPresent(Double.self, forKey: .denoiseAmount) ?? legacy.denoiseAmount
        denoiseChromaAmount = try container.decodeIfPresent(Double.self, forKey: .denoiseChromaAmount) ?? legacy.denoiseChromaAmount
        denoiseRadius = try container.decodeIfPresent(Int.self, forKey: .denoiseRadius) ?? legacy.denoiseRadius
        sharpenAmount = try container.decodeIfPresent(Double.self, forKey: .sharpenAmount) ?? legacy.sharpenAmount
        sharpenRadius = try container.decodeIfPresent(Int.self, forKey: .sharpenRadius) ?? legacy.sharpenRadius
        cloudRemovalStrength = try container.decodeIfPresent(Double.self, forKey: .cloudRemovalStrength) ?? legacy.cloudRemovalStrength
        workflowMasterMethod = try container.decodeIfPresent(StackMethod.self, forKey: .workflowMasterMethod) ?? legacy.workflowMasterMethod
        workflowDarkBiasState = try container.decodeIfPresent(CalibrationBiasState.self, forKey: .workflowDarkBiasState) ?? legacy.workflowDarkBiasState
        workflowFlatBiasState = try container.decodeIfPresent(CalibrationBiasState.self, forKey: .workflowFlatBiasState) ?? legacy.workflowFlatBiasState
        workflowStackMethod = try container.decodeIfPresent(StackMethod.self, forKey: .workflowStackMethod) ?? legacy.workflowStackMethod
        workflowAlignment = try container.decodeIfPresent(AlignmentMethod.self, forKey: .workflowAlignment) ?? legacy.workflowAlignment
        workflowRestoreMeteors = try container.decodeIfPresent(Bool.self, forKey: .workflowRestoreMeteors) ?? legacy.workflowRestoreMeteors
        curveBlackPoint = try container.decodeIfPresent(Double.self, forKey: .curveBlackPoint) ?? legacy.curveBlackPoint
        curveMidInput = try container.decodeIfPresent(Double.self, forKey: .curveMidInput) ?? legacy.curveMidInput
        curveMidOutput = try container.decodeIfPresent(Double.self, forKey: .curveMidOutput) ?? legacy.curveMidOutput
        curveWhitePoint = try container.decodeIfPresent(Double.self, forKey: .curveWhitePoint) ?? legacy.curveWhitePoint
        curveChannel = try container.decodeIfPresent(CurveChannel.self, forKey: .curveChannel) ?? .rgb
        curvePoints = Self.normalizedCurvePoints(
            try container.decodeIfPresent([CurveEditorPoint].self, forKey: .curvePoints)
                ?? Self.defaultCurvePoints(
                    black: curveBlackPoint,
                    midInput: curveMidInput,
                    midOutput: curveMidOutput,
                    white: curveWhitePoint
                )
        )
        mosaicOverlapPixels = try container.decodeIfPresent(Int.self, forKey: .mosaicOverlapPixels) ?? legacy.mosaicOverlapPixels
        mosaicProjection = try container.decodeIfPresent(MosaicProjection.self, forKey: .mosaicProjection) ?? legacy.mosaicProjection
        mosaicLayout = try container.decodeIfPresent(MosaicLayoutMode.self, forKey: .mosaicLayout) ?? legacy.mosaicLayout
        mosaicAlignment = try container.decodeIfPresent(MosaicAlignmentMode.self, forKey: .mosaicAlignment) ?? legacy.mosaicAlignment
        mosaicBlendMode = try container.decodeIfPresent(MosaicBlendMode.self, forKey: .mosaicBlendMode) ?? legacy.mosaicBlendMode
        mosaicExposureMatching = try container.decodeIfPresent(Bool.self, forKey: .mosaicExposureMatching) ?? legacy.mosaicExposureMatching
        mosaicColumns = try container.decodeIfPresent(Int.self, forKey: .mosaicColumns) ?? legacy.mosaicColumns
        mosaicPreviewWidth = try container.decodeIfPresent(Int.self, forKey: .mosaicPreviewWidth) ?? legacy.mosaicPreviewWidth
        rawProcessingOptions = try container.decodeIfPresent(RawProcessingOptions.self, forKey: .rawProcessingOptions) ?? legacy.rawProcessingOptions
    }

    public func encode(to encoder: Encoder) throws {
        var container = encoder.container(keyedBy: CodingKeys.self)
        try container.encode(previewWidth, forKey: .previewWidth)
        try container.encode(stretchTargetBackground, forKey: .stretchTargetBackground)
        try container.encode(starReductionAmount, forKey: .starReductionAmount)
        try container.encode(profileAwareStarReduction, forKey: .profileAwareStarReduction)
        try container.encode(edgeAwareStarReduction, forKey: .edgeAwareStarReduction)
        try container.encode(comaReductionAmount, forKey: .comaReductionAmount)
        try container.encode(comaReductionRadius, forKey: .comaReductionRadius)
        try container.encode(comaReductionEccentricity, forKey: .comaReductionEccentricity)
        try container.encode(edgeAwareComaReduction, forKey: .edgeAwareComaReduction)
        try container.encode(localContrastAmount, forKey: .localContrastAmount)
        try container.encode(localContrastRadius, forKey: .localContrastRadius)
        try container.encode(denoiseAmount, forKey: .denoiseAmount)
        try container.encode(denoiseChromaAmount, forKey: .denoiseChromaAmount)
        try container.encode(denoiseRadius, forKey: .denoiseRadius)
        try container.encode(sharpenAmount, forKey: .sharpenAmount)
        try container.encode(sharpenRadius, forKey: .sharpenRadius)
        try container.encode(cloudRemovalStrength, forKey: .cloudRemovalStrength)
        try container.encode(workflowMasterMethod, forKey: .workflowMasterMethod)
        try container.encode(workflowDarkBiasState, forKey: .workflowDarkBiasState)
        try container.encode(workflowFlatBiasState, forKey: .workflowFlatBiasState)
        try container.encode(workflowStackMethod, forKey: .workflowStackMethod)
        try container.encode(workflowAlignment, forKey: .workflowAlignment)
        try container.encode(workflowRestoreMeteors, forKey: .workflowRestoreMeteors)
        try container.encode(curveBlackPoint, forKey: .curveBlackPoint)
        try container.encode(curveMidInput, forKey: .curveMidInput)
        try container.encode(curveMidOutput, forKey: .curveMidOutput)
        try container.encode(curveWhitePoint, forKey: .curveWhitePoint)
        try container.encode(curveChannel, forKey: .curveChannel)
        try container.encode(curveEditorPoints, forKey: .curvePoints)
        try container.encode(mosaicOverlapPixels, forKey: .mosaicOverlapPixels)
        try container.encode(mosaicProjection, forKey: .mosaicProjection)
        try container.encode(mosaicLayout, forKey: .mosaicLayout)
        try container.encode(mosaicAlignment, forKey: .mosaicAlignment)
        try container.encode(mosaicBlendMode, forKey: .mosaicBlendMode)
        try container.encode(mosaicExposureMatching, forKey: .mosaicExposureMatching)
        try container.encode(mosaicColumns, forKey: .mosaicColumns)
        try container.encode(mosaicPreviewWidth, forKey: .mosaicPreviewWidth)
        try container.encode(rawProcessingOptions, forKey: .rawProcessingOptions)
    }

    public var curveControlPoints: [CurveControlPoint] {
        let white = clamp(curveWhitePoint, lower: 0.05, upper: 1.0)
        let black = clamp(curveBlackPoint, lower: 0.0, upper: min(0.95, white - 0.02))
        let midInput = clamp(curveMidInput, lower: black + 0.01, upper: white - 0.01)
        let midOutput = clamp(curveMidOutput, lower: 0.0, upper: 1.0)

        return [
            CurveControlPoint(role: .black, input: black, output: 0.0),
            CurveControlPoint(role: .mid, input: midInput, output: midOutput),
            CurveControlPoint(role: .white, input: white, output: 1.0),
        ]
    }

    public var curvePointPairs: [(Double, Double)] {
        curveEditorPoints.map { ($0.input, $0.output) }
    }

    public var curveEditorPoints: [CurveEditorPoint] {
        Self.normalizedCurvePoints(curvePoints)
    }

    public var matchingCurvePreset: CurvePreset {
        CurvePreset.allCases.first { preset in
            guard preset != .custom else {
                return false
            }
            return preset.matches(self)
        } ?? .custom
    }

    public mutating func applyCurvePreset(_ preset: CurvePreset) {
        guard let points = preset.parameters else {
            return
        }

        curveBlackPoint = points.black
        curveMidInput = points.midInput
        curveMidOutput = points.midOutput
        curveWhitePoint = points.white
        syncCurvePointsFromLegacyControls()
    }

    public mutating func updateCurveControlPoint(_ role: CurveControlPointRole, input: Double, output: Double) {
        let points = curveControlPoints
        let black = points[0]
        let mid = points[1]
        let white = points[2]

        switch role {
        case .black:
            curveBlackPoint = clamp(input, lower: 0.0, upper: mid.input - 0.01)
        case .mid:
            curveMidInput = clamp(input, lower: black.input + 0.01, upper: white.input - 0.01)
            curveMidOutput = clamp(output, lower: 0.0, upper: 1.0)
        case .white:
            curveWhitePoint = clamp(input, lower: mid.input + 0.01, upper: 1.0)
        }
        syncCurvePointsFromLegacyControls()
    }

    public mutating func addCurvePoint(input: Double, output: Double) {
        curvePoints.append(CurveEditorPoint(input: input, output: output))
        curvePoints = Self.normalizedCurvePoints(curvePoints)
        syncLegacyControlsFromCurvePoints()
    }

    public mutating func updateCurvePoint(_ id: CurveEditorPoint.ID, input: Double, output: Double) {
        guard let index = curvePoints.firstIndex(where: { $0.id == id }) else {
            return
        }
        curvePoints[index].input = input
        curvePoints[index].output = output
        curvePoints = Self.normalizedCurvePoints(curvePoints)
        syncLegacyControlsFromCurvePoints()
    }

    public mutating func replaceCurvePoints(_ points: [CurveEditorPoint]) {
        curvePoints = Self.normalizedCurvePoints(points)
        syncLegacyControlsFromCurvePoints()
    }

    public mutating func deleteCurvePoint(_ id: CurveEditorPoint.ID) {
        guard curvePoints.count > 2 else {
            return
        }
        curvePoints.removeAll { $0.id == id }
        curvePoints = Self.normalizedCurvePoints(curvePoints)
        syncLegacyControlsFromCurvePoints()
    }

    public mutating func resetCurve() {
        curveBlackPoint = 0.0
        curveMidInput = 0.5
        curveMidOutput = 0.5
        curveWhitePoint = 1.0
        curveChannel = .rgb
        syncCurvePointsFromLegacyControls()
    }

    private mutating func syncCurvePointsFromLegacyControls() {
        curvePoints = Self.defaultCurvePoints(
            black: curveBlackPoint,
            midInput: curveMidInput,
            midOutput: curveMidOutput,
            white: curveWhitePoint
        )
    }

    private mutating func syncLegacyControlsFromCurvePoints() {
        let points = curveEditorPoints
        curveBlackPoint = points.first?.input ?? 0.0
        curveWhitePoint = points.last?.input ?? 1.0
        let mid = points.min { left, right in
            abs(left.input - 0.5) < abs(right.input - 0.5)
        }
        curveMidInput = mid?.input ?? 0.5
        curveMidOutput = mid?.output ?? 0.5
    }

    private static func defaultCurvePoints(black: Double, midInput: Double, midOutput: Double, white: Double) -> [CurveEditorPoint] {
        [
            CurveEditorPoint(id: UUID(uuid: (0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1)), input: black, output: 0.0),
            CurveEditorPoint(id: UUID(uuid: (0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2)), input: midInput, output: midOutput),
            CurveEditorPoint(id: UUID(uuid: (0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 3)), input: white, output: 1.0),
        ]
    }

    private static func normalizedCurvePoints(_ points: [CurveEditorPoint]) -> [CurveEditorPoint] {
        let source = points.count >= 2 ? points : defaultCurvePoints(black: 0.0, midInput: 0.5, midOutput: 0.5, white: 1.0)
        var normalized = source.map { point in
            CurveEditorPoint(
                id: point.id,
                input: clamp(point.input, lower: 0.0, upper: 1.0),
                output: clamp(point.output, lower: 0.0, upper: 1.0)
            )
        }
        normalized.sort { left, right in
            if approximatelyEqual(left.input, right.input) {
                return left.id.uuidString < right.id.uuidString
            }
            return left.input < right.input
        }

        for index in normalized.indices.dropFirst() where normalized[index].input <= normalized[index - 1].input {
            normalized[index].input = min(normalized[index - 1].input + 0.01, 1.0)
        }
        for index in normalized.indices.dropLast().reversed() where normalized[index].input >= normalized[index + 1].input {
            normalized[index].input = max(normalized[index + 1].input - 0.01, 0.0)
        }
        return normalized
    }
}

public enum MosaicProjection: String, Codable, CaseIterable, Equatable, Identifiable, Sendable {
    case planar
    case cylindrical

    public var id: String {
        rawValue
    }
}

public enum MosaicLayoutMode: String, Codable, CaseIterable, Equatable, Identifiable, Sendable {
    case horizontal
    case grid

    public var id: String {
        rawValue
    }
}

public enum MosaicAlignmentMode: String, Codable, CaseIterable, Equatable, Identifiable, Sendable {
    case manual
    case auto

    public var id: String {
        rawValue
    }
}

public enum MosaicBlendMode: String, Codable, CaseIterable, Equatable, Identifiable, Sendable {
    case average
    case feather
    case multiband

    public var id: String {
        rawValue
    }
}

public enum CurvePreset: String, Codable, CaseIterable, Equatable, Identifiable, Sendable {
    case custom
    case linear
    case softStretch
    case deepSkyContrast
    case shadowLift
    case highlightProtect

    public var id: String {
        rawValue
    }

    public var parameters: CurvePresetParameters? {
        switch self {
        case .custom:
            return nil
        case .linear:
            return CurvePresetParameters(black: 0.0, midInput: 0.5, midOutput: 0.5, white: 1.0)
        case .softStretch:
            return CurvePresetParameters(black: 0.0, midInput: 0.44, midOutput: 0.56, white: 1.0)
        case .deepSkyContrast:
            return CurvePresetParameters(black: 0.0, midInput: 0.40, midOutput: 0.58, white: 1.0)
        case .shadowLift:
            return CurvePresetParameters(black: 0.0, midInput: 0.34, midOutput: 0.62, white: 1.0)
        case .highlightProtect:
            return CurvePresetParameters(black: 0.0, midInput: 0.55, midOutput: 0.60, white: 1.0)
        }
    }

    public func matches(_ parameters: ProcessingParameters) -> Bool {
        guard let preset = self.parameters else {
            return false
        }
        return approximatelyEqual(parameters.curveBlackPoint, preset.black)
            && approximatelyEqual(parameters.curveMidInput, preset.midInput)
            && approximatelyEqual(parameters.curveMidOutput, preset.midOutput)
            && approximatelyEqual(parameters.curveWhitePoint, preset.white)
    }
}

public struct CurvePresetParameters: Codable, Equatable, Sendable {
    public var black: Double
    public var midInput: Double
    public var midOutput: Double
    public var white: Double

    public init(black: Double, midInput: Double, midOutput: Double, white: Double) {
        let normalizedWhite = white.isFinite ? clamp(white, lower: 0.05, upper: 1.0) : 1.0
        let maximumBlack = min(0.95, normalizedWhite - 0.02)
        let normalizedBlack = black.isFinite ? clamp(black, lower: 0.0, upper: maximumBlack) : 0.0
        let fallbackMidInput = (normalizedBlack + normalizedWhite) / 2

        self.black = normalizedBlack
        self.midInput = clamp(
            midInput.isFinite ? midInput : fallbackMidInput,
            lower: normalizedBlack + 0.01,
            upper: normalizedWhite - 0.01
        )
        self.midOutput = midOutput.isFinite ? clamp(midOutput, lower: 0.0, upper: 1.0) : 0.5
        self.white = normalizedWhite
    }

    public init(parameters: ProcessingParameters) {
        let points = parameters.curveControlPoints
        self.black = points[0].input
        self.midInput = points[1].input
        self.midOutput = points[1].output
        self.white = points[2].input
    }

    private enum CodingKeys: String, CodingKey {
        case black
        case midInput
        case midOutput
        case white
    }

    public init(from decoder: Decoder) throws {
        let container = try decoder.container(keyedBy: CodingKeys.self)
        self.init(
            black: try container.decode(Double.self, forKey: .black),
            midInput: try container.decode(Double.self, forKey: .midInput),
            midOutput: try container.decode(Double.self, forKey: .midOutput),
            white: try container.decode(Double.self, forKey: .white)
        )
    }

    public func matches(_ parameters: ProcessingParameters) -> Bool {
        approximatelyEqual(parameters.curveControlPoints[0].input, black)
            && approximatelyEqual(parameters.curveControlPoints[1].input, midInput)
            && approximatelyEqual(parameters.curveControlPoints[1].output, midOutput)
            && approximatelyEqual(parameters.curveControlPoints[2].input, white)
    }
}

public enum CurveControlPointRole: String, Codable, CaseIterable, Equatable, Sendable {
    case black
    case mid
    case white
}

public struct CurveControlPoint: Codable, Equatable, Identifiable, Sendable {
    public var role: CurveControlPointRole
    public var input: Double
    public var output: Double

    public var id: CurveControlPointRole {
        role
    }

    public init(role: CurveControlPointRole, input: Double, output: Double) {
        self.role = role
        self.input = input
        self.output = output
    }
}

public struct HistogramSnapshot: Codable, Equatable, Sendable {
    public var bins: [Double]
    public var minimum: Double
    public var maximum: Double
    public var mean: Double

    public init(bins: [Double] = [], minimum: Double = 0, maximum: Double = 0, mean: Double = 0) {
        self.bins = bins
        self.minimum = minimum
        self.maximum = maximum
        self.mean = mean
    }
}

private func clamp(_ value: Double, lower: Double, upper: Double) -> Double {
    min(max(value, lower), upper)
}

private func approximatelyEqual(_ left: Double, _ right: Double) -> Bool {
    abs(left - right) < 0.000_001
}

public enum StackMethod: String, Codable, CaseIterable, Equatable, Sendable {
    case average
    case weighted
    case median
    case sigma
    case winsorized
    case percentile
}

public enum AlignmentMethod: String, Codable, CaseIterable, Equatable, Sendable {
    case none
    case translation
    case similarity
    case affine
    case distortion
}

public struct ProcessingJob: Identifiable, Codable, Equatable, Sendable {
    public var id: UUID
    public var title: String
    public var kind: ProcessingOperationKind
    public var status: ProcessingJobStatus
    public var createdAt: Date
    public var finishedAt: Date?
    public var outputURL: URL?
    public var message: String
    public var durationMilliseconds: Double?
    public var cacheHit: Bool

    public init(
        id: UUID = UUID(),
        title: String,
        kind: ProcessingOperationKind,
        status: ProcessingJobStatus = .queued,
        createdAt: Date = Date(),
        finishedAt: Date? = nil,
        outputURL: URL? = nil,
        message: String = "",
        durationMilliseconds: Double? = nil,
        cacheHit: Bool = false
    ) {
        self.id = id
        self.title = title
        self.kind = kind
        self.status = status
        self.createdAt = createdAt
        self.finishedAt = finishedAt
        self.outputURL = outputURL
        self.message = message
        self.durationMilliseconds = durationMilliseconds
        self.cacheHit = cacheHit
    }
}

public struct BatchQueueItem: Identifiable, Codable, Equatable, Sendable {
    public var id: UUID
    public var title: String
    public var kind: ProcessingOperationKind
    public var inputURL: URL
    public var outputURL: URL?
    public var status: ProcessingJobStatus
    public var attempts: Int {
        didSet {
            attempts = max(attempts, 0)
        }
    }
    public var message: String

    public init(
        id: UUID = UUID(),
        title: String,
        kind: ProcessingOperationKind,
        inputURL: URL,
        outputURL: URL? = nil,
        status: ProcessingJobStatus = .queued,
        attempts: Int = 0,
        message: String = ""
    ) {
        self.id = id
        self.title = title
        self.kind = kind
        self.inputURL = inputURL
        self.outputURL = outputURL
        self.status = status
        self.attempts = max(attempts, 0)
        self.message = message
    }

    private enum CodingKeys: String, CodingKey {
        case id
        case title
        case kind
        case inputURL
        case outputURL
        case status
        case attempts
        case message
    }

    public init(from decoder: Decoder) throws {
        let container = try decoder.container(keyedBy: CodingKeys.self)
        id = try container.decode(UUID.self, forKey: .id)
        title = try container.decode(String.self, forKey: .title)
        kind = try container.decode(ProcessingOperationKind.self, forKey: .kind)
        inputURL = try container.decode(URL.self, forKey: .inputURL)
        outputURL = try container.decodeIfPresent(URL.self, forKey: .outputURL)
        status = try container.decodeIfPresent(ProcessingJobStatus.self, forKey: .status) ?? .queued
        attempts = max(try container.decodeIfPresent(Int.self, forKey: .attempts) ?? 0, 0)
        message = try container.decodeIfPresent(String.self, forKey: .message) ?? ""
    }

    public mutating func recordAttempt() {
        if attempts < Int.max {
            attempts += 1
        }
    }
}

public enum ProcessingJobStatus: String, Codable, Equatable, Sendable {
    case queued
    case running
    case succeeded
    case failed
    case cancelled
}

public enum ProcessingOperationKind: String, Codable, CaseIterable, Equatable, Sendable {
    case inspect
    case rawDecode
    case preview
    case crop
    case calibrate
    case register
    case stack
    case drizzle
    case meteorRestore
    case cloudRemove
    case background
    case normalize
    case histogram
    case stretch
    case curves
    case localContrast
    case denoise
    case sharpen
    case deconvolve
    case colorNeutralize
    case colorSaturate
    case starDetect
    case starMask
    case starReduce
    case comaReduce
    case artifactDetect
    case artifactRemove
    case cloudDetect
    case mosaic
    case export
}

public struct RecentProject: Identifiable, Codable, Equatable, Sendable {
    public var id: UUID
    public var name: String
    public var directory: URL
    public var openedAt: Date

    public init(id: UUID = UUID(), name: String, directory: URL, openedAt: Date = Date()) {
        self.id = id
        self.name = name
        self.directory = directory
        self.openedAt = openedAt
    }

    private enum CodingKeys: String, CodingKey {
        case id
        case name
        case directory
        case openedAt
    }

    public init(from decoder: Decoder) throws {
        let container = try decoder.container(keyedBy: CodingKeys.self)
        id = try container.decodeIfPresent(UUID.self, forKey: .id) ?? UUID()
        name = try container.decode(String.self, forKey: .name)
        directory = try container.decode(URL.self, forKey: .directory)
        openedAt = try container.decodeIfPresent(Date.self, forKey: .openedAt)
            ?? Date(timeIntervalSince1970: 0)
    }
}

public struct AppSettings: Codable, Equatable, Sendable {
    private static let defaultRecentProjectLimit = 8

    public var recentProjects: [RecentProject]
    public var autosaveEnabled: Bool
    public var selectedTemplate: ProjectTemplate
    public var language: AppLanguage?
    public var customCurvePresets: [CustomCurvePreset]
    public var exportOptions: ExportOptions

    private enum CodingKeys: String, CodingKey {
        case recentProjects
        case autosaveEnabled
        case selectedTemplate
        case language
        case customCurvePresets
        case exportOptions
    }

    public init(
        recentProjects: [RecentProject] = [],
        autosaveEnabled: Bool = true,
        selectedTemplate: ProjectTemplate = .deepSky,
        language: AppLanguage? = nil,
        customCurvePresets: [CustomCurvePreset] = [],
        exportOptions: ExportOptions = ExportOptions()
    ) {
        self.recentProjects = Self.deduplicatedRecentProjects(recentProjects, limit: Self.defaultRecentProjectLimit)
        self.autosaveEnabled = autosaveEnabled
        self.selectedTemplate = selectedTemplate
        self.language = language
        self.customCurvePresets = Self.presetsWithUniqueIdentifiers(customCurvePresets)
        self.exportOptions = exportOptions
    }

    public init(from decoder: Decoder) throws {
        let container = try decoder.container(keyedBy: CodingKeys.self)
        let decodedRecentProjects = try container.decodeIfPresent([RecentProject].self, forKey: .recentProjects) ?? []
        recentProjects = Self.deduplicatedRecentProjects(decodedRecentProjects, limit: Self.defaultRecentProjectLimit)
        autosaveEnabled = try container.decodeIfPresent(Bool.self, forKey: .autosaveEnabled) ?? true
        selectedTemplate = try container.decodeIfPresent(ProjectTemplate.self, forKey: .selectedTemplate) ?? .deepSky
        language = try container.decodeIfPresent(AppLanguage.self, forKey: .language)
        let decodedCurvePresets = try container.decodeIfPresent([CustomCurvePreset].self, forKey: .customCurvePresets) ?? []
        customCurvePresets = Self.presetsWithUniqueIdentifiers(decodedCurvePresets)
        exportOptions = try container.decodeIfPresent(ExportOptions.self, forKey: .exportOptions) ?? ExportOptions()
    }

    public mutating func markRecentProject(name: String, directory: URL, limit: Int = 8) {
        let directoryKey = Self.recentProjectKey(for: directory)
        recentProjects.removeAll { Self.recentProjectKey(for: $0.directory) == directoryKey }
        recentProjects.insert(RecentProject(name: name, directory: directory), at: 0)
        recentProjects = Self.deduplicatedRecentProjects(recentProjects, limit: limit)
    }

    public mutating func deduplicateRecentProjects(limit: Int = 8) {
        recentProjects = Self.deduplicatedRecentProjects(recentProjects, limit: limit)
    }

    private static func deduplicatedRecentProjects(_ projects: [RecentProject], limit: Int) -> [RecentProject] {
        guard limit > 0 else {
            return []
        }
        var seenKeys = Set<String>()
        var deduplicated: [RecentProject] = []
        deduplicated.reserveCapacity(min(projects.count, limit))
        for project in projects {
            let key = recentProjectKey(for: project.directory)
            guard seenKeys.insert(key).inserted else {
                continue
            }
            deduplicated.append(project)
            if deduplicated.count == limit {
                break
            }
        }
        return deduplicated
    }

    private static func recentProjectKey(for directory: URL) -> String {
        var path = directory
            .standardizedFileURL
            .resolvingSymlinksInPath()
            .path
        while path.count > 1, path.hasSuffix("/") {
            path.removeLast()
        }
        return path.lowercased()
    }

    private static func presetsWithUniqueIdentifiers(_ presets: [CustomCurvePreset]) -> [CustomCurvePreset] {
        var identifiers = Set<CustomCurvePreset.ID>()
        return presets.map { preset in
            if identifiers.insert(preset.id).inserted {
                return preset
            }
            var repaired = preset
            repeat {
                repaired.id = UUID()
            } while identifiers.insert(repaired.id).inserted == false
            return repaired
        }
    }
}

public struct CustomCurvePreset: Identifiable, Codable, Equatable, Sendable {
    public var id: UUID
    public var name: String
    public var parameters: CurvePresetParameters
    public var createdAt: Date

    public init(
        id: UUID = UUID(),
        name: String,
        parameters: CurvePresetParameters,
        createdAt: Date = Date()
    ) {
        self.id = id
        self.name = name
        self.parameters = parameters
        self.createdAt = createdAt
    }

    private enum CodingKeys: String, CodingKey {
        case id
        case name
        case parameters
        case createdAt
    }

    public init(from decoder: Decoder) throws {
        let container = try decoder.container(keyedBy: CodingKeys.self)
        id = try container.decodeIfPresent(UUID.self, forKey: .id) ?? UUID()
        name = try container.decode(String.self, forKey: .name)
        parameters = try container.decode(CurvePresetParameters.self, forKey: .parameters)
        createdAt = try container.decodeIfPresent(Date.self, forKey: .createdAt)
            ?? Date(timeIntervalSince1970: 0)
    }

    public func matches(_ parameters: ProcessingParameters) -> Bool {
        self.parameters.matches(parameters)
    }
}

public struct CustomCurvePresetLibrary: Codable, Equatable, Sendable {
    public static let currentSchemaVersion = 1

    public var schemaVersion: Int
    public var presets: [CustomCurvePreset]

    public init(schemaVersion: Int = Self.currentSchemaVersion, presets: [CustomCurvePreset] = []) {
        self.schemaVersion = schemaVersion
        self.presets = presets
    }

    private enum CodingKeys: String, CodingKey {
        case schemaVersion
        case presets
    }

    public init(from decoder: Decoder) throws {
        let container = try decoder.container(keyedBy: CodingKeys.self)
        let schemaVersion = try container.decodeIfPresent(Int.self, forKey: .schemaVersion)
            ?? Self.currentSchemaVersion
        guard schemaVersion == Self.currentSchemaVersion else {
            throw DecodingError.dataCorruptedError(
                forKey: .schemaVersion,
                in: container,
                debugDescription: "Unsupported curve preset schema version \(schemaVersion)"
            )
        }
        self.schemaVersion = schemaVersion
        self.presets = try container.decodeIfPresent([CustomCurvePreset].self, forKey: .presets) ?? []
    }
}
