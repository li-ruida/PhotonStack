import Foundation
import Testing
@testable import PhotonStackAppCore

@Test func projectAddsAssetsAndDetectsKinds() {
    var project = PhotonStackProject(name: "M42")

    project.addAssets(from: [
        URL(fileURLWithPath: "/tmp/light.fit"),
        URL(fileURLWithPath: "/tmp/light.fit"),
        URL(fileURLWithPath: "/tmp/light.dng"),
        URL(fileURLWithPath: "/tmp/mobile.heic"),
        URL(fileURLWithPath: "/tmp/result.tiff"),
    ])

    #expect(project.assets.count == 4)
    #expect(project.assets.map(\.kind) == [.fits, .raw, .heif, .tiff])
    #expect(project.assets.allSatisfy { $0.role == .light })
}

@Test func projectDeduplicatesStandardizedAndSymlinkedAssetPaths() throws {
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackAssetIdentity-\(UUID().uuidString)", isDirectory: true)
    let nestedDirectory = directory.appendingPathComponent("nested", isDirectory: true)
    try FileManager.default.createDirectory(at: nestedDirectory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    let original = directory.appendingPathComponent("light.tiff")
    let standardizedAlias = nestedDirectory.appendingPathComponent("../light.tiff")
    let symlink = directory.appendingPathComponent("light-link.tiff")
    try Data([0x01]).write(to: original)
    try FileManager.default.createSymbolicLink(at: symlink, withDestinationURL: original)
    var project = PhotonStackProject(name: "Asset Identity")

    project.addAssets(from: [original, standardizedAlias, symlink])

    #expect(project.assets.count == 1)
    #expect(project.assets.first?.originalURL == original)
}

@Test func projectRelinkRejectsSymlinkAliasOfAnotherAsset() throws {
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackRelinkIdentity-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    let first = directory.appendingPathComponent("first.tiff")
    let second = directory.appendingPathComponent("second.tiff")
    let secondAlias = directory.appendingPathComponent("second-link.tiff")
    try Data([0x01]).write(to: first)
    try Data([0x02]).write(to: second)
    try FileManager.default.createSymbolicLink(at: secondAlias, withDestinationURL: second)
    var project = PhotonStackProject(name: "Relink Identity")
    project.addAssets(from: [first, second])
    let firstID = try #require(project.assets.first?.id)

    #expect(project.relinkAsset(firstID, to: secondAlias) == false)
    #expect(project.assets.map(\.originalURL) == [first, second])
}

@Test func projectUpdatesAndPersistsAssetRoles() async throws {
    let repository = ProjectRepository()
    let directory = FileManager.default.temporaryDirectory.appendingPathComponent("PhotonStackRoleTests-\(UUID().uuidString)")
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    var project = PhotonStackProject(name: "Calibration Roles")
    project.addAssets(from: [URL(fileURLWithPath: "/tmp/dark.tiff")])
    let assetID = try #require(project.assets.first?.id)

    project.updateRole(for: assetID, role: .dark)
    try await repository.save(project, to: directory)

    let loaded = try await repository.load(from: directory)
    #expect(loaded.assets.first?.role == .dark)
}

@Test func projectRemovesAssetsAndDerivedReferences() {
    var project = PhotonStackProject(name: "Remove Asset")
    project.addAssets(from: [
        URL(fileURLWithPath: "/tmp/remove-a.nef"),
        URL(fileURLWithPath: "/tmp/remove-b.nef"),
    ])
    let removedID = project.assets[0].id
    let keptID = project.assets[1].id
    project.updateRole(for: removedID, role: .mosaic)
    project.updateRole(for: keptID, role: .mosaic)
    project.appendLayer(
        ProcessingLayer(
            name: "Removed Asset Layer",
            kind: .baseImage,
            inputURL: project.assets[0].originalURL,
            parameters: ["source": "asset", "assetID": removedID.uuidString]
        )
    )

    project.removeAsset(removedID)

    #expect(project.assets.map(\.id) == [keptID])
    #expect(project.mosaicPanelOrder == [keptID])
    #expect(project.layers.isEmpty)
}

@Test func projectRemovingSourceSequenceFrameRefreshesSummary() throws {
    let lightURL = URL(fileURLWithPath: "/tmp/source-summary-light.tiff")
    let darkURL = URL(fileURLWithPath: "/tmp/source-summary-dark.tiff")
    var project = PhotonStackProject(name: "Source Summary")
    project.addAssets(from: [lightURL, darkURL])
    let light = try #require(project.assets.first)
    let dark = try #require(project.assets.last)
    project.updateRole(for: dark.id, role: .dark)
    let sourceArtifact = ProcessingArtifact(
        kind: .rawSequence,
        name: "Source Sequence",
        outputURLs: [lightURL, darkURL],
        frames: [
            ProcessingArtifactFrame(
                sourceURL: lightURL,
                outputURL: lightURL,
                metrics: [
                    "assetID": light.id.uuidString,
                    "kind": AssetKind.tiff.rawValue,
                    "role": CalibrationFrameRole.light.rawValue,
                ]
            ),
            ProcessingArtifactFrame(
                sourceURL: darkURL,
                outputURL: darkURL,
                metrics: [
                    "assetID": dark.id.uuidString,
                    "kind": AssetKind.tiff.rawValue,
                    "role": CalibrationFrameRole.dark.rawValue,
                ]
            ),
        ],
        parameters: ["role": "mixed", "source": "import"],
        metrics: ["frames": "2"]
    )
    project.artifacts = [sourceArtifact]

    project.removeAsset(light.id)

    let updated = try #require(project.artifacts.first)
    #expect(updated.id == sourceArtifact.id)
    #expect(updated.frameCount == 1)
    #expect(updated.outputURLs == [darkURL])
    #expect(updated.metrics["frames"] == "1")
    #expect(updated.parameters["role"] == CalibrationFrameRole.dark.rawValue)
}

@Test func projectRemovingUnusedSourceFramePreservesPreciselyScopedDerivatives() throws {
    let firstURL = URL(fileURLWithPath: "/tmp/source-scope-first.tiff")
    let secondURL = URL(fileURLWithPath: "/tmp/source-scope-second.tiff")
    let unusedURL = URL(fileURLWithPath: "/tmp/source-scope-unused.tiff")
    var project = PhotonStackProject(name: "Precise Source Scope")
    project.addAssets(from: [firstURL, secondURL, unusedURL])
    let first = project.assets[0]
    let second = project.assets[1]
    let unused = project.assets[2]
    let source = ProcessingArtifact(
        kind: .rawSequence,
        name: "Source Sequence",
        outputURLs: [firstURL, secondURL, unusedURL],
        frames: [first, second, unused].map { asset in
            ProcessingArtifactFrame(
                sourceURL: asset.originalURL,
                outputURL: asset.originalURL,
                metrics: [
                    "assetID": asset.id.uuidString,
                    "kind": asset.kind.rawValue,
                    "role": asset.role.rawValue,
                ]
            )
        },
        parameters: ["role": CalibrationFrameRole.light.rawValue],
        metrics: ["frames": "3"]
    )
    let registered = ProcessingArtifact(
        kind: .registeredSequence,
        name: "Registered Pair",
        sourceArtifactIDs: [source.id],
        outputURLs: [URL(fileURLWithPath: "/tmp/source-scope-registered.tiff")],
        parameters: [
            "sourceFrameIDs": [first.id, second.id].map(\.uuidString).joined(separator: ","),
        ]
    )
    let edited = ProcessingArtifact(
        kind: .editedImage,
        name: "Edited Pair",
        sourceArtifactIDs: [registered.id],
        outputURLs: [URL(fileURLWithPath: "/tmp/source-scope-edited.tiff")]
    )
    let layer = ProcessingLayer(
        name: "Edited Pair",
        kind: .baseImage,
        sourceArtifactID: edited.id,
        inputURL: edited.outputURLs[0]
    )
    project.artifacts = [source, registered, edited]
    project.layers = [layer]

    project.removeAsset(unused.id)

    #expect(project.artifacts.map(\.id) == [source.id, registered.id, edited.id])
    #expect(project.artifacts[0].frames.map { $0.metrics["assetID"] } == [first.id.uuidString, second.id.uuidString])
    #expect(project.layers.map(\.id) == [layer.id])
}

@Test func persistedWorkspaceProductsDecodeLegacyMissingLifecycleFields() throws {
    let artifactID = UUID()
    let frameID = UUID()
    let artifact = try JSONDecoder().decode(
        ProcessingArtifact.self,
        from: Data(
            """
            {
              "id": "\(artifactID.uuidString)",
              "kind": "rawSequence",
              "name": "Legacy Source",
              "frames": [
                { "id": "\(frameID.uuidString)", "outputURL": "file:///tmp/legacy-frame.tiff" }
              ]
            }
            """.utf8
        )
    )

    #expect(artifact.id == artifactID)
    #expect(artifact.operationKind == nil)
    #expect(artifact.sourceArtifactIDs.isEmpty)
    #expect(artifact.outputURLs.isEmpty)
    #expect(artifact.parameters.isEmpty)
    #expect(artifact.metrics.isEmpty)
    #expect(artifact.createdAt == Date(timeIntervalSince1970: 0))
    #expect(artifact.updatedAt == artifact.createdAt)
    #expect(artifact.frames.first?.id == frameID)
    #expect(artifact.frames.first?.isReference == false)
    #expect(artifact.frames.first?.metrics.isEmpty == true)

    let layerID = UUID()
    let layer = try JSONDecoder().decode(
        ProcessingLayer.self,
        from: Data(
            """
            {
              "id": "\(layerID.uuidString)",
              "name": "Legacy Layer",
              "kind": "baseImage"
            }
            """.utf8
        )
    )
    #expect(layer.blendMode == .normal)
    #expect(layer.opacity == 1)
    #expect(layer.isVisible)
    #expect(layer.parameters.isEmpty)
    #expect(layer.createdAt == Date(timeIntervalSince1970: 0))

    let batchID = UUID()
    let batchItem = try JSONDecoder().decode(
        BatchQueueItem.self,
        from: Data(
            """
            {
              "id": "\(batchID.uuidString)",
              "title": "Legacy Batch",
              "kind": "denoise",
              "inputURL": "file:///tmp/legacy-input.tiff"
            }
            """.utf8
        )
    )
    #expect(batchItem.status == .queued)
    #expect(batchItem.attempts == 0)
    #expect(batchItem.message.isEmpty)

    let operationID = UUID()
    let operation = try JSONDecoder().decode(
        EditOperation.self,
        from: Data(
            """
            {
              "id": "\(operationID.uuidString)",
              "kind": "stretch"
            }
            """.utf8
        )
    )
    #expect(operation.parameters.isEmpty)
    #expect(operation.isEnabled)

    let emptyGraph = try JSONDecoder().decode(EditGraph.self, from: Data("{}".utf8))
    #expect(emptyGraph.operations.isEmpty)
}

@Test func projectRemovingSourceFrameInvalidatesLegacyUnscopedDerivative() throws {
    let firstURL = URL(fileURLWithPath: "/tmp/source-legacy-first.tiff")
    let removedURL = URL(fileURLWithPath: "/tmp/source-legacy-removed.tiff")
    var project = PhotonStackProject(name: "Legacy Source Scope")
    project.addAssets(from: [firstURL, removedURL])
    let first = project.assets[0]
    let removed = project.assets[1]
    let source = ProcessingArtifact(
        kind: .rawSequence,
        name: "Legacy Source Sequence",
        outputURLs: [firstURL, removedURL],
        frames: [first, removed].map { asset in
            ProcessingArtifactFrame(
                sourceURL: asset.originalURL,
                outputURL: asset.originalURL,
                metrics: ["assetID": asset.id.uuidString]
            )
        }
    )
    let legacyDerived = ProcessingArtifact(
        kind: .editedImage,
        name: "Legacy Unscoped Result",
        sourceArtifactIDs: [source.id],
        outputURLs: [URL(fileURLWithPath: "/tmp/source-legacy-result.tiff")]
    )
    project.artifacts = [source, legacyDerived]

    project.removeAsset(removed.id)

    #expect(project.artifacts.map(\.id) == [source.id])
    #expect(project.artifacts.first?.frames.first?.metrics["assetID"] == first.id.uuidString)
}

@Test func projectRemovesAssetReferencesStoredThroughPathAliases() throws {
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackRemoveAlias-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    let removedURL = directory.appendingPathComponent("removed.tiff")
    let removedAlias = directory.appendingPathComponent("removed-link.tiff")
    let keptURL = directory.appendingPathComponent("kept.tiff")
    let operationOutput = directory.appendingPathComponent("removed-output.tiff")
    let operationOutputAlias = directory.appendingPathComponent("removed-output-link.tiff")
    try Data([0x01]).write(to: removedURL)
    try Data([0x02]).write(to: keptURL)
    try Data([0x03]).write(to: operationOutput)
    try FileManager.default.createSymbolicLink(at: removedAlias, withDestinationURL: removedURL)
    try FileManager.default.createSymbolicLink(at: operationOutputAlias, withDestinationURL: operationOutput)

    var project = PhotonStackProject(name: "Remove Aliased Asset")
    project.addAssets(from: [removedURL, keptURL])
    let removed = try #require(project.assets.first { $0.originalURL == removedURL })
    let kept = try #require(project.assets.first { $0.originalURL == keptURL })
    let sourceOperation = EditOperation(
        kind: .stretch,
        parameters: ["input": removedAlias.path, "output": operationOutput.path]
    )
    let descendantOperation = EditOperation(
        kind: .denoise,
        parameters: ["input": operationOutputAlias.path, "output": directory.appendingPathComponent("child.tiff").path]
    )
    project.editGraph = EditGraph(operations: [sourceOperation, descendantOperation])
    project.artifacts = [
        ProcessingArtifact(
            kind: .rawSequence,
            name: "Aliased Source",
            outputURLs: [removedAlias],
            frames: [
                ProcessingArtifactFrame(
                    sourceURL: removedAlias,
                    outputURL: removedAlias,
                    metrics: ["assetID": removed.id.uuidString]
                ),
            ]
        ),
        ProcessingArtifact(
            kind: .editedImage,
            name: "Aliased Descendant",
            outputURLs: [directory.appendingPathComponent("child.tiff")],
            frames: [ProcessingArtifactFrame(sourceURL: operationOutputAlias)]
        ),
    ]
    project.layers = [
        ProcessingLayer(name: "Aliased Layer", kind: .baseImage, inputURL: removedAlias),
    ]
    project.batchQueue = [BatchQueueItem(title: "Aliased Queue", kind: .stretch, inputURL: removedAlias)]
    project.workspaceState = ProjectWorkspaceState(
        selectedAssetID: removed.id,
        previewURL: removedAlias
    )

    project.removeAsset(removed.id)

    #expect(project.assets.map(\.id) == [kept.id])
    #expect(project.editGraph.operations.isEmpty)
    #expect(project.artifacts.isEmpty)
    #expect(project.layers.isEmpty)
    #expect(project.batchQueue.isEmpty)
    #expect(project.workspaceState?.selectedAssetID == kept.id)
    #expect(project.workspaceState?.previewURL == keptURL)
}

@Test func projectRemovesAssetAcrossPersistedProcessingState() throws {
    let removedURL = URL(fileURLWithPath: "/tmp/remove-persisted-a.nef")
    let keptURL = URL(fileURLWithPath: "/tmp/remove-persisted-b.nef")
    let removedOutput = URL(fileURLWithPath: "/tmp/remove-persisted-a-stretch.tiff")
    let keptOutput = URL(fileURLWithPath: "/tmp/remove-persisted-b-stretch.tiff")
    var project = PhotonStackProject(name: "Remove Persisted Asset")
    project.addAssets(from: [removedURL, keptURL])
    let removed = try #require(project.assets.first { $0.originalURL == removedURL })
    let kept = try #require(project.assets.first { $0.originalURL == keptURL })
    let removedOperation = EditOperation(
        kind: .stretch,
        parameters: [
            "assetID": removed.id.uuidString,
            "input": removedURL.path,
            "output": removedOutput.path,
        ]
    )
    let removedDescendant = EditOperation(
        kind: .denoise,
        parameters: [
            "input": removedOutput.path,
            "output": "/tmp/remove-persisted-a-denoise.tiff",
        ]
    )
    let keptOperation = EditOperation(
        kind: .stretch,
        parameters: [
            "assetID": kept.id.uuidString,
            "input": keptURL.path,
            "output": keptOutput.path,
        ]
    )
    project.editGraph = EditGraph(operations: [removedOperation, removedDescendant, keptOperation])

    let removedArtifact = ProcessingArtifact(
        kind: .editedImage,
        name: "Removed Edit",
        operationKind: .stretch,
        outputURLs: [removedOutput],
        frames: [
            ProcessingArtifactFrame(
                sourceURL: removedURL,
                outputURL: removedOutput,
                metrics: ["assetID": removed.id.uuidString]
            ),
        ],
        parameters: ["input": removedURL.path]
    )
    let keptArtifact = ProcessingArtifact(
        kind: .editedImage,
        name: "Kept Edit",
        operationKind: .stretch,
        outputURLs: [keptOutput],
        frames: [
            ProcessingArtifactFrame(
                sourceURL: keptURL,
                outputURL: keptOutput,
                metrics: ["assetID": kept.id.uuidString]
            ),
        ],
        parameters: ["input": keptURL.path]
    )
    let removedArtifactDescendant = ProcessingArtifact(
        kind: .editedImage,
        name: "Removed Edit Descendant",
        operationKind: .denoise,
        outputURLs: [URL(fileURLWithPath: "/tmp/remove-persisted-a-artifact-child.tiff")],
        frames: [
            ProcessingArtifactFrame(
                sourceURL: removedOutput,
                outputURL: URL(fileURLWithPath: "/tmp/remove-persisted-a-artifact-child.tiff")
            ),
        ]
    )
    project.artifacts = [removedArtifact, removedArtifactDescendant, keptArtifact]
    let removedLayer = ProcessingLayer(
        name: "Removed Artifact Layer",
        kind: .adjustment,
        sourceArtifactID: removedArtifact.id,
        inputURL: removedOutput,
        parameters: ["operationID": removedDescendant.id.uuidString]
    )
    let keptLayer = ProcessingLayer(
        name: "Kept Artifact Layer",
        kind: .adjustment,
        sourceArtifactID: keptArtifact.id,
        inputURL: keptOutput,
        parameters: ["operationID": keptOperation.id.uuidString]
    )
    project.layers = [removedLayer, keptLayer]
    project.batchQueue = [
        BatchQueueItem(title: "Removed", kind: .stretch, inputURL: removedURL),
        BatchQueueItem(title: "Kept", kind: .stretch, inputURL: keptURL),
    ]
    project.workspaceState = ProjectWorkspaceState(
        selectedAssetID: removed.id,
        activeLayerID: removedLayer.id,
        previewURL: removedOutput,
        previewOperationID: removedDescendant.id,
        editGraphNeedsReplay: false
    )

    project.removeAsset(removed.id)

    #expect(project.assets.map(\.id) == [kept.id])
    #expect(project.editGraph.operations.map(\.id) == [keptOperation.id])
    #expect(project.artifacts.map(\.id) == [keptArtifact.id])
    #expect(project.layers.map(\.id) == [keptLayer.id])
    #expect(project.batchQueue.map(\.inputURL) == [keptURL])
    #expect(project.workspaceState?.selectedAssetID == kept.id)
    #expect(project.workspaceState?.activeLayerID == nil)
    #expect(project.workspaceState?.previewURL == keptURL)
    #expect(project.workspaceState?.previewOperationID == nil)
}

@Test func removingMaskSourceAssetDetachesMaskWithoutDeletingUnrelatedLayer() throws {
    let maskSourceURL = URL(fileURLWithPath: "/tmp/remove-mask-source.nef")
    let imageURL = URL(fileURLWithPath: "/tmp/keep-masked-image.nef")
    var project = PhotonStackProject(name: "Detach Removed Mask")
    project.addAssets(from: [maskSourceURL, imageURL])
    let maskSource = try #require(project.assets.first { $0.originalURL == maskSourceURL })
    let imageAsset = try #require(project.assets.first { $0.originalURL == imageURL })
    let maskArtifact = ProcessingArtifact(
        kind: .mask,
        name: "Asset Mask",
        operationKind: .starMask,
        outputURLs: [URL(fileURLWithPath: "/tmp/remove-mask-source-mask.tiff")],
        frames: [
            ProcessingArtifactFrame(
                sourceURL: maskSourceURL,
                outputURL: URL(fileURLWithPath: "/tmp/remove-mask-source-mask.tiff"),
                metrics: ["assetID": maskSource.id.uuidString]
            ),
        ]
    )
    let keptLayer = ProcessingLayer(
        name: "Kept Image",
        kind: .baseImage,
        inputURL: imageURL,
        maskArtifactID: maskArtifact.id,
        parameters: ["assetID": imageAsset.id.uuidString]
    )
    project.artifacts = [maskArtifact]
    project.layers = [keptLayer]

    project.removeAsset(maskSource.id)

    #expect(project.assets.map(\.id) == [imageAsset.id])
    #expect(project.artifacts.isEmpty)
    #expect(project.layers.map(\.id) == [keptLayer.id])
    #expect(project.layers.first?.maskArtifactID == nil)
}

@Test func projectRemovesArtifactDependencyClosureWithoutDeletingIndependentState() throws {
    let sourceURL = URL(fileURLWithPath: "/tmp/remove-artifact-source.tiff")
    let targetURL = URL(fileURLWithPath: "/tmp/remove-artifact-target.tiff")
    let childURL = URL(fileURLWithPath: "/tmp/remove-artifact-child.tiff")
    let pathChildURL = URL(fileURLWithPath: "/tmp/remove-artifact-path-child.tiff")
    let keptURL = URL(fileURLWithPath: "/tmp/remove-artifact-kept.tiff")
    var project = PhotonStackProject(name: "Remove Artifact")
    project.addAssets(from: [sourceURL])
    let asset = try #require(project.assets.first)
    let target = ProcessingArtifact(
        kind: .mask,
        name: "Target Mask",
        outputURLs: [targetURL]
    )
    let child = ProcessingArtifact(
        kind: .editedImage,
        name: "ID Child",
        sourceArtifactIDs: [target.id],
        outputURLs: [childURL]
    )
    let pathChild = ProcessingArtifact(
        kind: .editedImage,
        name: "Path Child",
        outputURLs: [pathChildURL],
        frames: [ProcessingArtifactFrame(sourceURL: childURL, outputURL: pathChildURL)]
    )
    let kept = ProcessingArtifact(
        kind: .editedImage,
        name: "Kept",
        outputURLs: [keptURL]
    )
    project.artifacts = [target, child, pathChild, kept]

    let targetLayer = ProcessingLayer(
        name: "Target Layer",
        kind: .mask,
        sourceArtifactID: target.id,
        inputURL: targetURL
    )
    let childLayer = ProcessingLayer(
        name: "Child Layer",
        kind: .adjustment,
        inputURL: childURL,
        parameters: ["sourceLayerID": targetLayer.id.uuidString]
    )
    let keptLayer = ProcessingLayer(
        name: "Kept Layer",
        kind: .baseImage,
        sourceArtifactID: kept.id,
        inputURL: keptURL,
        maskArtifactID: target.id,
        parameters: ["assetID": asset.id.uuidString]
    )
    project.layers = [targetLayer, childLayer, keptLayer]
    project.batchQueue = [
        BatchQueueItem(title: "Removed", kind: .denoise, inputURL: childURL),
        BatchQueueItem(title: "Kept", kind: .sharpen, inputURL: keptURL),
    ]
    let operation = EditOperation(kind: .stretch, parameters: ["output": targetURL.path])
    project.editGraph = EditGraph(operations: [operation])
    project.workspaceState = ProjectWorkspaceState(
        selectedAssetID: asset.id,
        activeLayerID: targetLayer.id,
        previewURL: pathChildURL,
        previewOperationID: operation.id,
        editGraphNeedsReplay: true
    )

    project.removeArtifact(target.id)

    #expect(project.artifacts.map(\.id) == [kept.id])
    #expect(project.layers.map(\.id) == [keptLayer.id])
    #expect(project.layers.first?.maskArtifactID == nil)
    #expect(project.batchQueue.map(\.inputURL) == [keptURL])
    #expect(project.editGraph.operations.map(\.id) == [operation.id])
    #expect(project.workspaceState?.activeLayerID == nil)
    #expect(project.workspaceState?.previewURL == sourceURL)
    #expect(project.workspaceState?.previewOperationID == nil)
    #expect(project.workspaceState?.editGraphNeedsReplay == false)
}

@Test func projectRemovesArtifactReferencesStoredThroughPathAliases() throws {
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackRemoveArtifactAlias-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    let sourceURL = directory.appendingPathComponent("source.tiff")
    let targetURL = directory.appendingPathComponent("target.tiff")
    let targetAlias = directory.appendingPathComponent("target-link.tiff")
    let childURL = directory.appendingPathComponent("child.tiff")
    try Data([0x01]).write(to: sourceURL)
    try Data([0x02]).write(to: targetURL)
    try FileManager.default.createSymbolicLink(at: targetAlias, withDestinationURL: targetURL)

    var project = PhotonStackProject(name: "Remove Aliased Artifact")
    project.addAssets(from: [sourceURL])
    let asset = try #require(project.assets.first)
    let target = ProcessingArtifact(kind: .mask, name: "Target", outputURLs: [targetURL])
    let child = ProcessingArtifact(
        kind: .editedImage,
        name: "Aliased Child",
        outputURLs: [childURL],
        frames: [ProcessingArtifactFrame(sourceURL: targetAlias, outputURL: childURL)]
    )
    project.artifacts = [target, child]
    project.layers = [ProcessingLayer(name: "Aliased Layer", kind: .mask, inputURL: targetAlias)]
    project.batchQueue = [BatchQueueItem(title: "Aliased Queue", kind: .denoise, inputURL: targetAlias)]
    project.workspaceState = ProjectWorkspaceState(
        selectedAssetID: asset.id,
        previewURL: targetAlias
    )

    project.removeArtifact(target.id)

    #expect(project.artifacts.isEmpty)
    #expect(project.layers.isEmpty)
    #expect(project.batchQueue.isEmpty)
    #expect(project.workspaceState?.previewURL == sourceURL)
}

@Test func projectRelinksAssetAcrossEveryPersistedReference() throws {
    let oldURL = URL(fileURLWithPath: "/tmp/moved-original.nef")
    let replacementURL = URL(fileURLWithPath: "/tmp/relinked-original.tiff")
    var project = PhotonStackProject(name: "Relink Asset")
    project.addAssets(from: [oldURL])
    let assetID = try #require(project.assets.first?.id)
    project.assets[0].metadata = AssetMetadata(width: 6048, height: 4032)
    project.appendLayer(
        ProcessingLayer(
            name: oldURL.lastPathComponent,
            kind: .baseImage,
            inputURL: oldURL,
            parameters: [
                "source": "asset",
                "assetID": assetID.uuidString,
                "input": oldURL.path,
            ]
        )
    )
    project.appendArtifact(
        ProcessingArtifact(
            kind: .rawSequence,
            name: oldURL.lastPathComponent,
            outputURLs: [oldURL],
            frames: [
                ProcessingArtifactFrame(
                    sourceURL: oldURL,
                    outputURL: oldURL,
                    metrics: [
                        "assetID": assetID.uuidString,
                        "sourcePath": oldURL.path,
                        "kind": AssetKind.raw.rawValue,
                        "role": CalibrationFrameRole.light.rawValue,
                    ]
                ),
            ],
            parameters: ["inputPath.0": oldURL.path, "role": CalibrationFrameRole.light.rawValue],
            metrics: ["frames": "1"]
        )
    )
    project.appendArtifact(
        ProcessingArtifact(
            kind: .editedImage,
            name: oldURL.lastPathComponent,
            outputURLs: [URL(fileURLWithPath: "/tmp/unrelated-output.tiff")]
        )
    )
    project.appendOperation(
        EditOperation(
            kind: .rawDecode,
            parameters: [
                "assetID": assetID.uuidString,
                "input": oldURL.path,
                "sourcePath": oldURL.path,
            ]
        )
    )
    project.replaceBatchQueue([
        BatchQueueItem(title: "Relink", kind: .stretch, inputURL: oldURL),
    ])
    project.workspaceState = ProjectWorkspaceState(
        selectedAssetID: assetID,
        previewURL: oldURL
    )

    let didRelink = project.relinkAsset(assetID, to: replacementURL)
    #expect(didRelink)
    #expect(project.assets[0].originalURL == replacementURL)
    #expect(project.assets[0].kind == .tiff)
    #expect(project.assets[0].metadata == nil)
    #expect(project.layers[0].name == replacementURL.lastPathComponent)
    #expect(project.layers[0].inputURL == replacementURL)
    #expect(project.layers[0].parameters["input"] == replacementURL.path)
    #expect(project.artifacts[0].name == replacementURL.lastPathComponent)
    #expect(project.artifacts[0].outputURLs == [replacementURL])
    #expect(project.artifacts[0].frames[0].sourceURL == replacementURL)
    #expect(project.artifacts[0].frames[0].outputURL == replacementURL)
    #expect(project.artifacts[0].frames[0].metrics["sourcePath"] == replacementURL.path)
    #expect(project.artifacts[0].frames[0].metrics["kind"] == AssetKind.tiff.rawValue)
    #expect(project.artifacts[0].metrics["frames"] == "1")
    #expect(project.artifacts[0].parameters["role"] == CalibrationFrameRole.light.rawValue)
    #expect(project.artifacts[0].parameters["inputPath.0"] == replacementURL.path)
    #expect(project.artifacts[1].name == oldURL.lastPathComponent)
    #expect(project.artifacts[1].outputURLs == [URL(fileURLWithPath: "/tmp/unrelated-output.tiff")])
    #expect(project.editGraph.operations[0].parameters["input"] == replacementURL.path)
    #expect(project.editGraph.operations[0].parameters["sourcePath"] == replacementURL.path)
    #expect(project.batchQueue[0].inputURL == replacementURL)
    #expect(project.workspaceState?.previewURL == replacementURL)
}

@Test func projectRelinksAssetReferencesStoredThroughPathAliases() throws {
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackRelinkAlias-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    let oldURL = directory.appendingPathComponent("old.tiff")
    let oldAlias = directory.appendingPathComponent("old-link.tiff")
    let replacementURL = directory.appendingPathComponent("replacement.tiff")
    try Data([0x01]).write(to: oldURL)
    try Data([0x02]).write(to: replacementURL)
    try FileManager.default.createSymbolicLink(at: oldAlias, withDestinationURL: oldURL)

    var project = PhotonStackProject(name: "Relink Aliased Asset")
    project.addAssets(from: [oldURL])
    let assetID = try #require(project.assets.first?.id)
    project.layers = [
        ProcessingLayer(
            name: oldURL.lastPathComponent,
            kind: .baseImage,
            inputURL: oldAlias,
            parameters: ["assetID": assetID.uuidString, "input": oldAlias.path]
        ),
    ]
    project.artifacts = [
        ProcessingArtifact(
            kind: .rawSequence,
            name: oldURL.lastPathComponent,
            outputURLs: [oldAlias],
            frames: [
                ProcessingArtifactFrame(
                    sourceURL: oldAlias,
                    outputURL: oldAlias,
                    metrics: ["assetID": assetID.uuidString, "sourcePath": oldAlias.path]
                ),
            ],
            parameters: ["inputPath.0": oldAlias.path]
        ),
    ]
    project.editGraph = EditGraph(operations: [
        EditOperation(kind: .rawDecode, parameters: ["input": oldAlias.path]),
    ])
    project.batchQueue = [BatchQueueItem(title: "Aliased Queue", kind: .stretch, inputURL: oldAlias)]
    project.workspaceState = ProjectWorkspaceState(selectedAssetID: assetID, previewURL: oldAlias)

    let didRelink = project.relinkAsset(assetID, to: replacementURL)
    #expect(didRelink)
    #expect(project.assets.first?.originalURL == replacementURL)
    #expect(project.layers.first?.inputURL == replacementURL)
    #expect(project.layers.first?.parameters["input"] == replacementURL.path)
    #expect(project.artifacts.first?.outputURLs == [replacementURL])
    #expect(project.artifacts.first?.frames.first?.sourceURL == replacementURL)
    #expect(project.artifacts.first?.frames.first?.outputURL == replacementURL)
    #expect(project.artifacts.first?.frames.first?.metrics["sourcePath"] == replacementURL.path)
    #expect(project.artifacts.first?.parameters["inputPath.0"] == replacementURL.path)
    #expect(project.editGraph.operations.first?.parameters["input"] == replacementURL.path)
    #expect(project.batchQueue.first?.inputURL == replacementURL)
    #expect(project.workspaceState?.previewURL == replacementURL)
}

@Test func projectRejectsRelinkToAnotherAssetPath() throws {
    let firstURL = URL(fileURLWithPath: "/tmp/relink-first.nef")
    let secondURL = URL(fileURLWithPath: "/tmp/relink-second.nef")
    var project = PhotonStackProject(name: "Duplicate Relink")
    project.addAssets(from: [firstURL, secondURL])
    let firstID = try #require(project.assets.first?.id)

    let didRelink = project.relinkAsset(firstID, to: secondURL)
    #expect(didRelink == false)
    #expect(project.assets.map(\.originalURL) == [firstURL, secondURL])
}

@Test func projectPersistsMosaicPanelOrder() async throws {
    let repository = ProjectRepository()
    let directory = FileManager.default.temporaryDirectory.appendingPathComponent("PhotonStackMosaicOrderTests-\(UUID().uuidString)")
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    var project = PhotonStackProject(name: "Mosaic")
    project.addAssets(from: [
        URL(fileURLWithPath: "/tmp/panel-a.tiff"),
        URL(fileURLWithPath: "/tmp/panel-b.tiff"),
    ])
    let firstID = try #require(project.assets.first?.id)
    let secondID = try #require(project.assets.dropFirst().first?.id)
    project.updateRole(for: firstID, role: .mosaic)
    project.updateRole(for: secondID, role: .mosaic)
    project.mosaicPanelOrder = [secondID, firstID]

    try await repository.save(project, to: directory)
    let loaded = try await repository.load(from: directory)

    #expect(loaded.mosaicPanelOrder == [secondID, firstID])
}

@Test func projectPersistsArtifactsAndLayers() async throws {
    let repository = ProjectRepository()
    let directory = FileManager.default.temporaryDirectory.appendingPathComponent("PhotonStackArtifactTests-\(UUID().uuidString)")
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    let artifact = ProcessingArtifact(
        kind: .registeredSequence,
        name: "Registered Orion",
        operationKind: .register,
        outputDirectory: URL(fileURLWithPath: "/tmp/registered", isDirectory: true),
        outputURLs: [URL(fileURLWithPath: "/tmp/registered/aligned-001.tiff")],
        frames: [
            ProcessingArtifactFrame(
                sourceURL: URL(fileURLWithPath: "/tmp/light-001.nef"),
                outputURL: URL(fileURLWithPath: "/tmp/registered/aligned-001.tiff"),
                isReference: true,
                metrics: ["matches": "0"]
            ),
        ],
        metrics: ["frames": "1"]
    )
    let layer = ProcessingLayer(
        name: "Aligned Preview",
        kind: .baseImage,
        sourceArtifactID: artifact.id,
        inputURL: artifact.previewURL,
        blendMode: .normal,
        opacity: 0.75
    )
    var project = PhotonStackProject(name: "Products")
    project.appendArtifact(artifact)
    project.appendLayer(layer)

    try await repository.save(project, to: directory)
    let loaded = try await repository.load(from: directory)

    #expect(loaded.artifacts.first?.kind == .registeredSequence)
    #expect(loaded.artifacts.first?.frameCount == 1)
    #expect(loaded.layers.first?.sourceArtifactID == artifact.id)
    #expect(loaded.layers.first?.opacity == 0.75)
}

@Test func processingArtifactSeparatesScientificDataFromDisplayPreview() {
    let scientificURL = URL(fileURLWithPath: "/tmp/registered/aligned-001.fits")
    let previewURL = URL(fileURLWithPath: "/tmp/registered/aligned-001-preview.png")
    let artifact = ProcessingArtifact(
        kind: .registeredSequence,
        name: "Registered Scientific Sequence",
        outputURLs: [scientificURL],
        parameters: ["previewPath": previewURL.path]
    )

    #expect(artifact.scientificURL == scientificURL)
    #expect(artifact.previewURL == previewURL)
}

@Test func processingLayerDecodingClampsPersistedOpacity() throws {
    let encoder = JSONEncoder()
    let decoder = JSONDecoder()
    let layer = ProcessingLayer(name: "Persisted Opacity", kind: .adjustment)
    let encoded = try encoder.encode(layer)
    var object = try #require(JSONSerialization.jsonObject(with: encoded) as? [String: Any])
    object["opacity"] = 1.75
    let decodedHigh = try decoder.decode(ProcessingLayer.self, from: JSONSerialization.data(withJSONObject: object))
    object["opacity"] = -0.25
    let decodedLow = try decoder.decode(ProcessingLayer.self, from: JSONSerialization.data(withJSONObject: object))

    #expect(decodedHigh.opacity == 1.0)
    #expect(decodedLow.opacity == 0.0)

    var mutated = layer
    mutated.opacity = .nan
    #expect(mutated.opacity == 1.0)
    mutated.opacity = -3.0
    #expect(mutated.opacity == 0.0)
}

@Test func batchQueueAttemptCountRemainsNonnegativeAndSaturates() throws {
    var item = BatchQueueItem(
        title: "Saturating Attempt Count",
        kind: .stretch,
        inputURL: URL(fileURLWithPath: "/tmp/saturating-attempt.tiff"),
        attempts: -4
    )
    #expect(item.attempts == 0)

    item.attempts = -1
    #expect(item.attempts == 0)
    item.attempts = Int.max
    item.recordAttempt()
    #expect(item.attempts == Int.max)

    let decoded = try JSONDecoder().decode(BatchQueueItem.self, from: JSONEncoder().encode(item))
    #expect(decoded.attempts == Int.max)
}

@Test func mosaicPlanEstimatesHorizontalOutputSize() {
    let assets = [
        PhotonStackAsset(
            originalURL: URL(fileURLWithPath: "/tmp/panel-a.tiff"),
            kind: .tiff,
            role: .mosaic,
            metadata: AssetMetadata(width: 1200, height: 800)
        ),
        PhotonStackAsset(
            originalURL: URL(fileURLWithPath: "/tmp/panel-b.tiff"),
            kind: .tiff,
            role: .mosaic,
            metadata: AssetMetadata(width: 1000, height: 800)
        ),
    ]

    let plan = MosaicPlanSummary(assets: assets, overlapPixels: 120)

    #expect(plan.estimatedWidth == 2080)
    #expect(plan.estimatedHeight == 800)
    #expect(plan.missingMetadataCount == 0)
    #expect(plan.hasHeightMismatch == false)
    #expect(plan.suggestedOverlapPixels == 150)

    let clampedPlan = MosaicPlanSummary(assets: assets, overlapPixels: 10_000)
    #expect(clampedPlan.estimatedWidth == 1201)

    let gridPlan = MosaicPlanSummary(assets: assets + assets, overlapPixels: 120, layout: .grid, columns: 2)
    #expect(gridPlan.estimatedWidth == 2080)
    #expect(gridPlan.estimatedHeight == 1480)
}

@Test func mosaicPlanRejectsInvalidAndOverflowingMetadataWithoutTrapping() {
    let first = PhotonStackAsset(
        originalURL: URL(fileURLWithPath: "/tmp/mosaic-overflow-a.tiff"),
        kind: .tiff,
        role: .mosaic,
        metadata: AssetMetadata(width: Int.max, height: 100)
    )
    let second = PhotonStackAsset(
        originalURL: URL(fileURLWithPath: "/tmp/mosaic-overflow-b.tiff"),
        kind: .tiff,
        role: .mosaic,
        metadata: AssetMetadata(width: Int.max, height: 100)
    )
    let overflowing = MosaicPlanSummary(assets: [first, second], overlapPixels: 0)

    #expect(overflowing.missingMetadataCount == 0)
    #expect(overflowing.estimatedWidth == nil)
    #expect(overflowing.estimatedHeight == 100)

    var invalid = first
    invalid.metadata = AssetMetadata(width: -1, height: 0)
    let invalidPlan = MosaicPlanSummary(assets: [invalid], overlapPixels: 0)
    #expect(invalidPlan.missingMetadataCount == 1)
    #expect(invalidPlan.estimatedWidth == nil)
    #expect(invalidPlan.estimatedHeight == nil)
}

@Test func mosaicPlanReportsMissingMetadataAndHeightMismatch() {
    let missingPlan = MosaicPlanSummary(
        assets: [
            PhotonStackAsset(originalURL: URL(fileURLWithPath: "/tmp/panel-a.tiff"), kind: .tiff, role: .mosaic),
        ],
        overlapPixels: 64
    )

    #expect(missingPlan.estimatedWidth == nil)
    #expect(missingPlan.missingMetadataCount == 1)
    #expect(missingPlan.suggestedOverlapPixels == nil)

    let mismatchPlan = MosaicPlanSummary(
        assets: [
            PhotonStackAsset(
                originalURL: URL(fileURLWithPath: "/tmp/panel-a.tiff"),
                kind: .tiff,
                role: .mosaic,
                metadata: AssetMetadata(width: 1000, height: 800)
            ),
            PhotonStackAsset(
                originalURL: URL(fileURLWithPath: "/tmp/panel-b.tiff"),
                kind: .tiff,
                role: .mosaic,
                metadata: AssetMetadata(width: 1000, height: 820)
            ),
        ],
        overlapPixels: 64
    )

    #expect(mismatchPlan.estimatedWidth == 1936)
    #expect(mismatchPlan.estimatedHeight == 820)
    #expect(mismatchPlan.hasHeightMismatch)
    #expect(mismatchPlan.suggestedOverlapPixels == 150)
}

@Test func localizationSupportsEnglishAndSimplifiedChinese() {
    #expect(AppLanguage.english.text(.importAction) == "Import")
    #expect(AppLanguage.simplifiedChinese.text(.importAction) == "导入")
    #expect(AppLanguage.simplifiedChinese.roleName(.light) == "亮场")
    #expect(AppLanguage.simplifiedChinese.roleName(.mosaic) == "接片")
    #expect(AppLanguage.simplifiedChinese.alignmentMethodName(.translation) == "平移")
    #expect(AppLanguage.simplifiedChinese.alignmentMethodName(.distortion) == "畸变分区")
    #expect(AppLanguage.simplifiedChinese.curvePresetName(.deepSkyContrast) == "深空对比")
    #expect(AppLanguage.simplifiedChinese.text(.importCurvePresets) == "导入预设")
    #expect(AppLanguage.simplifiedChinese.text(.openProjectMessage) == "选择一个 PhotonStack 项目文件夹。")
    #expect(AppLanguage.simplifiedChinese.text(.exportImageMessage) == "选择最终成片的文件名和格式。")
    #expect(AppLanguage.simplifiedChinese.text(.chooseBatchOutputDirectoryMessage) == "选择批处理成片的输出文件夹。")
    #expect(AppLanguage.simplifiedChinese.text(.batchOutputDirectory) == "输出目录")
    #expect(AppLanguage.simplifiedChinese.text(.batchRegistrationFrames) == "参与对齐")
    #expect(AppLanguage.simplifiedChinese.text(.toolsPanel) == "工具")
    #expect(AppLanguage.simplifiedChinese.text(.inspectorWorkspaceGroup) == "操作区")
    #expect(AppLanguage.simplifiedChinese.text(.inspectorAdjustmentsGroup) == "图像调整")
    #expect(AppLanguage.simplifiedChinese.text(.exportSettingsSection) == "导出设置")
    #expect(AppLanguage.simplifiedChinese.text(.exportSidecarFailed).contains("图像已导出"))
    #expect(AppLanguage.simplifiedChinese.exportColorSpaceName(.linearSRGB) == "线性 sRGB")
    #expect(AppLanguage.simplifiedChinese.rawWhiteBalanceModeName(.camera) == "相机")
    #expect(AppLanguage.simplifiedChinese.rawDemosaicQualityName(.high) == "完整质量")
    #expect(AppLanguage.simplifiedChinese.text(.rawMetadataSection) == "相机元数据")
    #expect(AppLanguage.simplifiedChinese.text(.rawAppliedParameters).contains("预览"))
    #expect(AppLanguage.simplifiedChinese.batchOutputFormatName(.tiff) == "TIFF")
    #expect(AppLanguage.simplifiedChinese.artifactKindName(.registeredSequence) == "已对齐序列")
    #expect(AppLanguage.simplifiedChinese.text(.workspaceObjectsTab) == "智能对象")
    #expect(AppLanguage.simplifiedChinese.text(.addLayerMenu) == "添加图层")
    #expect(AppLanguage.simplifiedChinese.text(.activeLayerLabel) == "活动图层")
    #expect(AppLanguage.simplifiedChinese.text(.hideLayer) == "隐藏图层")
    #expect(AppLanguage.simplifiedChinese.layerKindName(.baseImage) == "基础图层")
    #expect(AppLanguage.simplifiedChinese.layerBlendModeName(.screen) == "滤色")
    #expect(AppLanguage.simplifiedChinese.text(.mosaicEstimatedSize) == "预计尺寸")
    #expect(AppLanguage.simplifiedChinese.text(.estimateOverlap) == "估算重叠")
    #expect(AppLanguage.simplifiedChinese.mosaicProjectionName(.cylindrical) == "圆柱")
    #expect(AppLanguage.simplifiedChinese.mosaicLayoutName(.grid) == "网格")
    #expect(AppLanguage.simplifiedChinese.mosaicAlignmentName(.auto) == "自动仿射")
    #expect(AppLanguage.simplifiedChinese.mosaicBlendModeName(.feather) == "羽化")
}

@Test func processingParametersIncludeMosaicDefaults() {
    let parameters = ProcessingParameters()
    #expect(parameters.mosaicOverlapPixels == 0)
    #expect(parameters.mosaicProjection == .planar)
    #expect(parameters.mosaicLayout == .horizontal)
    #expect(parameters.mosaicAlignment == .manual)
    #expect(parameters.mosaicBlendMode == .feather)
    #expect(parameters.mosaicExposureMatching)
    #expect(parameters.mosaicColumns == 2)
    #expect(parameters.mosaicPreviewWidth == 1200)
    #expect(parameters.curveMidOutput == 0.5)
    #expect(parameters.rawProcessingOptions.whiteBalanceMode == .camera)
    #expect(parameters.rawProcessingOptions.manualWhiteBalanceTemperature == 6500)
    #expect(parameters.rawProcessingOptions.manualWhiteBalanceTint == 0)
    #expect(parameters.rawProcessingOptions.blackLevelMode == .camera)
    #expect(parameters.rawProcessingOptions.manualBlackLevel == 0)
    #expect(parameters.rawProcessingOptions.demosaicQuality == .high)
    #expect(parameters.rawProcessingOptions.linearOutput)
    #expect(parameters.workflowDarkBiasState == .included)
    #expect(parameters.workflowFlatBiasState == .included)
}

@Test func calibrationBiasStatesDecodeLegacyDefaultsAndRoundTrip() throws {
    let legacy = try JSONDecoder().decode(ProcessingParameters.self, from: Data("{}".utf8))
    #expect(legacy.workflowDarkBiasState == .included)
    #expect(legacy.workflowFlatBiasState == .included)

    let requested = ProcessingParameters(
        workflowDarkBiasState: .removed,
        workflowFlatBiasState: .removed
    )
    let decoded = try JSONDecoder().decode(
        ProcessingParameters.self,
        from: JSONEncoder().encode(requested)
    )
    #expect(decoded.workflowDarkBiasState == .removed)
    #expect(decoded.workflowFlatBiasState == .removed)
}

@Test func rawProcessingOptionsDecodeLegacyDefaultsAndRoundTripManualValues() throws {
    let legacy = Data(
        """
        {
          "whiteBalanceMode": "manual",
          "exposureBias": 0.5,
          "blackLevelMode": "manual",
          "demosaicQuality": "high",
          "linearOutput": true
        }
        """.utf8
    )
    let legacyOptions = try JSONDecoder().decode(RawProcessingOptions.self, from: legacy)
    #expect(legacyOptions.manualWhiteBalanceTemperature == 6500)
    #expect(legacyOptions.manualWhiteBalanceTint == 0)
    #expect(legacyOptions.manualBlackLevel == 0)

    let manual = RawProcessingOptions(
        whiteBalanceMode: .manual,
        manualWhiteBalanceTemperature: 7200,
        manualWhiteBalanceTint: -24,
        exposureBias: -0.5,
        blackLevelMode: .manual,
        manualBlackLevel: 0.0125,
        demosaicQuality: .high,
        linearOutput: false
    )
    let decoded = try JSONDecoder().decode(
        RawProcessingOptions.self,
        from: JSONEncoder().encode(manual)
    )
    #expect(decoded == manual)
}

@Test func curveControlPointsStayOrderedAndClamped() {
    var parameters = ProcessingParameters(
        curveBlackPoint: 0.9,
        curveMidInput: 0.2,
        curveMidOutput: 1.3,
        curveWhitePoint: 0.4
    )

    #expect(parameters.curveControlPoints.map(\.input) == [0.38, 0.39, 0.4])
    #expect(parameters.curveControlPoints.map(\.output) == [0.0, 1.0, 1.0])

    parameters.updateCurveControlPoint(.mid, input: 0.99, output: -0.2)

    #expect(parameters.curveControlPoints[1].input == 0.39)
    #expect(parameters.curveControlPoints[1].output == 0.0)
}

@Test func curvePresetsApplyAndMatchParameters() {
    var parameters = ProcessingParameters()

    #expect(parameters.matchingCurvePreset == .linear)

    parameters.applyCurvePreset(.deepSkyContrast)

    #expect(parameters.curveBlackPoint == 0.0)
    #expect(parameters.curveMidInput == 0.4)
    #expect(parameters.curveMidOutput == 0.58)
    #expect(parameters.curveWhitePoint == 1.0)
    #expect(parameters.matchingCurvePreset == .deepSkyContrast)

    parameters.updateCurveControlPoint(.mid, input: 0.4, output: 0.66)

    #expect(parameters.matchingCurvePreset == .custom)

    let customPreset = CustomCurvePreset(name: "Custom Lift", parameters: CurvePresetParameters(parameters: parameters))
    #expect(customPreset.matches(parameters))
}

@Test func curveResetAndReplacementStayLinearAndSynced() {
    var parameters = ProcessingParameters()

    parameters.replaceCurvePoints([
        CurveEditorPoint(input: 0.0, output: 0.0),
        CurveEditorPoint(input: 0.4, output: 0.8),
        CurveEditorPoint(input: 1.0, output: 1.0),
    ])

    #expect(parameters.curveMidInput == 0.4)
    #expect(parameters.curveMidOutput == 0.8)
    #expect(parameters.matchingCurvePreset == .custom)

    parameters.resetCurve()

    #expect(parameters.curveChannel == .rgb)
    #expect(parameters.curveEditorPoints.map(\.input) == [0.0, 0.5, 1.0])
    #expect(parameters.curveEditorPoints.map(\.output) == [0.0, 0.5, 1.0])
    #expect(parameters.matchingCurvePreset == .linear)
}

@Test func customCurvePresetLibraryRoundTrips() throws {
    let preset = CustomCurvePreset(
        name: "Nebula Lift",
        parameters: CurvePresetParameters(black: 0.01, midInput: 0.42, midOutput: 0.7, white: 0.99)
    )
    let library = CustomCurvePresetLibrary(presets: [preset])
    let encoder = JSONEncoder()
    encoder.dateEncodingStrategy = .iso8601
    let data = try encoder.encode(library)
    let decoder = JSONDecoder()
    decoder.dateDecodingStrategy = .iso8601
    let decoded = try decoder.decode(CustomCurvePresetLibrary.self, from: data)

    #expect(decoded.schemaVersion == 1)
    #expect(decoded.presets.count == 1)
    #expect(decoded.presets.first?.name == "Nebula Lift")
    #expect(decoded.presets.first?.parameters == preset.parameters)
}

@Test func curvePresetParametersNormalizeImportedValues() throws {
    let parameters = CurvePresetParameters(
        black: 0.9,
        midInput: -2,
        midOutput: 3,
        white: 0.4
    )

    #expect(parameters.black == 0.38)
    #expect(parameters.midInput == 0.39)
    #expect(parameters.midOutput == 1.0)
    #expect(parameters.white == 0.4)

    let decoded = try JSONDecoder().decode(
        CurvePresetParameters.self,
        from: Data(#"{"black":-1,"midInput":5,"midOutput":-4,"white":0.2}"#.utf8)
    )

    #expect(decoded.black == 0.0)
    #expect(decoded.midInput == 0.19)
    #expect(decoded.midOutput == 0.0)
    #expect(decoded.white == 0.2)
}

@Test func customCurvePresetLibraryValidatesSchemaVersion() throws {
    let legacy = try JSONDecoder().decode(
        CustomCurvePresetLibrary.self,
        from: Data(#"{"presets":[]}"#.utf8)
    )
    #expect(legacy.schemaVersion == CustomCurvePresetLibrary.currentSchemaVersion)

    #expect(throws: DecodingError.self) {
        try JSONDecoder().decode(
            CustomCurvePresetLibrary.self,
            from: Data(#"{"schemaVersion":99,"presets":[]}"#.utf8)
        )
    }
}

@Test func projectTemplateCreatesDefaultEditGraph() {
    let project = ProjectTemplate.deepSky.makeProject(name: "M31")

    #expect(project.name == "M31")
    #expect(project.template == .deepSky)
    #expect(project.editGraph.operations.map(\.kind) == [.calibrate, .stack, .stretch])
}

@Test func projectAddsMosaicAssetsWithRoleAndStableOrder() {
    var project = ProjectTemplate.mosaic.makeProject(name: "Milky Way Panorama")

    project.addAssets(
        from: [
            URL(fileURLWithPath: "/tmp/panel-b.nef"),
            URL(fileURLWithPath: "/tmp/panel-a.nef"),
        ],
        role: .mosaic
    )

    #expect(project.assets.map(\.role) == [.mosaic, .mosaic])
    #expect(project.mosaicPanelOrder == project.assets.map(\.id))
}

@Test func appSettingsRoundTripsRecentProjects() async throws {
    let settingsURL = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackSettings-\(UUID().uuidString)")
        .appendingPathComponent(AppSettingsRepository.settingsFileName)
    let repository = AppSettingsRepository(settingsURL: settingsURL)
    defer {
        try? FileManager.default.removeItem(at: settingsURL.deletingLastPathComponent())
    }

    let customPreset = CustomCurvePreset(
        name: "M42 Soft",
        parameters: CurvePresetParameters(black: 0.02, midInput: 0.4, midOutput: 0.68, white: 0.98)
    )
    var settings = AppSettings(
        autosaveEnabled: false,
        selectedTemplate: .mosaic,
        language: .simplifiedChinese,
        customCurvePresets: [customPreset],
        exportOptions: ExportOptions(bitDepth: .sixteen, colorSpace: .linearSRGB, jpegQuality: 0.8)
    )
    settings.markRecentProject(name: "M42", directory: URL(fileURLWithPath: "/tmp/M42"))
    settings.markRecentProject(name: "M31", directory: URL(fileURLWithPath: "/tmp/M31"))
    settings.markRecentProject(name: "M42 Updated", directory: URL(fileURLWithPath: "/tmp/M42"))
    settings.markRecentProject(name: "M31 Updated", directory: URL(fileURLWithPath: "/tmp/M31/"))

    try await repository.save(settings)
    let loaded = try await repository.load()

    #expect(loaded.autosaveEnabled == false)
    #expect(loaded.selectedTemplate == .mosaic)
    #expect(loaded.language == .simplifiedChinese)
    #expect(loaded.customCurvePresets.count == 1)
    #expect(loaded.customCurvePresets.first?.name == customPreset.name)
    #expect(loaded.customCurvePresets.first?.parameters == customPreset.parameters)
    #expect(loaded.exportOptions == ExportOptions(bitDepth: .sixteen, colorSpace: .linearSRGB, jpegQuality: 0.8))
    #expect(loaded.recentProjects.count == 2)
    #expect(loaded.recentProjects.first?.name == "M31 Updated")
    #expect(loaded.recentProjects.last?.name == "M42 Updated")
}

@Test func appSettingsDecodesLegacyDuplicateRecentProjects() throws {
    let json = """
    {
      "autosaveEnabled" : true,
      "recentProjects" : [
        {
          "id" : "00000000-0000-0000-0000-000000000001",
          "name" : "Mosaic Project Latest",
          "directory" : "file:///tmp/PhotonStack%20Project/",
          "openedAt" : "2026-05-25T15:25:00Z"
        },
        {
          "id" : "00000000-0000-0000-0000-000000000002",
          "name" : "Mosaic Project Duplicate",
          "directory" : "file:///tmp/PhotonStack%20Project",
          "openedAt" : "2026-05-25T15:20:00Z"
        }
      ],
      "selectedTemplate" : "singleFrame"
    }
    """
    let data = try #require(json.data(using: .utf8))
    let decoder = JSONDecoder()
    decoder.dateDecodingStrategy = .iso8601

    let settings = try decoder.decode(AppSettings.self, from: data)

    #expect(settings.recentProjects.count == 1)
    #expect(settings.recentProjects.first?.name == "Mosaic Project Latest")
}

@Test func appSettingsDecodesLegacyEntriesWithoutDisplayIdentityFields() throws {
    let json = """
    {
      "recentProjects" : [
        {
          "name" : "Legacy Project",
          "directory" : "file:///tmp/legacy-project"
        }
      ],
      "customCurvePresets" : [
        {
          "name" : "Legacy Curve",
          "parameters" : {
            "black" : 0.01,
            "midInput" : 0.4,
            "midOutput" : 0.65,
            "white" : 0.99
          }
        }
      ]
    }
    """
    let settings = try JSONDecoder().decode(AppSettings.self, from: Data(json.utf8))

    #expect(settings.recentProjects.first?.name == "Legacy Project")
    #expect(settings.recentProjects.first?.openedAt == Date(timeIntervalSince1970: 0))
    #expect(settings.customCurvePresets.first?.name == "Legacy Curve")
    #expect(settings.customCurvePresets.first?.createdAt == Date(timeIntervalSince1970: 0))
    #expect(settings.customCurvePresets.first?.parameters.midOutput == 0.65)
}

@Test func appSettingsHandlesNonPositiveRecentProjectLimits() {
    var settings = AppSettings(recentProjects: [
        RecentProject(name: "Existing", directory: URL(fileURLWithPath: "/tmp/existing")),
    ])

    settings.markRecentProject(
        name: "Ignored",
        directory: URL(fileURLWithPath: "/tmp/ignored"),
        limit: 0
    )
    #expect(settings.recentProjects.isEmpty)

    settings.markRecentProject(
        name: "Also Ignored",
        directory: URL(fileURLWithPath: "/tmp/also-ignored"),
        limit: -1
    )
    #expect(settings.recentProjects.isEmpty)
}

@Test func appSettingsRepairsDuplicateCurvePresetIdentifiersWithoutDroppingPresets() throws {
    let sharedID = UUID()
    let first = CustomCurvePreset(
        id: sharedID,
        name: "First",
        parameters: CurvePresetParameters(black: 0, midInput: 0.4, midOutput: 0.6, white: 1)
    )
    let second = CustomCurvePreset(
        id: sharedID,
        name: "Second",
        parameters: CurvePresetParameters(black: 0.02, midInput: 0.45, midOutput: 0.7, white: 0.98)
    )

    let settings = AppSettings(customCurvePresets: [first, second])

    #expect(settings.customCurvePresets.map(\.name) == ["First", "Second"])
    #expect(settings.customCurvePresets.first?.id == sharedID)
    #expect(Set(settings.customCurvePresets.map(\.id)).count == 2)

    let encoder = JSONEncoder()
    encoder.dateEncodingStrategy = .iso8601
    let decoder = JSONDecoder()
    decoder.dateDecodingStrategy = .iso8601
    let duplicateData = try encoder.encode([first, second])
    let duplicateJSON = try #require(String(data: duplicateData, encoding: .utf8))
    let settingsData = try #require(
        "{\"customCurvePresets\":\(duplicateJSON)}".data(using: .utf8)
    )
    let decoded = try decoder.decode(AppSettings.self, from: settingsData)

    #expect(decoded.customCurvePresets.map(\.name) == ["First", "Second"])
    #expect(Set(decoded.customCurvePresets.map(\.id)).count == 2)
}

@Test func exportOptionsKeepJPEGQualityFiniteAndInRange() throws {
    var options = ExportOptions(jpegQuality: 2)
    #expect(options.jpegQuality == 1)

    options.jpegQuality = -1
    #expect(options.jpegQuality == 0)

    options.jpegQuality = .nan
    #expect(options.jpegQuality == 0.92)

    let decoded = try JSONDecoder().decode(
        ExportOptions.self,
        from: Data(#"{"bitDepth":"sixteen","colorSpace":"linearSRGB","fitsValueMode":"scientific","jpegQuality":4}"#.utf8)
    )
    #expect(decoded.bitDepth == .sixteen)
    #expect(decoded.colorSpace == .linearSRGB)
    #expect(decoded.fitsValueMode == .scientific)
    #expect(decoded.jpegQuality == 1)

    let legacy = try JSONDecoder().decode(ExportOptions.self, from: Data(#"{}"#.utf8))
    #expect(legacy == ExportOptions())
    #expect(legacy.fitsValueMode == .display)
}

@Test func appSettingsDecodesLegacySettingsWithoutCustomCurvePresets() throws {
    let json = """
    {
      "autosaveEnabled" : false,
      "language" : "simplifiedChinese",
      "recentProjects" : [],
      "selectedTemplate" : "singleFrame"
    }
    """
    let data = try #require(json.data(using: .utf8))
    let settings = try JSONDecoder().decode(AppSettings.self, from: data)

    #expect(settings.autosaveEnabled == false)
    #expect(settings.selectedTemplate == .singleFrame)
    #expect(settings.language == .simplifiedChinese)
    #expect(settings.customCurvePresets.isEmpty)
    #expect(settings.exportOptions == ExportOptions())
}

@Test func projectRepositoryRoundTripsProjectJSON() async throws {
    let repository = ProjectRepository()
    let directory = FileManager.default.temporaryDirectory.appendingPathComponent("PhotonStackAppCoreTests-\(UUID().uuidString)")
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    var project = ProjectTemplate.mosaic.makeProject(name: "Round Trip")
    project.addAssets(from: [URL(fileURLWithPath: "/tmp/light.png")])
    project.setBatchOutputDirectory(URL(fileURLWithPath: "/tmp/photonstack-batch"))
    project.setBatchOutputFormat(.tiff)
    project.replaceBatchQueue([
        BatchQueueItem(
            title: "stretch light",
            kind: .stretch,
            inputURL: URL(fileURLWithPath: "/tmp/light.png")
        ),
    ])
    let selectedAssetID = try #require(project.assets.first?.id)
    let previewURL = URL(fileURLWithPath: "/tmp/photonstack-batch/final-preview.png")
    let previewOperationID = UUID()
    project.workspaceState = ProjectWorkspaceState(
        selectedAssetID: selectedAssetID,
        previewURL: previewURL,
        previewOperationID: previewOperationID,
        editGraphNeedsReplay: true,
        canvasMode: .sourcePreview
    )

    try await repository.save(project, to: directory)
    let loaded = try await repository.load(from: directory)

    #expect(loaded.name == "Round Trip")
    #expect(loaded.template == .mosaic)
    #expect(loaded.assets.first?.kind == .png)
    #expect(loaded.batchQueue.count == 1)
    #expect(loaded.batchQueue.first?.kind == .stretch)
    #expect(loaded.batchQueue.first?.inputURL.path == "/tmp/light.png")
    #expect(loaded.batchOutputDirectory?.path == "/tmp/photonstack-batch")
    #expect(loaded.batchOutputFormat == .tiff)
    #expect(loaded.workspaceState?.selectedAssetID == selectedAssetID)
    #expect(loaded.workspaceState?.activeLayerID == nil)
    #expect(loaded.workspaceState?.previewURL == previewURL)
    #expect(loaded.workspaceState?.previewOperationID == previewOperationID)
    #expect(loaded.workspaceState?.editGraphNeedsReplay == true)
    #expect(loaded.workspaceState?.canvasMode == .sourcePreview)
}

@Test func boundedDocumentReaderAcceptsExactLimitAndStopsUnboundedStreams() throws {
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackBoundedDocumentRead-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }
    let input = directory.appendingPathComponent("input.json")
    let exactData = Data(repeating: 0x41, count: 128)
    try exactData.write(to: input)

    let loaded = try BoundedDocumentIO.read(
        from: input,
        maximumBytes: 128,
        fileManager: .default
    )

    #expect(loaded == exactData)

    try Data(repeating: 0x41, count: 129).write(to: input)
    do {
        _ = try BoundedDocumentIO.read(from: input, maximumBytes: 128, fileManager: .default)
        Issue.record("A document one byte above the limit should fail")
    } catch let error as StructuredDocumentError {
        #expect(error == .fileTooLarge(maximumBytes: 128))
    }

    do {
        _ = try BoundedDocumentIO.read(
            from: URL(fileURLWithPath: "/dev/zero"),
            maximumBytes: 128,
            fileManager: .default
        )
        Issue.record("An unbounded stream should stop at the document limit")
    } catch let error as StructuredDocumentError {
        #expect(error == .fileTooLarge(maximumBytes: 128))
    }
}

@Test func projectRepositorySizeLimitPreservesExistingDocumentOnLoadAndSaveFailure() async throws {
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackProjectDocumentLimit-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }
    let projectURL = directory.appendingPathComponent(ProjectRepository.projectFileName)
    let repository = ProjectRepository(maximumDocumentBytes: 128)
    try Data(repeating: 0x20, count: 129).write(to: projectURL)

    do {
        _ = try await repository.load(from: directory)
        Issue.record("An oversized project should fail before decoding")
    } catch let error as StructuredDocumentError {
        #expect(error == .fileTooLarge(maximumBytes: 128))
    }

    let original = Data("existing project".utf8)
    try original.write(to: projectURL)
    do {
        try await repository.save(PhotonStackProject(name: "Too Large"), to: directory)
        Issue.record("An oversized encoded project should not be saved")
    } catch let error as StructuredDocumentError {
        #expect(error == .fileTooLarge(maximumBytes: 128))
    }
    #expect(try Data(contentsOf: projectURL) == original)
}

@Test func settingsRepositorySizeLimitPreservesExistingDocumentOnLoadAndSaveFailure() async throws {
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackSettingsDocumentLimit-\(UUID().uuidString)", isDirectory: true)
    let settingsURL = directory.appendingPathComponent(AppSettingsRepository.settingsFileName)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }
    let repository = AppSettingsRepository(settingsURL: settingsURL, maximumDocumentBytes: 64)
    try Data(repeating: 0x20, count: 65).write(to: settingsURL)

    do {
        _ = try await repository.load()
        Issue.record("Oversized settings should fail before decoding")
    } catch let error as StructuredDocumentError {
        #expect(error == .fileTooLarge(maximumBytes: 64))
    }

    let original = Data("existing settings".utf8)
    try original.write(to: settingsURL)
    do {
        try await repository.save(AppSettings())
        Issue.record("Oversized encoded settings should not be saved")
    } catch let error as StructuredDocumentError {
        #expect(error == .fileTooLarge(maximumBytes: 64))
    }
    #expect(try Data(contentsOf: settingsURL) == original)
}

@Test func projectRepositoryRejectsDuplicateObjectIdentifiersOnSaveAndLoad() async throws {
    let repository = ProjectRepository()
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackDuplicateIdentityTests-\(UUID().uuidString)")
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    let duplicateID = UUID()
    let project = PhotonStackProject(
        name: "Duplicate Asset Identity",
        assets: [
            PhotonStackAsset(
                id: duplicateID,
                originalURL: URL(fileURLWithPath: "/tmp/duplicate-a.tiff"),
                kind: .tiff
            ),
            PhotonStackAsset(
                id: duplicateID,
                originalURL: URL(fileURLWithPath: "/tmp/duplicate-b.tiff"),
                kind: .tiff
            ),
        ]
    )
    let expectedError = ProjectRepositoryError.duplicateIdentifier(collection: "assets")

    do {
        try await repository.save(project, to: directory)
        Issue.record("Saving a project with duplicate asset identifiers should fail")
    } catch let error as ProjectRepositoryError {
        #expect(error == expectedError)
    }

    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    let encoder = JSONEncoder()
    encoder.dateEncodingStrategy = .iso8601
    let data = try encoder.encode(project)
    try data.write(to: directory.appendingPathComponent(ProjectRepository.projectFileName))

    do {
        _ = try await repository.load(from: directory)
        Issue.record("Loading a project with duplicate asset identifiers should fail")
    } catch let error as ProjectRepositoryError {
        #expect(error == expectedError)
    }
}

@Test func projectRepositoryRejectsDuplicateCanonicalAssetLocations() async throws {
    let repository = ProjectRepository()
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackDuplicateLocationTests-\(UUID().uuidString)", isDirectory: true)
    let projectDirectory = directory.appendingPathComponent("project", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    let original = directory.appendingPathComponent("light.tiff")
    let alias = directory.appendingPathComponent("light-link.tiff")
    try Data([0x01]).write(to: original)
    try FileManager.default.createSymbolicLink(at: alias, withDestinationURL: original)
    let project = PhotonStackProject(
        name: "Duplicate Asset Location",
        assets: [
            PhotonStackAsset(originalURL: original, kind: .tiff),
            PhotonStackAsset(originalURL: alias, kind: .tiff),
        ]
    )
    let expectedError = ProjectRepositoryError.duplicateIdentifier(collection: "asset locations")

    do {
        try await repository.save(project, to: projectDirectory)
        Issue.record("Saving duplicate canonical asset locations should fail")
    } catch let error as ProjectRepositoryError {
        #expect(error == expectedError)
    }

    try FileManager.default.createDirectory(at: projectDirectory, withIntermediateDirectories: true)
    let encoder = JSONEncoder()
    encoder.dateEncodingStrategy = .iso8601
    try encoder.encode(project).write(
        to: projectDirectory.appendingPathComponent(ProjectRepository.projectFileName)
    )
    do {
        _ = try await repository.load(from: projectDirectory)
        Issue.record("Loading duplicate canonical asset locations should fail")
    } catch let error as ProjectRepositoryError {
        #expect(error == expectedError)
    }
}

@Test func workspaceStateDecodesWithoutLegacyCanvasMode() throws {
    let json = """
    {
      "selectedAssetID" : "\(UUID().uuidString)",
      "previewURL" : "file:///tmp/legacy-preview.png"
    }
    """
    let data = try #require(json.data(using: .utf8))
    let state = try JSONDecoder().decode(ProjectWorkspaceState.self, from: data)

    #expect(state.previewURL?.path == "/tmp/legacy-preview.png")
    #expect(state.canvasMode == nil)
}

@Test func projectDecodesLegacyProjectWithoutBatchQueue() throws {
    let now = ISO8601DateFormatter().string(from: Date())
    let json = """
    {
      "assets" : [],
      "createdAt" : "\(now)",
      "editGraph" : { "operations" : [] },
      "id" : "\(UUID().uuidString)",
      "mosaicPanelOrder" : [],
      "name" : "Legacy Project",
      "updatedAt" : "\(now)"
    }
    """
    let data = try #require(json.data(using: .utf8))
    let decoder = JSONDecoder()
    decoder.dateDecodingStrategy = .iso8601
    let project = try decoder.decode(PhotonStackProject.self, from: data)

    #expect(project.name == "Legacy Project")
    #expect(project.batchQueue.isEmpty)
    #expect(project.batchOutputDirectory == nil)
    #expect(project.batchOutputFormat == .png)
    #expect(project.workspaceState == nil)
    #expect(project.template == nil)
}

@Test func assetMetadataNormalizesInvalidValuesAcrossCoding() throws {
    var metadata = AssetMetadata(
        width: -1,
        height: 4032,
        channels: 0,
        bitsPerChannel: 16,
        cameraMake: "  Nikon  ",
        cameraModel: "   ",
        exposureTimeSeconds: -1,
        fNumber: .infinity,
        focalLengthMM: 16,
        iso: 0,
        orientation: 9
    )

    #expect(metadata.width == nil)
    #expect(metadata.height == 4032)
    #expect(metadata.channels == nil)
    #expect(metadata.bitsPerChannel == 16)
    #expect(metadata.cameraMake == "Nikon")
    #expect(metadata.cameraModel == nil)
    #expect(metadata.exposureTimeSeconds == nil)
    #expect(metadata.fNumber == nil)
    #expect(metadata.focalLengthMM == 16)
    #expect(metadata.iso == nil)
    #expect(metadata.orientation == nil)

    metadata.width = -2
    metadata.exposureTimeSeconds = .nan
    metadata.iso = -100
    metadata.orientation = 10
    let encoded = try JSONEncoder().encode(metadata)
    let roundTripped = try JSONDecoder().decode(AssetMetadata.self, from: encoded)
    #expect(roundTripped.width == nil)
    #expect(roundTripped.exposureTimeSeconds == nil)
    #expect(roundTripped.iso == nil)
    #expect(roundTripped.orientation == nil)

    let malformed = """
    {
      "width": -1,
      "height": 4032,
      "channels": "three",
      "bitsPerChannel": 16,
      "cameraModel": "  Z 6  ",
      "exposureTimeSeconds": false,
      "fNumber": -2.8,
      "focalLengthMM": 20,
      "iso": "3200",
      "orientation": 9
    }
    """
    let malformedData = try #require(malformed.data(using: .utf8))
    let decoded = try JSONDecoder().decode(AssetMetadata.self, from: malformedData)
    #expect(decoded.width == nil)
    #expect(decoded.height == 4032)
    #expect(decoded.channels == nil)
    #expect(decoded.bitsPerChannel == 16)
    #expect(decoded.cameraModel == "Z 6")
    #expect(decoded.exposureTimeSeconds == nil)
    #expect(decoded.fNumber == nil)
    #expect(decoded.focalLengthMM == 20)
    #expect(decoded.iso == nil)
    #expect(decoded.orientation == nil)
}
