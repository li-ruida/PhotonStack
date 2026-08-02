import Foundation
import CoreImage
import ImageIO
import PhotonStackAppCore
import PhotonStackProcessing
import Testing
@testable import PhotonStackUI

#if os(macOS)
@Test func nativePreviewDecoderDownconvertsSixteenBitTIFFForAppKit() throws {
    let output = FileManager.default.temporaryDirectory
        .appendingPathComponent("photonstack-native-preview-\(UUID().uuidString).tiff")
    defer { try? FileManager.default.removeItem(at: output) }

    let extent = CGRect(x: 0, y: 0, width: 64, height: 48)
    let image = CIImage(color: CIColor(red: 0.35, green: 0.55, blue: 0.75, alpha: 1))
        .cropped(to: extent)
    let context = CIContext()
    let colorSpace = try #require(CGColorSpace(name: CGColorSpace.sRGB))
    try context.writeTIFFRepresentation(
        of: image,
        to: output,
        format: .RGBA16,
        colorSpace: colorSpace
    )

    let source = try #require(CGImageSourceCreateWithURL(output as CFURL, nil))
    let sourceImage = try #require(CGImageSourceCreateImageAtIndex(source, 0, nil))
    #expect(sourceImage.bitsPerComponent == 16)

    let preview = try #require(NativePreviewImageDecoder.decodeCGImage(contentsOf: output))
    #expect(preview.width == 64)
    #expect(preview.height == 48)
    #expect(preview.bitsPerComponent == 8)
}
#endif

@Test @MainActor func applicationProcessCommandsTrackLocalizationAndWorkspacePrerequisites() throws {
    let commandState = PhotonStackApplicationCommandState(language: .english)
    #expect(commandState.processMenuTitle == "Process")
    #expect(commandState.batchRegistrationTitle == "Batch Register Frames")
    #expect(commandState.layerStackPreviewTitle == "Preview Stack")
    #expect(commandState.productManifestExportTitle == "Export Product Manifest")
    #expect(commandState.snapshot.isWorkspaceActive == false)
    #expect(commandState.snapshot.canRunBatchRegistration == false)
    #expect(commandState.snapshot.canPreviewLayerStack == false)

    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackApplicationCommands-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    let referenceURL = directory.appendingPathComponent("reference.png")
    let movingURL = directory.appendingPathComponent("moving.png")
    let layerURL = directory.appendingPathComponent("layer.png")
    try writeSolidPNG(to: referenceURL, red: 0.8, green: 0.3, blue: 0.2)
    try writeSolidPNG(to: movingURL, red: 0.2, green: 0.7, blue: 0.4)
    try writeSolidPNG(to: layerURL, red: 0.2, green: 0.4, blue: 0.9)

    var project = PhotonStackProject(name: "Command State")
    project.addAssets(from: [referenceURL, movingURL])
    project.appendLayer(ProcessingLayer(name: "Visible Layer", kind: .baseImage, inputURL: layerURL))
    let model = PhotonStackWorkspaceModel(
        project: project,
        language: .english,
        processingService: RecordingProcessingService(requiresExistingInputFiles: true),
        previewDirectory: directory.appendingPathComponent("previews", isDirectory: true)
    )

    var snapshot = model.applicationCommandSnapshot
    #expect(snapshot.isWorkspaceActive)
    #expect(snapshot.canRunBatchRegistration)
    #expect(snapshot.canPreviewLayerStack)

    commandState.update(snapshot)
    #expect(commandState.snapshot == snapshot)

    model.clearBatchRegistrationFrames()
    snapshot = model.applicationCommandSnapshot
    #expect(snapshot.canRunBatchRegistration == false)
    #expect(snapshot.canPreviewLayerStack)

    try FileManager.default.removeItem(at: layerURL)
    snapshot = model.applicationCommandSnapshot
    #expect(snapshot.canPreviewLayerStack == false)

    model.language = .simplifiedChinese
    commandState.update(model.applicationCommandSnapshot)
    #expect(commandState.processMenuTitle == "处理")
    #expect(commandState.batchRegistrationTitle == "批量星点对齐")
    #expect(commandState.layerStackPreviewTitle == "预览图层栈")
    #expect(commandState.productManifestExportTitle == "导出产品清单")

    commandState.update(.inactive(language: .simplifiedChinese))
    #expect(commandState.snapshot.isWorkspaceActive == false)
    #expect(commandState.snapshot.canRunBatchRegistration == false)
    #expect(commandState.snapshot.canPreviewLayerStack == false)
}

@Test @MainActor func stackWorkflowSkipsCalibrationWhenNoCalibrationFrames() async {
    let service = RecordingProcessingService()
    let lightA = URL(fileURLWithPath: "/tmp/photonstack-light-a.nef")
    let lightB = URL(fileURLWithPath: "/tmp/photonstack-light-b.nef")
    var project = PhotonStackProject(name: "Stack Test")
    project.addAssets(from: [lightA, lightB])

    let previewDirectory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackWorkflowTest-\(UUID().uuidString)", isDirectory: true)
    defer {
        try? FileManager.default.removeItem(at: previewDirectory)
    }

    let model = PhotonStackWorkspaceModel(
        project: project,
        parameters: ProcessingParameters(workflowStackMethod: .average, workflowAlignment: .none),
        processingService: service,
        previewDirectory: previewDirectory
    )

    await model.runStackWorkflow()

    let snapshot = await service.snapshot()
    #expect(snapshot.calibrateCalls == 0)
    #expect(snapshot.convertCalls == 2)
    #expect(snapshot.stackInputs.map(\.lastPathComponent) == [
        "photonstack-light-a.decoded.fits",
        "photonstack-light-b.decoded.fits",
    ])
    #expect(snapshot.convertOptions.allSatisfy { $0.fitsValueMode == .scientific })
    #expect(snapshot.autoStretchInputs.first?.lastPathComponent == "stack-background.fits")
    #expect(snapshot.autoStretchCalls == 1)
    #expect(model.latestJob?.status == .succeeded)
}

@Test @MainActor func calibratedStackWorkflowUsesAndRecordsRawDecodeOptions() async throws {
    let service = RecordingProcessingService()
    let light = URL(fileURLWithPath: "/tmp/photonstack-calibrated-raw-light.nef")
    let dark = URL(fileURLWithPath: "/tmp/photonstack-calibrated-raw-dark.nef")
    var project = PhotonStackProject(name: "Calibrated RAW Options")
    project.addAssets(from: [light, dark])
    project.updateRole(for: try #require(project.assets.last?.id), role: .dark)
    let requested = RawProcessingOptions(
        whiteBalanceMode: .manual,
        manualWhiteBalanceTemperature: 7200,
        manualWhiteBalanceTint: -15,
        exposureBias: -0.75,
        blackLevelMode: .manual,
        manualBlackLevel: 0.0125,
        demosaicQuality: .high,
        linearOutput: false
    )
    let parameters = ProcessingParameters(
        workflowStackMethod: .average,
        workflowAlignment: .none,
        rawProcessingOptions: requested
    )
    let previewDirectory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackCalibratedRawOptions-\(UUID().uuidString)", isDirectory: true)
    defer {
        try? FileManager.default.removeItem(at: previewDirectory)
    }
    let model = PhotonStackWorkspaceModel(
        project: project,
        parameters: parameters,
        processingService: service,
        previewDirectory: previewDirectory
    )

    await model.runStackWorkflow()

    let effective = RawProcessingOptions(
        whiteBalanceMode: .manual,
        manualWhiteBalanceTemperature: 7200,
        manualWhiteBalanceTint: -15,
        exposureBias: -0.75,
        blackLevelMode: .manual,
        manualBlackLevel: 0.0125,
        demosaicQuality: .high,
        linearOutput: true
    )
    let snapshot = await service.snapshot()
    #expect(snapshot.masterRawOptions == [effective])
    #expect(snapshot.calibrateRawOptions == [effective])
    #expect(snapshot.calibrateDarkBiasStates == [.included])
    #expect(snapshot.calibrateFlatBiasStates == [.included])
    let calibration = try #require(model.project.editGraph.operations.first { $0.kind == .calibrate })
    #expect(calibration.parameters["rawWhiteBalance"] == RawWhiteBalanceMode.manual.rawValue)
    #expect(calibration.parameters["rawTemperature"] == "7200.0")
    #expect(calibration.parameters["rawTint"] == "-15.0")
    #expect(calibration.parameters["rawExposureBias"] == "-0.75")
    #expect(calibration.parameters["rawBlackLevel"] == RawBlackLevelMode.manual.rawValue)
    #expect(calibration.parameters["rawBlackValue"] == "0.0125")
    #expect(calibration.parameters["rawDemosaic"] == RawDemosaicQuality.high.rawValue)
    #expect(calibration.parameters["rawLinear"] == "true")
    #expect(calibration.parameters["darkBiasState"] == CalibrationBiasState.included.rawValue)
    #expect(calibration.parameters["flatBiasState"] == CalibrationBiasState.included.rawValue)
    #expect(model.latestJob?.status == .succeeded)
}

@Test @MainActor func stackWorkflowRequiresBiasForUncorrectedFlat() async throws {
    var project = PhotonStackProject(name: "Flat Bias Validation")
    project.addAssets(from: [
        URL(fileURLWithPath: "/tmp/photonstack-flat-validation-light.nef"),
        URL(fileURLWithPath: "/tmp/photonstack-flat-validation-flat.nef"),
    ])
    project.updateRole(for: try #require(project.assets.last?.id), role: .flat)
    let service = RecordingProcessingService()
    let model = PhotonStackWorkspaceModel(project: project, processingService: service)

    #expect(model.parameters.workflowFlatBiasState == .included)
    #expect(model.calibrationConfigurationWarning != nil)
    #expect(model.canRunStackWorkflow == false)
    await model.runStackWorkflow()
    #expect(await service.snapshot().masterInputGroups.isEmpty)
    #expect(model.errorMessage == model.localized(.flatBiasMissingWarning))

    var updated = model.parameters
    updated.workflowFlatBiasState = .removed
    model.setProcessingParameters(updated)
    #expect(model.calibrationConfigurationWarning == nil)
    #expect(model.canRunStackWorkflow)
}

@Test @MainActor func weightedStackWorkflowUsesStackerQualityInsteadOfRedundantPreflight() async {
    let service = RecordingProcessingService(failQuality: true)
    var project = PhotonStackProject(name: "Weighted Stack Quality")
    project.addAssets(from: [
        URL(fileURLWithPath: "/tmp/photonstack-weighted-a.tiff"),
        URL(fileURLWithPath: "/tmp/photonstack-weighted-b.tiff"),
    ])
    let model = PhotonStackWorkspaceModel(
        project: project,
        parameters: ProcessingParameters(workflowStackMethod: .weighted, workflowAlignment: .none),
        processingService: service
    )

    await model.runStackWorkflow()

    #expect(await service.snapshot().stackInputs.count == 2)
    #expect(model.latestJob?.status == .succeeded)
    #expect(model.project.artifacts.contains { $0.kind == .stackMaster })
}

@Test @MainActor func stackWorkflowMapsChildProgressIntoCurrentStep() async throws {
    let service = RecordingProcessingService(
        stackDelayNanoseconds: 300_000_000,
        stackProgressToEmit: 0.94
    )
    var project = PhotonStackProject(name: "Stack Progress Mapping")
    project.addAssets(from: [
        URL(fileURLWithPath: "/tmp/photonstack-progress-a.nef"),
        URL(fileURLWithPath: "/tmp/photonstack-progress-b.nef"),
    ])
    let previewDirectory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackWorkflowProgress-\(UUID().uuidString)", isDirectory: true)
    defer {
        try? FileManager.default.removeItem(at: previewDirectory)
    }
    let model = PhotonStackWorkspaceModel(
        project: project,
        parameters: ProcessingParameters(workflowStackMethod: .average, workflowAlignment: .none),
        processingService: service,
        previewDirectory: previewDirectory
    )

    model.startRunStackWorkflow()
    let deadline = ContinuousClock.now + .seconds(5)
    while ContinuousClock.now < deadline,
          model.progressMessage != "stack combine" {
        try await Task.sleep(for: .milliseconds(10))
    }

    let expected = (2.0 + 0.94) / 19.0
    #expect(abs((model.progressFraction ?? 0) - expected) < 0.000_001)
    #expect(model.progressFraction ?? 1 < 0.2)
    model.cancelCurrentTask()
    try await waitForProcessingCompletion(in: model)
}

@Test @MainActor func stackWorkflowUsesRegisteredSequenceArtifactWhenAvailable() async throws {
    let service = RecordingProcessingService()
    var project = PhotonStackProject(name: "Registered Stack Test")
    project.addAssets(from: [
        URL(fileURLWithPath: "/tmp/photonstack-light-a.nef"),
        URL(fileURLWithPath: "/tmp/photonstack-light-b.nef"),
    ])
    let previewDirectory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackRegisteredStackTest-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: previewDirectory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: previewDirectory)
    }
    let alignedA = previewDirectory.appendingPathComponent("aligned-a.png")
    let alignedB = previewDirectory.appendingPathComponent("aligned-b.png")
    try writeSolidPNG(to: alignedA, red: 0.1, green: 0.2, blue: 0.3)
    try writeSolidPNG(to: alignedB, red: 0.2, green: 0.3, blue: 0.4)
    let registered = ProcessingArtifact(
        kind: .registeredSequence,
        name: "Registered Lights",
        operationKind: .register,
        outputDirectory: previewDirectory,
        outputURLs: [alignedA, alignedB],
        parameters: [
            "sourceFrameIDs": project.assets.map(\.id.uuidString).joined(separator: ","),
        ],
        metrics: ["frames": "2"]
    )
    project.appendArtifact(registered)

    let model = PhotonStackWorkspaceModel(
        project: project,
        parameters: ProcessingParameters(workflowStackMethod: .average, workflowAlignment: .distortion),
        processingService: service,
        previewDirectory: previewDirectory
    )

    await model.runStackWorkflow()

    let snapshot = await service.snapshot()
    #expect(snapshot.convertCalls == 0)
    #expect(snapshot.stackInputs == [alignedA, alignedB])
    #expect(model.project.artifacts.contains { $0.kind == .stackMaster })
    #expect(model.project.artifacts.contains { $0.kind == .editedImage })
    #expect(model.project.layers.contains { $0.kind == .baseImage })
}

@Test @MainActor func stackWorkflowRejectsMalformedRegistrationReportBeforeStacking() async throws {
    let service = RecordingProcessingService(
        registerOutputOverride: #"{"type":"complete","command":"register","mode":"distortion","dx":1,"dy":2,"scale":1,"rotationRadians":0,"matches":8,"detectedReferenceStars":60,"detectedMovingStars":58,"inlierRatio":0.7,"usedFallback":false,"output":"/tmp/wrong-aligned-output.tiff"}"#
    )
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackInvalidWorkflowRegistration-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    let firstInput = directory.appendingPathComponent("first.png")
    let secondInput = directory.appendingPathComponent("second.png")
    try writeSolidPNG(to: firstInput, red: 0.2, green: 0.3, blue: 0.4, width: 16, height: 8)
    try writeSolidPNG(to: secondInput, red: 0.3, green: 0.4, blue: 0.5, width: 16, height: 8)
    var project = PhotonStackProject(name: "Invalid Workflow Registration")
    project.addAssets(from: [firstInput, secondInput])
    let model = PhotonStackWorkspaceModel(
        project: project,
        parameters: ProcessingParameters(workflowStackMethod: .average, workflowAlignment: .distortion),
        processingService: service,
        previewDirectory: directory.appendingPathComponent("previews", isDirectory: true)
    )

    await model.runStackWorkflow()

    #expect(await service.snapshot().stackInputs.isEmpty)
    #expect(model.latestJob?.status == .failed)
    #expect(model.errorMessage?.contains("register") == true)
    #expect(model.project.artifacts.isEmpty)
    #expect(model.project.editGraph.operations.isEmpty)
}

@Test @MainActor func stackWorkflowKeepsReferenceFrameInItsOriginalFormat() async throws {
    let service = RecordingProcessingService()
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackReferenceFormat-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    let inputs = ["first.png", "reference.png", "third.png"].map {
        directory.appendingPathComponent($0)
    }
    for (index, input) in inputs.enumerated() {
        try writeSolidPNG(
            to: input,
            red: Double(index + 1) * 0.1,
            green: 0.2,
            blue: 0.3,
            width: 16,
            height: 8
        )
    }
    var project = PhotonStackProject(name: "Reference Format")
    project.addAssets(from: inputs)
    let model = PhotonStackWorkspaceModel(
        project: project,
        parameters: ProcessingParameters(workflowStackMethod: .average, workflowAlignment: .distortion),
        processingService: service,
        previewDirectory: directory.appendingPathComponent("previews", isDirectory: true)
    )

    await model.runStackWorkflow()

    let stackInputs = await service.snapshot().stackInputs
    #expect(stackInputs.count == 3)
    #expect(stackInputs[1] == inputs[1])
    #expect(stackInputs[1].pathExtension == "png")
    #expect(stackInputs[0].lastPathComponent.hasSuffix(".aligned.fits"))
    #expect(stackInputs[2].lastPathComponent.hasSuffix(".aligned.fits"))
    #expect(model.latestJob?.status == .succeeded)
}

@Test @MainActor func stackWorkflowIgnoresMissingRegisteredSequenceOutputs() async throws {
    let service = RecordingProcessingService()
    var project = PhotonStackProject(name: "Missing Registered Stack Test")
    project.addAssets(from: [
        URL(fileURLWithPath: "/tmp/photonstack-missing-registered-a.nef"),
        URL(fileURLWithPath: "/tmp/photonstack-missing-registered-b.nef"),
    ])
    project.appendArtifact(
        ProcessingArtifact(
            kind: .registeredSequence,
            name: "Missing Registered Lights",
            operationKind: .register,
            outputDirectory: URL(fileURLWithPath: "/tmp/missing-registered", isDirectory: true),
            outputURLs: [
                URL(fileURLWithPath: "/tmp/missing-registered/aligned-a.tiff"),
                URL(fileURLWithPath: "/tmp/missing-registered/aligned-b.tiff"),
            ],
            metrics: ["frames": "2"]
        )
    )
    let model = PhotonStackWorkspaceModel(
        project: project,
        parameters: ProcessingParameters(workflowStackMethod: .average, workflowAlignment: .none),
        processingService: service
    )

    await model.runStackWorkflow()

    let snapshot = await service.snapshot()
    #expect(snapshot.convertCalls == 2)
    #expect(snapshot.stackInputs == snapshot.convertOutputs)
    #expect(model.latestJob?.status == .succeeded)
}

@Test @MainActor func stackWorkflowIgnoresRegisteredSequenceFromDifferentLights() async throws {
    let service = RecordingProcessingService()
    var project = PhotonStackProject(name: "Mismatched Registered Stack Test")
    project.addAssets(from: [
        URL(fileURLWithPath: "/tmp/photonstack-current-registered-a.nef"),
        URL(fileURLWithPath: "/tmp/photonstack-current-registered-b.nef"),
    ])
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackMismatchedRegistered-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }
    let alignedA = directory.appendingPathComponent("other-a.png")
    let alignedB = directory.appendingPathComponent("other-b.png")
    try writeSolidPNG(to: alignedA, red: 0.4, green: 0.1, blue: 0.1)
    try writeSolidPNG(to: alignedB, red: 0.5, green: 0.1, blue: 0.1)
    project.appendArtifact(
        ProcessingArtifact(
            kind: .registeredSequence,
            name: "Other Registered Lights",
            operationKind: .register,
            outputDirectory: directory,
            outputURLs: [alignedA, alignedB],
            parameters: [
                "sourceFrameIDs": [UUID(), UUID()].map(\.uuidString).joined(separator: ","),
            ],
            metrics: ["frames": "2"]
        )
    )
    let model = PhotonStackWorkspaceModel(
        project: project,
        parameters: ProcessingParameters(workflowStackMethod: .average, workflowAlignment: .none),
        processingService: service,
        previewDirectory: directory.appendingPathComponent("previews", isDirectory: true)
    )

    await model.runStackWorkflow()

    let snapshot = await service.snapshot()
    #expect(snapshot.convertCalls == 2)
    #expect(snapshot.stackInputs == snapshot.convertOutputs)
    #expect(snapshot.stackInputs.contains(alignedA) == false)
    #expect(snapshot.stackInputs.contains(alignedB) == false)
    #expect(model.latestJob?.status == .succeeded)
}

@Test @MainActor func importingRawSequenceSelectsWideFieldStackingDefaults() async {
    let service = RecordingProcessingService()
    let model = PhotonStackWorkspaceModel(
        selectedTemplate: .mosaic,
        processingService: service
    )

    model.importAssets(from: [
        URL(fileURLWithPath: "/tmp/astro-a.nef"),
        URL(fileURLWithPath: "/tmp/astro-b.nef"),
        URL(fileURLWithPath: "/tmp/astro-c.nef"),
    ])

    #expect(model.selectedTemplate == .deepSky)
    #expect(model.parameters.workflowStackMethod == .winsorized)
    #expect(model.parameters.workflowAlignment == .distortion)
    #expect(model.parameters.workflowRestoreMeteors == false)
    #expect(model.parameters.rawProcessingOptions.blackLevelMode == .auto)
    #expect(model.project.artifacts.first?.kind == .rawSequence)
    #expect(model.project.artifacts.first?.frameCount == 3)
}

@Test @MainActor func importedSourceSequenceRecordsAndSynchronizesAssetRoles() throws {
    let model = PhotonStackWorkspaceModel(
        project: ProjectTemplate.mosaic.makeProject(name: "Source Roles"),
        language: .simplifiedChinese,
        processingService: RecordingProcessingService()
    )

    model.importAssets(from: [
        URL(fileURLWithPath: "/tmp/photonstack-source-role-a.tiff"),
        URL(fileURLWithPath: "/tmp/photonstack-source-role-b.tiff"),
    ])

    let artifact = try #require(model.project.artifacts.first { $0.kind == .rawSequence })
    #expect(artifact.name == "源素材序列")
    #expect(artifact.parameters["role"] == CalibrationFrameRole.mosaic.rawValue)
    #expect(artifact.parameters["source"] == "import")
    #expect(artifact.frames.allSatisfy { $0.metrics["role"] == CalibrationFrameRole.mosaic.rawValue })

    let firstAsset = try #require(model.project.assets.first)
    model.setRole(.light, for: firstAsset)

    let updatedArtifact = try #require(model.project.artifacts.first { $0.id == artifact.id })
    #expect(updatedArtifact.parameters["role"] == "mixed")
    #expect(updatedArtifact.frames.first?.metrics["role"] == CalibrationFrameRole.light.rawValue)
    #expect(updatedArtifact.frames.last?.metrics["role"] == CalibrationFrameRole.mosaic.rawValue)
}

@Test @MainActor func batchRegistrationLinksSourceArtifactsAcrossSeparateImports() async throws {
    let model = PhotonStackWorkspaceModel(processingService: RecordingProcessingService())
    model.importAssets(from: [URL(fileURLWithPath: "/tmp/photonstack-source-batch-a.tiff")])
    model.importAssets(from: [URL(fileURLWithPath: "/tmp/photonstack-source-batch-b.tiff")])
    let sourceIDs = model.project.artifacts.filter { $0.kind == .rawSequence }.map(\.id)
    let reference = try #require(model.project.assets.first)

    await model.registerBatch(reference: reference, alignment: .distortion)

    let registered = try #require(model.project.artifacts.first { $0.kind == .registeredSequence })
    #expect(sourceIDs.count == 2)
    #expect(registered.sourceArtifactIDs == sourceIDs)
}

@Test @MainActor func unregisteredStackLinksSourceArtifactsAcrossSeparateImports() async throws {
    let model = PhotonStackWorkspaceModel(
        parameters: ProcessingParameters(workflowStackMethod: .average, workflowAlignment: .none),
        processingService: RecordingProcessingService()
    )
    model.importAssets(from: [URL(fileURLWithPath: "/tmp/photonstack-source-stack-a.tiff")])
    model.importAssets(from: [URL(fileURLWithPath: "/tmp/photonstack-source-stack-b.tiff")])
    let sourceIDs = model.project.artifacts.filter { $0.kind == .rawSequence }.map(\.id)

    await model.runStackWorkflow()

    let stack = try #require(model.project.artifacts.first { $0.kind == .stackMaster })
    #expect(sourceIDs.count == 2)
    #expect(stack.sourceArtifactIDs == sourceIDs)
}

@Test @MainActor func importingFolderExpandsSupportedAssets() async throws {
    let service = RecordingProcessingService()
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackImportFolderTest-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }
    let nested = directory.appendingPathComponent("nested", isDirectory: true)
    try FileManager.default.createDirectory(at: nested, withIntermediateDirectories: true)
    let rawA = directory.appendingPathComponent("a.nef")
    let rawB = nested.appendingPathComponent("b.NEF")
    let rawC = nested.appendingPathComponent("c.dng")
    let ignored = directory.appendingPathComponent("notes.txt")
    try Data().write(to: rawA)
    try Data().write(to: rawB)
    try Data().write(to: rawC)
    try Data().write(to: ignored)

    let model = PhotonStackWorkspaceModel(processingService: service)
    model.importAssets(from: [directory])

    #expect(model.project.assets.map { $0.originalURL.standardizedFileURL.path } == [
        rawA.standardizedFileURL.path,
        rawB.standardizedFileURL.path,
        rawC.standardizedFileURL.path,
    ])
    #expect(model.parameters.workflowAlignment == .distortion)
}

#if os(macOS)
@Test @MainActor func nativeImportPanelsSeparateFileAndFolderIntent() {
    let filesPanel = NativeFilePanels.makeImportFilesPanel(message: "files")
    #expect(filesPanel.canChooseFiles)
    #expect(filesPanel.canChooseDirectories == false)
    #expect(filesPanel.allowsMultipleSelection)

    let folderPanel = NativeFilePanels.makeImportFolderPanel(message: "folder")
    #expect(folderPanel.canChooseFiles == false)
    #expect(folderPanel.canChooseDirectories)
    #expect(folderPanel.allowsMultipleSelection == false)

    let relinkPanel = NativeFilePanels.makeRelinkAssetPanel(message: "relink")
    #expect(relinkPanel.canChooseFiles)
    #expect(relinkPanel.canChooseDirectories == false)
    #expect(relinkPanel.allowsMultipleSelection == false)

    let openProjectPanel = NativeFilePanels.makeOpenProjectPanel(message: "open project")
    #expect(openProjectPanel.message == "open project")
    #expect(openProjectPanel.canChooseFiles == false)
    #expect(openProjectPanel.canChooseDirectories)
    #expect(openProjectPanel.allowsMultipleSelection == false)

    let saveProjectPanel = NativeFilePanels.makeSaveProjectPanel(message: "save project")
    #expect(saveProjectPanel.message == "save project")
    #expect(saveProjectPanel.canCreateDirectories)

    let exportImagePanel = NativeFilePanels.makeExportImagePanel(message: "export image")
    #expect(exportImagePanel.message == "export image")
    #expect(exportImagePanel.canCreateDirectories)
    #expect(exportImagePanel.allowedContentTypes.contains(.tiff))
    #expect(exportImagePanel.allowedContentTypes.contains(.png))
    #expect(exportImagePanel.allowedContentTypes.contains(.jpeg))

    let openPresetPanel = NativeFilePanels.makeOpenCurvePresetLibraryPanel(message: "open presets")
    #expect(openPresetPanel.message == "open presets")
    #expect(openPresetPanel.canChooseFiles)
    #expect(openPresetPanel.canChooseDirectories == false)
    #expect(openPresetPanel.allowedContentTypes == [.json])

    let exportPresetPanel = NativeFilePanels.makeExportCurvePresetLibraryPanel(message: "export presets")
    #expect(exportPresetPanel.message == "export presets")
    #expect(exportPresetPanel.allowedContentTypes == [.json])

    let batchOutputPanel = NativeFilePanels.makeBatchOutputDirectoryPanel(message: "batch output")
    #expect(batchOutputPanel.message == "batch output")
    #expect(batchOutputPanel.canChooseFiles == false)
    #expect(batchOutputPanel.canChooseDirectories)
    #expect(batchOutputPanel.canCreateDirectories)
}
#endif

@Test @MainActor func stackWorkflowCanRestoreMeteorsBeforeStretch() async throws {
    let service = RecordingProcessingService()
    let lightA = URL(fileURLWithPath: "/tmp/photonstack-meteor-a.nef")
    let lightB = URL(fileURLWithPath: "/tmp/photonstack-meteor-b.nef")
    var project = PhotonStackProject(name: "Meteor Restore Test")
    project.addAssets(from: [lightA, lightB])

    let previewDirectory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackMeteorRestoreTest-\(UUID().uuidString)", isDirectory: true)
    defer {
        try? FileManager.default.removeItem(at: previewDirectory)
    }

    let model = PhotonStackWorkspaceModel(
        project: project,
        parameters: ProcessingParameters(workflowStackMethod: .average, workflowAlignment: .none, workflowRestoreMeteors: true),
        processingService: service,
        previewDirectory: previewDirectory
    )

    await model.runStackWorkflow()

    let snapshot = await service.snapshot()
    #expect(snapshot.restoreMeteorCalls == 2)
    #expect(model.project.editGraph.operations.first?.kind == .stack)
    #expect(model.project.editGraph.operations.dropFirst().first?.kind == .meteorRestore)
    #expect(model.project.editGraph.operations.last?.kind == .crop)
    let meteorOperation = try #require(model.project.editGraph.operations.first { $0.kind == .meteorRestore })
    #expect(meteorOperation.parameters["mode"] == "restoreSequence")
    #expect(meteorOperation.parameters["sourceLightIDs"] == model.project.assets.map(\.id.uuidString).joined(separator: ","))
    #expect(meteorOperation.parameters["sourcePath.0"]?.isEmpty == false)
    #expect(meteorOperation.parameters["sourcePath.1"]?.isEmpty == false)
    #expect(snapshot.restoreMeteorSources == snapshot.stackInputs)
    #expect(model.latestJob?.status == .succeeded)

    let replayService = RecordingProcessingService()
    let replayModel = PhotonStackWorkspaceModel(
        project: model.project,
        processingService: replayService,
        previewDirectory: previewDirectory.appendingPathComponent("replay", isDirectory: true)
    )
    replayModel.startReplayEditGraph()
    try await waitForStartedProcessingCompletion(in: replayModel)

    let replaySnapshot = await replayService.snapshot()
    #expect(replaySnapshot.convertInputs == [lightA, lightB])
    #expect(replaySnapshot.stackInputs == replaySnapshot.convertOutputs)
    #expect(replaySnapshot.restoreMeteorSources == replaySnapshot.stackInputs)
    #expect(replaySnapshot.restoreMeteorCalls == 2)
    #expect(replayModel.latestJob?.status == .succeeded)
}

@Test @MainActor func replayStackNodeKeepsLinearStackSeparateFromStretch() async throws {
    let service = RecordingProcessingService()
    let light = URL(fileURLWithPath: "/tmp/photonstack-replay-stack-light.tiff")
    var project = PhotonStackProject(name: "Replay Stack Test")
    project.addAssets(from: [light])
    project.editGraph = EditGraph(operations: [
        EditOperation(
            kind: .stack,
            parameters: [
                "stackMethod": StackMethod.median.rawValue,
                "alignment": AlignmentMethod.none.rawValue,
            ]
        ),
    ])
    let model = PhotonStackWorkspaceModel(project: project, processingService: service)

    model.startReplayEditGraph()
    try await waitForStartedProcessingCompletion(in: model)

    let snapshot = await service.snapshot()
    #expect(snapshot.stackInputs == [light])
    #expect(snapshot.autoStretchCalls == 0)
    #expect(model.previewURL?.pathExtension == "tiff")
    #expect(model.latestJob?.status == .succeeded)
}

@Test @MainActor func stackReplayDecodesRawLightsUsingRecordedOptions() async throws {
    let service = RecordingProcessingService()
    let rawLight = URL(fileURLWithPath: "/tmp/photonstack-replay-recorded-raw.nef")
    var project = PhotonStackProject(name: "Replay Recorded RAW Stack Test")
    project.addAssets(from: [rawLight])
    let lightID = try #require(project.assets.first?.id.uuidString)
    project.editGraph = EditGraph(operations: [
        EditOperation(
            kind: .stack,
            parameters: [
                "stackMethod": StackMethod.average.rawValue,
                "alignment": AlignmentMethod.none.rawValue,
                "calibrationApplied": "false",
                "calibrationLightIDs": lightID,
                "rawWhiteBalance": RawWhiteBalanceMode.auto.rawValue,
                "rawExposureBias": "1.25",
                "rawBlackLevel": RawBlackLevelMode.auto.rawValue,
                "rawDemosaic": RawDemosaicQuality.high.rawValue,
                "rawLinear": "false",
            ]
        ),
    ])
    let model = PhotonStackWorkspaceModel(project: project, processingService: service)

    model.startReplayEditGraph()
    try await waitForStartedProcessingCompletion(in: model)

    let snapshot = await service.snapshot()
    #expect(snapshot.convertInputs == [rawLight])
    #expect(snapshot.convertOutputs.count == 1)
    #expect(snapshot.stackInputs == snapshot.convertOutputs)
    let options = try #require(snapshot.convertRawOptions.first)
    #expect(options.whiteBalanceMode == .auto)
    #expect(abs(options.exposureBias - 1.25) < 0.0001)
    #expect(options.blackLevelMode == .auto)
    #expect(options.demosaicQuality == .high)
    #expect(options.linearOutput == false)
    #expect(model.latestJob?.status == .succeeded)
}

@Test @MainActor func replayParsesWhitespaceWrappedNumericAndBooleanParameters() async throws {
    let service = RecordingProcessingService()
    let rawLight = URL(fileURLWithPath: "/tmp/photonstack-replay-invalid-raw.nef")
    var project = PhotonStackProject(name: "Replay Invalid Parameter Test")
    project.addAssets(from: [rawLight])
    let lightID = try #require(project.assets.first?.id.uuidString)
    project.editGraph = EditGraph(operations: [
        EditOperation(
            kind: .stack,
            parameters: [
                "stackMethod": StackMethod.average.rawValue,
                "alignment": AlignmentMethod.none.rawValue,
                "calibrationApplied": "false",
                "calibrationLightIDs": lightID,
                "rawExposureBias": " 1.25 ",
                "rawLinear": " yes ",
            ]
        ),
    ])
    let parameters = ProcessingParameters(
        rawProcessingOptions: RawProcessingOptions(exposureBias: 0.75, linearOutput: false)
    )
    let model = PhotonStackWorkspaceModel(
        project: project,
        parameters: parameters,
        processingService: service
    )

    model.startReplayEditGraph()
    try await waitForStartedProcessingCompletion(in: model)

    let options = try #require(await service.snapshot().convertRawOptions.first)
    #expect(options.exposureBias == 1.25)
    #expect(options.linearOutput)
    #expect(model.latestJob?.status == .succeeded)
}

@Test @MainActor func curveParameterParsingRejectsPartialOrNonFinitePointLists() {
    let model = PhotonStackWorkspaceModel(processingService: RecordingProcessingService())
    let operation = EditOperation(
        kind: .curves,
        parameters: [
            "black": "nan",
            "midInput": "infinity",
            "points": "0:0,nan:0.5,1:1",
        ]
    )

    let parsed = model.curveParameters(for: operation)

    #expect(parsed?.curveBlackPoint == model.parameters.curveBlackPoint)
    #expect(parsed?.curveMidInput == model.parameters.curveMidInput)
    #expect(parsed?.curveEditorPoints == model.parameters.curveEditorPoints)
}

@Test @MainActor func registerThenStackReplayUsesFreshRegisteredSequenceOutputs() async throws {
    let service = RecordingProcessingService()
    var project = PhotonStackProject(name: "Fresh Registered Replay Test")
    project.addAssets(from: [
        URL(fileURLWithPath: "/tmp/photonstack-fresh-register-reference.nef"),
        URL(fileURLWithPath: "/tmp/photonstack-fresh-register-moving.nef"),
    ])
    let reference = project.assets[0]
    let moving = project.assets[1]
    let registeredArtifact = ProcessingArtifact(
        kind: .registeredSequence,
        name: "Registered Fresh Sequence",
        operationKind: .register,
        outputDirectory: URL(fileURLWithPath: "/tmp/photonstack-stale-register", isDirectory: true),
        outputURLs: [
            URL(fileURLWithPath: "/tmp/photonstack-stale-register/reference.tiff"),
            URL(fileURLWithPath: "/tmp/photonstack-stale-register/moving.tiff"),
        ],
        parameters: [
            "referenceID": reference.id.uuidString,
            "alignment": AlignmentMethod.distortion.rawValue,
            "sourceFrameIDs": [reference.id, moving.id].map(\.uuidString).joined(separator: ","),
        ],
        metrics: ["frames": "2"]
    )
    project.appendArtifact(registeredArtifact)
    project.editGraph = EditGraph(operations: [
        EditOperation(
            kind: .register,
            parameters: [
                "reference": reference.displayName,
                "referenceID": reference.id.uuidString,
                "selectedMovingFrameIDs": moving.id.uuidString,
                "alignment": AlignmentMethod.distortion.rawValue,
                "batch": "true",
                "artifactID": registeredArtifact.id.uuidString,
            ]
        ),
        EditOperation(
            kind: .stack,
            parameters: [
                "stackMethod": StackMethod.average.rawValue,
                "alignment": AlignmentMethod.none.rawValue,
                "inputs": "2",
                "inputPath.0": registeredArtifact.outputURLs[0].path,
                "inputPath.1": registeredArtifact.outputURLs[1].path,
                "sourceArtifact": registeredArtifact.name,
                "sourceArtifactID": registeredArtifact.id.uuidString,
                "calibrationLightIDs": [reference.id, moving.id].map(\.uuidString).joined(separator: ","),
            ]
        ),
    ])
    let model = PhotonStackWorkspaceModel(project: project, processingService: service)

    model.startReplayEditGraph()
    try await waitForStartedProcessingCompletion(in: model)

    let snapshot = await service.snapshot()
    #expect(snapshot.registerBatchCalls == 1)
    #expect(snapshot.registerBatchOutputFormat == "tiff")
    #expect(snapshot.stackInputs.map(\.lastPathComponent) == ["aligned-ref.tiff", "aligned-mov.tiff"])
    #expect(snapshot.stackInputs.allSatisfy { $0.path.contains("photonstack-stale-register") == false })
    #expect(model.latestJob?.status == .succeeded)
}

@Test @MainActor func stackReplayRebuildsMissingRegisteredIntermediateProduct() async throws {
    let service = RecordingProcessingService()
    var project = PhotonStackProject(name: "Rebuild Registered Intermediate Test")
    project.addAssets(from: [
        URL(fileURLWithPath: "/tmp/photonstack-rebuild-register-reference.nef"),
        URL(fileURLWithPath: "/tmp/photonstack-rebuild-register-moving.nef"),
    ])
    let reference = project.assets[0]
    let moving = project.assets[1]
    let registeredArtifact = ProcessingArtifact(
        kind: .registeredSequence,
        name: "Rebuild Registered Sequence",
        operationKind: .register,
        outputDirectory: URL(fileURLWithPath: "/tmp/photonstack-gone-register", isDirectory: true),
        outputURLs: [
            URL(fileURLWithPath: "/tmp/photonstack-gone-register/reference.tiff"),
            URL(fileURLWithPath: "/tmp/photonstack-gone-register/moving.tiff"),
        ],
        parameters: [
            "referenceID": reference.id.uuidString,
            "alignment": AlignmentMethod.distortion.rawValue,
            "sourceFrameIDs": [reference.id, moving.id].map(\.uuidString).joined(separator: ","),
        ],
        metrics: ["frames": "2"]
    )
    project.appendArtifact(registeredArtifact)
    project.editGraph = EditGraph(operations: [
        EditOperation(
            kind: .stack,
            parameters: [
                "stackMethod": StackMethod.average.rawValue,
                "alignment": AlignmentMethod.none.rawValue,
                "inputs": "2",
                "sourceArtifact": registeredArtifact.name,
                "sourceArtifactID": registeredArtifact.id.uuidString,
                "calibrationLightIDs": [reference.id, moving.id].map(\.uuidString).joined(separator: ","),
            ]
        ),
    ])
    let model = PhotonStackWorkspaceModel(project: project, processingService: service)

    model.startReplayEditGraph()
    try await waitForStartedProcessingCompletion(in: model)

    let snapshot = await service.snapshot()
    #expect(snapshot.registerBatchCalls == 1)
    #expect(snapshot.registerBatchReference == reference.originalURL)
    #expect(snapshot.registerBatchInputs == [moving.originalURL])
    #expect(snapshot.registerBatchOutputFormat == "fits")
    #expect(snapshot.stackInputs.map(\.lastPathComponent) == ["aligned-ref.fits", "aligned-mov.fits"])
    #expect(model.latestJob?.status == .succeeded)
}

@Test @MainActor func stackReplayFailsInsteadOfSubstitutingMissingRecordedLight() async throws {
    let service = RecordingProcessingService()
    let light = URL(fileURLWithPath: "/tmp/photonstack-stack-missing-lineage-a.tiff")
    var project = PhotonStackProject(name: "Stack Missing Input Test")
    project.addAssets(from: [light])
    let recordedAssetID = try #require(project.assets.first?.id)
    project.editGraph = EditGraph(operations: [
        EditOperation(
            kind: .stack,
            parameters: [
                "stackMethod": StackMethod.median.rawValue,
                "alignment": AlignmentMethod.none.rawValue,
                "calibrationApplied": "false",
                "calibrationLightIDs": [recordedAssetID, UUID()].map(\.uuidString).joined(separator: ","),
            ]
        ),
    ])
    let model = PhotonStackWorkspaceModel(project: project, processingService: service)

    model.startReplayEditGraph()
    try await waitForStartedProcessingCompletion(in: model)

    let snapshot = await service.snapshot()
    #expect(snapshot.stackInputs.isEmpty)
    #expect(model.latestJob?.status == .failed)
    #expect(model.errorMessage?.contains("stack") == true)
}

@Test @MainActor func stackReplayRejectsDuplicateProjectAssetIdentifiers() async throws {
    let duplicateID = UUID()
    let firstURL = URL(fileURLWithPath: "/tmp/photonstack-duplicate-replay-a.tiff")
    let secondURL = URL(fileURLWithPath: "/tmp/photonstack-duplicate-replay-b.tiff")
    let project = PhotonStackProject(
        name: "Duplicate Replay Asset Identity",
        assets: [
            PhotonStackAsset(id: duplicateID, originalURL: firstURL, kind: .tiff),
            PhotonStackAsset(id: duplicateID, originalURL: secondURL, kind: .tiff),
        ],
        editGraph: EditGraph(operations: [
            EditOperation(
                kind: .stack,
                parameters: [
                    "stackMethod": StackMethod.average.rawValue,
                    "alignment": AlignmentMethod.none.rawValue,
                    "calibrationApplied": "false",
                    "calibrationLightIDs": duplicateID.uuidString,
                ]
            ),
        ])
    )
    let service = RecordingProcessingService()
    let model = PhotonStackWorkspaceModel(project: project, processingService: service)

    model.startReplayEditGraph()
    try await waitForStartedProcessingCompletion(in: model)

    #expect(await service.snapshot().stackInputs.isEmpty)
    #expect(model.latestJob?.status == .failed)
    #expect(model.errorMessage?.contains(model.localized(.editGraphInvalidRecordedAssets)) == true)
}

@Test @MainActor func stackReplayRejectsCachedArtifactOfTheWrongKind() async throws {
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackWrongStackArtifactKind-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    let lightURL = directory.appendingPathComponent("light.png")
    let unrelatedPreviewURL = directory.appendingPathComponent("unrelated-preview.png")
    try writeSolidPNG(to: lightURL, red: 0.2, green: 0.4, blue: 0.8, width: 16, height: 8)
    try writeSolidPNG(to: unrelatedPreviewURL, red: 0.8, green: 0.2, blue: 0.1, width: 16, height: 8)
    var project = PhotonStackProject(name: "Wrong Stack Artifact Kind")
    project.addAssets(from: [lightURL])
    let light = try #require(project.assets.first)
    let unrelatedArtifact = ProcessingArtifact(
        kind: .editedImage,
        name: "Not a Stack Master",
        outputURLs: [unrelatedPreviewURL]
    )
    project.appendArtifact(unrelatedArtifact)
    project.editGraph = EditGraph(operations: [
        EditOperation(
            kind: .stack,
            parameters: [
                "artifactID": unrelatedArtifact.id.uuidString,
                "calibrationLightIDs": light.id.uuidString,
                "stackMethod": StackMethod.average.rawValue,
                "alignment": AlignmentMethod.none.rawValue,
            ]
        ),
    ])
    let service = RecordingProcessingService()
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: service,
        previewDirectory: directory.appendingPathComponent("previews", isDirectory: true)
    )

    model.startReplayEditGraph()
    try await waitForStartedProcessingCompletion(in: model)

    #expect(await service.snapshot().stackInputs == [lightURL])
    #expect(model.latestJob?.status == .succeeded)
    #expect(model.previewURL != unrelatedPreviewURL)
}

@Test @MainActor func calibratedStackReplayKeepsRecordedEmptyCalibrationGroups() async throws {
    let service = RecordingProcessingService()
    let light = URL(fileURLWithPath: "/tmp/photonstack-stack-recorded-light.tiff")
    let dark = URL(fileURLWithPath: "/tmp/photonstack-stack-recorded-dark.tiff")
    let laterBias = URL(fileURLWithPath: "/tmp/photonstack-stack-later-bias.tiff")
    var project = PhotonStackProject(name: "Stack Empty Calibration Group Test")
    project.addAssets(from: [light, dark, laterBias])
    project.updateRole(for: project.assets[1].id, role: .dark)
    project.updateRole(for: project.assets[2].id, role: .bias)
    let lightID = project.assets[0].id.uuidString
    let darkID = project.assets[1].id.uuidString
    let recordedCalibrationParameters = [
        "masterMethod": StackMethod.median.rawValue,
        "calibrationLightIDs": lightID,
        "calibrationDarkIDs": darkID,
        "calibrationBiasIDs": "",
        "calibrationFlatIDs": "",
    ]
    project.editGraph = EditGraph(operations: [
        EditOperation(kind: .calibrate, parameters: recordedCalibrationParameters),
        EditOperation(
            kind: .stack,
            parameters: recordedCalibrationParameters.merging(
                [
                    "stackMethod": StackMethod.average.rawValue,
                    "alignment": AlignmentMethod.none.rawValue,
                    "calibrationApplied": "true",
                    "inputs": "1",
                    "inputPath.0": "/tmp/missing-cache/recorded-light.calibrated.tiff",
                ],
                uniquingKeysWith: { _, new in new }
            )
        ),
    ])
    let previewDirectory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackRecordedCalibration-\(UUID().uuidString)", isDirectory: true)
    defer {
        try? FileManager.default.removeItem(at: previewDirectory)
    }
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: service,
        previewDirectory: previewDirectory
    )

    model.startReplayEditGraph()
    try await waitForStartedProcessingCompletion(in: model)

    let snapshot = await service.snapshot()
    #expect(snapshot.masterInputGroups == [[dark]])
    #expect(snapshot.calibrateLights == [light])
    #expect(snapshot.calibrateBiasInputs.count == 1)
    #expect(snapshot.calibrateBiasInputs.allSatisfy { $0 == nil })
    #expect(snapshot.calibrateDarkBiasStates == [.included])
    #expect(snapshot.calibrateFlatBiasStates == [.included])
    #expect(snapshot.stackInputs.count == 1)
    #expect(model.latestJob?.status == .succeeded)
}

@Test @MainActor func replayStackRebuildsMissingCalibratedInputsBeforeStacking() async throws {
    let service = RecordingProcessingService()
    let lightA = URL(fileURLWithPath: "/tmp/photonstack-replay-calibrated-light-a.tiff")
    let lightB = URL(fileURLWithPath: "/tmp/photonstack-replay-calibrated-light-b.tiff")
    let dark = URL(fileURLWithPath: "/tmp/photonstack-replay-dark.tiff")
    let bias = URL(fileURLWithPath: "/tmp/photonstack-replay-bias.tiff")
    let flat = URL(fileURLWithPath: "/tmp/photonstack-replay-flat.tiff")
    var project = PhotonStackProject(name: "Replay Calibrated Stack Test")
    project.addAssets(from: [lightA, lightB, dark, bias, flat])
    project.updateRole(for: project.assets[2].id, role: .dark)
    project.updateRole(for: project.assets[3].id, role: .bias)
    project.updateRole(for: project.assets[4].id, role: .flat)
    project.editGraph = EditGraph(operations: [
        EditOperation(
            kind: .calibrate,
            parameters: [
                "template": ProjectTemplate.deepSky.rawValue,
                "masterMethod": StackMethod.median.rawValue,
                "rawWhiteBalance": RawWhiteBalanceMode.daylight.rawValue,
                "rawExposureBias": "1.25",
                "rawBlackLevel": RawBlackLevelMode.auto.rawValue,
                "rawDemosaic": RawDemosaicQuality.high.rawValue,
                "rawLinear": "false",
                "darkBiasState": CalibrationBiasState.removed.rawValue,
                "flatBiasState": CalibrationBiasState.included.rawValue,
            ]
        ),
        EditOperation(
            kind: .stack,
            parameters: [
                "masterMethod": StackMethod.average.rawValue,
                "stackMethod": StackMethod.winsorized.rawValue,
                "alignment": AlignmentMethod.none.rawValue,
                "inputs": "2",
                "inputPath.0": "/tmp/missing-cache/calibrated-a.tiff",
                "inputPath.1": "/tmp/missing-cache/calibrated-b.tiff",
            ]
        ),
    ])
    let previewDirectory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackReplayCalibration-\(UUID().uuidString)", isDirectory: true)
    defer {
        try? FileManager.default.removeItem(at: previewDirectory)
    }
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: service,
        previewDirectory: previewDirectory
    )

    model.startReplayEditGraph()
    try await waitForStartedProcessingCompletion(in: model)

    let snapshot = await service.snapshot()
    #expect(snapshot.masterInputGroups == [[dark], [bias], [flat]])
    #expect(snapshot.masterMethods == [.median, .median, .median])
    let expectedRawOptions = RawProcessingOptions(
        whiteBalanceMode: .daylight,
        exposureBias: 1.25,
        blackLevelMode: .auto,
        demosaicQuality: .high,
        linearOutput: true
    )
    #expect(snapshot.masterRawOptions == [expectedRawOptions, expectedRawOptions, expectedRawOptions])
    #expect(snapshot.calibrateLights == [lightA, lightB])
    #expect(snapshot.calibrateRawOptions == [expectedRawOptions, expectedRawOptions])
    #expect(snapshot.calibrateDarkInputs.allSatisfy { $0?.lastPathComponent == "master-dark.fits" })
    #expect(snapshot.calibrateBiasInputs.allSatisfy { $0?.lastPathComponent == "master-bias.fits" })
    #expect(snapshot.calibrateFlatInputs.allSatisfy { $0?.lastPathComponent == "master-flat.fits" })
    #expect(snapshot.calibrateDarkBiasStates == [.removed, .removed])
    #expect(snapshot.calibrateFlatBiasStates == [.included, .included])
    #expect(snapshot.stackInputs.map(\.lastPathComponent) == [
        "1-photonstack-replay-calibrated-light-a.calibrated.fits",
        "2-photonstack-replay-calibrated-light-b.calibrated.fits",
    ])
    #expect(model.latestJob?.status == .succeeded)
}

@Test @MainActor func disabledCalibrationNodeKeepsReplayStackInputsUncalibrated() async throws {
    let service = RecordingProcessingService()
    let light = URL(fileURLWithPath: "/tmp/photonstack-replay-disabled-calibration-light.tiff")
    let dark = URL(fileURLWithPath: "/tmp/photonstack-replay-disabled-calibration-dark.tiff")
    var project = PhotonStackProject(name: "Disabled Replay Calibration")
    project.addAssets(from: [light, dark])
    project.updateRole(for: project.assets[1].id, role: .dark)
    project.editGraph = EditGraph(operations: [
        EditOperation(kind: .calibrate, isEnabled: false),
        EditOperation(kind: .stack, parameters: ["alignment": AlignmentMethod.none.rawValue]),
    ])
    let model = PhotonStackWorkspaceModel(project: project, processingService: service)

    model.startReplayEditGraph()
    try await waitForStartedProcessingCompletion(in: model)

    let snapshot = await service.snapshot()
    #expect(snapshot.masterInputGroups.isEmpty)
    #expect(snapshot.calibrateLights.isEmpty)
    #expect(snapshot.stackInputs == [light])
    #expect(model.latestJob?.status == .succeeded)
}

@Test @MainActor func deepSkyWorkflowReplacesTemplatePlaceholdersInExecutionOrder() async {
    let service = RecordingProcessingService()
    var project = ProjectTemplate.deepSky.makeProject(name: "Deep Sky Template Replay")
    project.addAssets(from: [
        URL(fileURLWithPath: "/tmp/photonstack-template-light-a.tiff"),
        URL(fileURLWithPath: "/tmp/photonstack-template-light-b.tiff"),
    ])
    let model = PhotonStackWorkspaceModel(
        project: project,
        parameters: ProcessingParameters(workflowStackMethod: .average, workflowAlignment: .none),
        processingService: service
    )

    await model.runStackWorkflow()

    let kinds = model.project.editGraph.operations.map(\.kind)
    #expect(kinds.filter { $0 == .calibrate }.count == 1)
    #expect(kinds.filter { $0 == .stack }.count == 1)
    #expect(kinds.filter { $0 == .stretch }.count == 1)
    let calibrationIndex = kinds.firstIndex(of: .calibrate)
    let stackIndex = kinds.firstIndex(of: .stack)
    let backgroundIndex = kinds.firstIndex(of: .background)
    let stretchIndex = kinds.firstIndex(of: .stretch)
    #expect(calibrationIndex != nil)
    #expect(stackIndex != nil)
    #expect(backgroundIndex != nil)
    #expect(stretchIndex != nil)
    if let calibrationIndex, let stackIndex, let backgroundIndex, let stretchIndex {
        #expect(calibrationIndex < stackIndex)
        #expect(stackIndex < backgroundIndex)
        #expect(backgroundIndex < stretchIndex)
    }
    #expect(model.latestJob?.status == .succeeded)
}

@Test @MainActor func replayUsesDedicatedGreenRemovalMeteorSequenceAndCrop() async throws {
    let service = RecordingProcessingService()
    let input = URL(fileURLWithPath: "/tmp/photonstack-replay-operations.tiff")
    let meteorA = URL(fileURLWithPath: "/tmp/photonstack-replay-meteor-a.tiff")
    let meteorB = URL(fileURLWithPath: "/tmp/photonstack-replay-meteor-b.tiff")
    var project = PhotonStackProject(name: "Replay Operations Test")
    project.addAssets(from: [input])
    project.editGraph = EditGraph(operations: [
        EditOperation(
            kind: .colorNeutralize,
            parameters: ["mode": "removeGreen", "amount": "0.7", "backgroundLimit": "0.34"]
        ),
        EditOperation(
            kind: .meteorRestore,
            parameters: [
                "mode": "restoreSequence",
                "sources": "2",
                "sourcePath.0": meteorA.path,
                "sourcePath.1": meteorB.path,
            ]
        ),
        EditOperation(kind: .crop, parameters: ["aspect": "16:9", "margin": "0.2"]),
    ])
    let model = PhotonStackWorkspaceModel(project: project, processingService: service)

    model.startReplayEditGraph()
    try await waitForStartedProcessingCompletion(in: model)

    let snapshot = await service.snapshot()
    #expect(snapshot.removeGreenAmount == 0.7)
    #expect(snapshot.removeGreenBackgroundLimit == 0.34)
    #expect(snapshot.restoreMeteorCalls == 2)
    #expect(snapshot.cliArguments.first == "crop")
    #expect(snapshot.cliArguments.contains("16:9"))
    #expect(model.latestJob?.status == .succeeded)
}

@Test @MainActor func meteorSequenceReplayFailsInsteadOfPartiallyRestoringMissingSource() async throws {
    let service = RecordingProcessingService()
    let input = URL(fileURLWithPath: "/tmp/photonstack-replay-incomplete-meteor-input.tiff")
    let meteorA = URL(fileURLWithPath: "/tmp/photonstack-replay-incomplete-meteor-a.tiff")
    var project = PhotonStackProject(name: "Incomplete Meteor Sequence Replay Test")
    project.addAssets(from: [input])
    project.editGraph = EditGraph(operations: [
        EditOperation(
            kind: .meteorRestore,
            parameters: [
                "mode": "restoreSequence",
                "sources": "2",
                "sourcePath.0": meteorA.path,
            ]
        ),
    ])
    let model = PhotonStackWorkspaceModel(project: project, processingService: service)

    model.startReplayEditGraph()
    try await waitForStartedProcessingCompletion(in: model)

    let snapshot = await service.snapshot()
    #expect(snapshot.restoreMeteorCalls == 0)
    #expect(model.latestJob?.status == .failed)
    #expect(model.errorMessage?.contains("meteorRestore") == true)
}

@Test @MainActor func singleMeteorRestoreRecordsAndRequiresSourceAssetIdentity() async throws {
    let recordingService = RecordingProcessingService()
    let source = URL(fileURLWithPath: "/tmp/photonstack-single-meteor-source.tiff")
    var recordedProject = PhotonStackProject(name: "Single Meteor Source Identity Test")
    recordedProject.addAssets(from: [source])
    let recordingModel = PhotonStackWorkspaceModel(project: recordedProject, processingService: recordingService)

    await recordingModel.restoreMeteorsFromSelectedAsset()

    let operation = try #require(recordingModel.project.editGraph.operations.last)
    #expect(operation.parameters["sourceAssetID"] == recordingModel.project.assets[0].id.uuidString)

    let replayService = RecordingProcessingService()
    var missingProject = recordingModel.project
    missingProject.editGraph.operations = [
        EditOperation(
            kind: .meteorRestore,
            parameters: [
                "mode": "restore",
                "source": source.lastPathComponent,
                "sourceAssetID": UUID().uuidString,
                "sourcePath": source.path,
            ]
        ),
    ]
    let replayModel = PhotonStackWorkspaceModel(project: missingProject, processingService: replayService)

    replayModel.startReplayEditGraph()
    try await waitForStartedProcessingCompletion(in: replayModel)

    let replaySnapshot = await replayService.snapshot()
    #expect(replaySnapshot.restoreMeteorCalls == 0)
    #expect(replayModel.latestJob?.status == .failed)
    #expect(replayModel.errorMessage?.contains("meteorRestore") == true)
}

@Test @MainActor func stackWorkflowEditGraphReplaysEachPipelineStageOnce() async throws {
    let workflowService = RecordingProcessingService()
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackReplayWorkflow-\(UUID().uuidString)", isDirectory: true)
    defer { try? FileManager.default.removeItem(at: directory) }
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    let lightA = directory.appendingPathComponent("photonstack-replay-workflow-a.tiff")
    let lightB = directory.appendingPathComponent("photonstack-replay-workflow-b.tiff")
    try writeSolidPNG(to: lightA, red: 0.2, green: 0.3, blue: 0.4)
    try writeSolidPNG(to: lightB, red: 0.3, green: 0.4, blue: 0.5)
    let workflowPreviewDirectory = directory.appendingPathComponent("workflow", isDirectory: true)
    var project = PhotonStackProject(name: "Replay Workflow Test")
    project.addAssets(from: [lightA, lightB])
    let workflowModel = PhotonStackWorkspaceModel(
        project: project,
        parameters: ProcessingParameters(workflowStackMethod: .average, workflowAlignment: .none),
        processingService: workflowService,
        previewDirectory: workflowPreviewDirectory
    )
    await workflowModel.runStackWorkflow()
    try? FileManager.default.removeItem(at: workflowPreviewDirectory)

    let replayService = RecordingProcessingService()
    let replayModel = PhotonStackWorkspaceModel(
        project: workflowModel.project,
        processingService: replayService,
        previewDirectory: directory.appendingPathComponent("replay", isDirectory: true)
    )
    replayModel.startReplayEditGraph()
    try await waitForStartedProcessingCompletion(in: replayModel)

    let snapshot = await replayService.snapshot()
    #expect(snapshot.stackInputs == [lightA, lightB])
    #expect(snapshot.autoStretchCalls == 1)
    #expect(snapshot.removeGreenAmount == 0.70)
    #expect(snapshot.cliArguments.first == "crop")
    #expect(replayModel.latestJob?.status == .succeeded)
}

@Test @MainActor func starDetectionAndMaskAreIndependentWorkspaceProducts() async throws {
    let service = RecordingProcessingService()
    let input = URL(fileURLWithPath: "/tmp/photonstack-star-tools.tiff")
    var project = PhotonStackProject(name: "Star Tools Test")
    project.addAssets(from: [input])
    let model = PhotonStackWorkspaceModel(project: project, processingService: service)

    model.startDetectStars(sigmaThreshold: 3.4, minPeak: 0.07, maxStars: 640)
    try await waitForStartedProcessingCompletion(in: model)
    #expect(model.detectedStarCount == 42)
    #expect(model.latestJob?.kind == .starDetect)

    model.startCreateStarMask(
        radius: 3,
        largeRadius: 9,
        layered: true,
        sigmaThreshold: 3.2,
        minPeak: 0.08
    )
    try await waitForStartedProcessingCompletion(in: model, minimumJobCount: 2)

    let snapshot = await service.snapshot()
    #expect(snapshot.starDetectCalls == 1)
    #expect(snapshot.starMaximumCount == 640)
    #expect(snapshot.starMaskCalls == 1)
    #expect(snapshot.starMaskRadius == 3)
    #expect(snapshot.largeStarMaskRadius == 9)
    #expect(snapshot.layeredStarMask == true)
    #expect(snapshot.starMaskMaximumCount == 50_000)
    #expect(model.detectedStarCount == 42)
    let artifact = try #require(model.project.artifacts.last { $0.kind == .mask })
    #expect(artifact.operationKind == .starMask)
    #expect(artifact.metrics["stars"] == "42")
    let layer = try #require(model.project.layers.last { $0.sourceArtifactID == artifact.id })
    #expect(layer.kind == .mask)
    #expect(layer.isVisible == false)
    #expect(model.canCreateLayer(from: artifact) == false)
    let layerCount = model.project.layers.count
    model.createLayer(from: artifact)
    #expect(model.project.layers.count == layerCount)
    let sourceLayer = try #require(model.project.layers.first { $0.kind == .baseImage })
    #expect(sourceLayer.inputURL == input)
    #expect(sourceLayer.maskArtifactID == artifact.id)
    #expect(model.activeLayerID == sourceLayer.id)
    #expect(model.project.editGraph.operations.suffix(2).map(\.kind) == [.starDetect, .starMask])
}

@Test @MainActor func starDetectionRejectsACompleteReportWithoutACount() async {
    let service = RecordingProcessingService(
        starDetectOutputOverride: #"{"type":"complete","command":"stars detect","limited":false}"#
    )
    let input = URL(fileURLWithPath: "/tmp/photonstack-invalid-star-report.tiff")
    var project = PhotonStackProject(name: "Invalid Star Report")
    project.addAssets(from: [input])
    let model = PhotonStackWorkspaceModel(project: project, processingService: service)

    await model.detectStars(sigmaThreshold: 3, minPeak: 0.05)

    #expect(model.latestJob?.status == .failed)
    #expect(model.detectedStarCount == nil)
    #expect(model.project.editGraph.operations.isEmpty)
}

@Test @MainActor func starDetectionRejectsFractionalCount() async {
    let service = RecordingProcessingService(
        starDetectOutputOverride: #"{"type":"complete","command":"stars detect","count":1.5}"#
    )
    let input = URL(fileURLWithPath: "/tmp/photonstack-fractional-star-count.tiff")
    var project = PhotonStackProject(name: "Fractional Star Count")
    project.addAssets(from: [input])
    let model = PhotonStackWorkspaceModel(project: project, processingService: service)

    await model.detectStars(sigmaThreshold: 3, minPeak: 0.05)

    #expect(model.latestJob?.status == .failed)
    #expect(model.detectedStarCount == nil)
    #expect(model.project.editGraph.operations.isEmpty)
}

@Test @MainActor func starMaskRejectsACompleteReportWithMissingMetrics() async {
    let service = RecordingProcessingService(
        starMaskOutputOverride: #"{"type":"complete","command":"stars mask","stars":12}"#
    )
    let input = URL(fileURLWithPath: "/tmp/photonstack-invalid-star-mask-report.tiff")
    var project = PhotonStackProject(name: "Invalid Star Mask Report")
    project.addAssets(from: [input])
    let model = PhotonStackWorkspaceModel(project: project, processingService: service)

    await model.createStarMask(
        radius: 2,
        largeRadius: 6,
        layered: true,
        sigmaThreshold: 3,
        minPeak: 0.05
    )

    #expect(model.latestJob?.status == .failed)
    #expect(model.project.artifacts.isEmpty)
    #expect(model.project.editGraph.operations.isEmpty)
}

@Test @MainActor func failedStarMaskReportDoesNotPoisonPreviewCache() async throws {
    let directory = FileManager.default.temporaryDirectory.appendingPathComponent(
        "PhotonStackStarMaskCachePoison-\(UUID().uuidString)",
        isDirectory: true
    )
    let input = directory.appendingPathComponent("input.png")
    let previewDirectory = directory.appendingPathComponent("previews", isDirectory: true)
    defer { try? FileManager.default.removeItem(at: directory) }
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    try writeSolidPNG(to: input, red: 0.1, green: 0.2, blue: 0.3, width: 64, height: 48)

    let service = RecordingProcessingService(
        starMaskOutputOverride: #"{"type":"complete","command":"stars mask","stars":12}"#
    )
    var project = PhotonStackProject(name: "Star Mask Cache Poison")
    project.addAssets(from: [input])
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: service,
        previewDirectory: previewDirectory,
        cacheBuildIdentity: "cache-poison-test"
    )

    await model.createStarMask(radius: 2, largeRadius: 6, layered: true, sigmaThreshold: 3, minPeak: 0.05)
    let firstSnapshot = await service.snapshot()
    let rejectedOutput = try #require(firstSnapshot.starMaskOutputs.first)
    #expect(model.latestJob?.status == .failed)
    #expect(firstSnapshot.starMaskCalls == 1)
    #expect(FileManager.default.fileExists(atPath: rejectedOutput.path))

    await model.createStarMask(radius: 2, largeRadius: 6, layered: true, sigmaThreshold: 3, minPeak: 0.05)

    #expect(model.latestJob?.status == .failed)
    #expect(await service.snapshot().starMaskCalls == 2)
    #expect(model.commandOutput.contains("Using cached preview") == false)
    #expect(model.project.artifacts.isEmpty)
    #expect(model.project.editGraph.operations.isEmpty)
}

@Test @MainActor func starMaskCacheWithoutStoredMetricsIsReprocessed() async throws {
    let directory = FileManager.default.temporaryDirectory.appendingPathComponent(
        "PhotonStackStarMaskMissingMetrics-\(UUID().uuidString)",
        isDirectory: true
    )
    let input = directory.appendingPathComponent("input.png")
    let previewDirectory = directory.appendingPathComponent("previews", isDirectory: true)
    defer { try? FileManager.default.removeItem(at: directory) }
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    try writeSolidPNG(to: input, red: 0.1, green: 0.2, blue: 0.3, width: 64, height: 48)

    var project = PhotonStackProject(name: "Star Mask Missing Metrics")
    project.addAssets(from: [input])
    let primingService = RecordingProcessingService()
    let primingModel = PhotonStackWorkspaceModel(
        project: project,
        processingService: primingService,
        previewDirectory: previewDirectory,
        cacheBuildIdentity: "star-mask-metadata-test"
    )
    await primingModel.createStarMask(
        radius: 2,
        largeRadius: 6,
        layered: true,
        sigmaThreshold: 3,
        minPeak: 0.05
    )
    #expect(await primingService.snapshot().starMaskCalls == 1)

    let replayService = RecordingProcessingService()
    let replayModel = PhotonStackWorkspaceModel(
        project: project,
        processingService: replayService,
        previewDirectory: previewDirectory,
        cacheBuildIdentity: "star-mask-metadata-test"
    )
    await replayModel.createStarMask(
        radius: 2,
        largeRadius: 6,
        layered: true,
        sigmaThreshold: 3,
        minPeak: 0.05
    )

    #expect(await replayService.snapshot().starMaskCalls == 1)
    #expect(replayModel.commandOutput.contains("Using cached preview") == false)
    #expect(replayModel.detectedStarCount == 42)
    let artifact = try #require(replayModel.project.artifacts.last { $0.kind == .mask })
    #expect(artifact.metrics["stars"] == "42")
    #expect(artifact.metrics["largeStars"] == "5")
}

@Test @MainActor func duplicateStarMaskArtifactsAreReprocessedWithoutGuessingAnIdentity() async throws {
    let directory = FileManager.default.temporaryDirectory.appendingPathComponent(
        "PhotonStackDuplicateStarMaskArtifacts-\(UUID().uuidString)",
        isDirectory: true
    )
    let input = directory.appendingPathComponent("input.png")
    let previewDirectory = directory.appendingPathComponent("previews", isDirectory: true)
    defer { try? FileManager.default.removeItem(at: directory) }
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    try writeSolidPNG(to: input, red: 0.1, green: 0.2, blue: 0.3, width: 64, height: 48)

    var project = PhotonStackProject(name: "Duplicate Star Mask Artifacts")
    project.addAssets(from: [input])
    let primingService = RecordingProcessingService()
    let primingModel = PhotonStackWorkspaceModel(
        project: project,
        processingService: primingService,
        previewDirectory: previewDirectory,
        cacheBuildIdentity: "duplicate-star-mask-artifacts"
    )

    await primingModel.createStarMask(
        radius: 2,
        largeRadius: 6,
        layered: true,
        sigmaThreshold: 3,
        minPeak: 0.05
    )
    let firstArtifact = try #require(primingModel.project.artifacts.first { $0.kind == .mask })
    let output = try #require(firstArtifact.previewURL)
    let duplicateArtifact = ProcessingArtifact(
        kind: .mask,
        name: "Duplicate legacy mask",
        operationKind: .starMask,
        outputURLs: [output],
        metrics: ["stars": "99", "largeStars": "9"]
    )
    var duplicateProject = primingModel.project
    duplicateProject.appendArtifact(duplicateArtifact)
    duplicateProject.workspaceState = ProjectWorkspaceState(
        selectedAssetID: duplicateProject.assets.first?.id,
        previewURL: input,
        canvasMode: .sourcePreview
    )
    let artifactCountBeforeReplay = duplicateProject.artifacts.count
    let replayService = RecordingProcessingService()
    let replayModel = PhotonStackWorkspaceModel(
        project: duplicateProject,
        processingService: replayService,
        previewDirectory: previewDirectory,
        cacheBuildIdentity: "duplicate-star-mask-artifacts"
    )

    await replayModel.createStarMask(
        radius: 2,
        largeRadius: 6,
        layered: true,
        sigmaThreshold: 3,
        minPeak: 0.05
    )

    #expect(await primingService.snapshot().starMaskCalls == 1)
    #expect(await replayService.snapshot().starMaskCalls == 1)
    #expect(replayModel.commandOutput.contains("Using cached preview") == false)
    #expect(replayModel.project.artifacts.count == artifactCountBeforeReplay + 1)
    let attachedArtifactID = try #require(replayModel.activeLayer?.maskArtifactID)
    #expect(attachedArtifactID != firstArtifact.id)
    #expect(attachedArtifactID != duplicateArtifact.id)
    #expect(replayModel.project.artifacts.first { $0.id == attachedArtifactID }?.metrics["stars"] == "42")
}

@Test @MainActor func starMaskRejectsLargeStarCountAboveTotalCount() async {
    let service = RecordingProcessingService(
        starMaskOutputOverride: #"{"type":"complete","command":"stars mask","stars":4,"largeStars":5}"#
    )
    let input = URL(fileURLWithPath: "/tmp/photonstack-invalid-large-star-count.tiff")
    var project = PhotonStackProject(name: "Invalid Large Star Count")
    project.addAssets(from: [input])
    let model = PhotonStackWorkspaceModel(project: project, processingService: service)

    await model.createStarMask(radius: 2, largeRadius: 6, layered: true, sigmaThreshold: 3, minPeak: 0.05)

    #expect(model.latestJob?.status == .failed)
    #expect(model.project.artifacts.isEmpty)
    #expect(model.project.editGraph.operations.isEmpty)
}

@Test @MainActor func replayRestoresStarDetectionAndMaskParameters() async throws {
    let service = RecordingProcessingService()
    let input = URL(fileURLWithPath: "/tmp/photonstack-replay-star-tools.tiff")
    var project = PhotonStackProject(name: "Replay Star Tools Test")
    project.addAssets(from: [input])
    project.editGraph = EditGraph(operations: [
        EditOperation(
            kind: .starDetect,
            parameters: ["sigmaThreshold": " 3.4 ", "minPeak": " 0.07 ", "maxStars": " 640 "]
        ),
        EditOperation(
            kind: .starMask,
            parameters: [
                "radius": " 3 ",
                "largeRadius": " 9 ",
                "layered": " yes ",
                "sigmaThreshold": " 3.2 ",
                "minPeak": " 0.08 ",
            ]
        ),
    ])
    let model = PhotonStackWorkspaceModel(project: project, processingService: service)

    model.startReplayEditGraph()
    try await waitForStartedProcessingCompletion(in: model)

    let snapshot = await service.snapshot()
    #expect(snapshot.starDetectCalls == 1)
    #expect(snapshot.starMaximumCount == 640)
    #expect(snapshot.starMaskCalls == 1)
    #expect(snapshot.starMaskRadius == 3)
    #expect(snapshot.largeStarMaskRadius == 9)
    #expect(snapshot.layeredStarMask == true)
    #expect(snapshot.starMaskMaximumCount == 50_000)
    #expect(model.detectedStarCount == 42)
    #expect(model.previewURL == input)
}

@Test @MainActor func satelliteMarkerNumbersRenderCanonicalUprightAndUseOneBasedLabels() throws {
    let service = RecordingProcessingService()
    let directory = FileManager.default.temporaryDirectory.appendingPathComponent("PhotonStackMarkerOrientation-\(UUID().uuidString)", isDirectory: true)
    let input = directory.appendingPathComponent("input.png")
    defer {
        try? FileManager.default.removeItem(at: directory)
    }
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    try writeSolidPNG(to: input, red: 0.02, green: 0.02, blue: 0.02, width: 320, height: 160)

    let item = ArtifactTrailItem(
        index: 0,
        kind: "satellite",
        confidence: 1,
        x1: 80,
        y1: 80,
        x2: 240,
        y2: 80,
        length: 160,
        width: 2,
        meanBrightness: 0.4,
        weight: 1,
        peakPosition: 0.5,
        taperScore: 0.5,
        path: [
            ArtifactTrailPathPoint(x: 80, y: 80),
            ArtifactTrailPathPoint(x: 160, y: 80),
            ArtifactTrailPathPoint(x: 240, y: 80),
        ]
    )
    let model = PhotonStackWorkspaceModel(
        project: PhotonStackProject(name: "Marker Orientation Test"),
        processingService: service,
        previewDirectory: directory
    )
    let markerColor = CGColor(gray: 1.0, alpha: 1.0)
    let zeroLabel = try #require(model.makeArtifactLabelImage("#0", color: markerColor, selected: true))
    let oneLabel = try #require(model.makeArtifactLabelImage("#1", color: markerColor, selected: true))
    let multiDigitLabel = try #require(model.makeArtifactLabelImage("#12", color: markerColor, selected: true))
    #expect(item.displayIndex == 1)
    #expect(item.displayLabel == "#1")
    #expect(zeroLabel.width == oneLabel.width)
    #expect(multiDigitLabel.width > oneLabel.width)
    let placement = try #require(model.artifactMarkerLabelPlacements(
        for: [item],
        selectedIndices: [item.index],
        imageSize: CGSize(width: 320, height: 160)
    )[item.index])
    #expect(placement.rect == CGRect(
        x: 168,
        y: 40,
        width: oneLabel.width,
        height: oneLabel.height
    ))

    let renderedOutput = try model.createArtifactMarkedPreview(
        input: input,
        report: ArtifactTrailReport(trails: 1, removedTrails: 0, protectedMeteors: 0, items: [item]),
        selectedIndices: [0]
    )
    let output = try #require(renderedOutput)
    #expect(output.lastPathComponent.contains("labels-v12-global-declutter-overflow-summary"))
    let source = try #require(CGImageSourceCreateWithURL(output as CFURL, nil))
    let image = try #require(CGImageSourceCreateImageAtIndex(source, 0, nil))
    let labelRect = CGRect(
        x: 168,
        y: 88,
        width: oneLabel.width,
        height: oneLabel.height
    )
    let renderedLabel = try #require(image.cropping(to: labelRect))

    func brightMask(_ source: CGImage) throws -> [Bool] {
        let provider = try #require(source.dataProvider)
        let data = try #require(provider.data)
        let bytes = try #require(CFDataGetBytePtr(data))
        let bytesPerPixel = source.bitsPerPixel / 8
        #expect(source.bitsPerComponent == 8)
        #expect(bytesPerPixel >= 3)
        return (0..<(source.width * source.height)).map { pixel in
            let offset = (pixel / source.width) * source.bytesPerRow + (pixel % source.width) * bytesPerPixel
            return bytes[offset] > 215 && bytes[offset + 1] > 215 && bytes[offset + 2] > 215
        }
    }

    let renderedMask = try brightMask(renderedLabel)
    let zeroMask = try brightMask(zeroLabel)
    let oneMask = try brightMask(oneLabel)

    func digitPixelCount(rows: ClosedRange<Int>) -> Int {
        renderedMask.indices.count { offset in
            let x = offset % oneLabel.width
            let y = offset / oneLabel.width
            return renderedMask[offset] && x >= 20 && x <= 38 && rows.contains(y)
        }
    }

    let upperDigitPixels = digitPixelCount(rows: 4...13)
    let lowerDigitPixels = digitPixelCount(rows: 18...27)
    // The upright "1" has its broad baseline in the lower rows. An upside-down label
    // moves that horizontal stroke into the upper rows and reads like a "J".
    #expect(lowerDigitPixels > upperDigitPixels)

    func overlap(with reference: [Bool], transform: (Int, Int) -> (Int, Int)) -> Int {
        var count = 0
        for offset in 0..<renderedMask.count {
            let x = offset % oneLabel.width
            let y = offset / oneLabel.width
            let transformed = transform(x, y)
            let referenceOffset = transformed.1 * oneLabel.width + transformed.0
            if renderedMask[offset] && reference[referenceOffset] {
                count += 1
            }
        }
        return count
    }

    let uprightOverlap = overlap(with: oneMask) { ($0, $1) }
    let zeroOverlap = overlap(with: zeroMask) { ($0, $1) }
    let horizontalMirrorOverlap = overlap(with: oneMask) { (oneLabel.width - 1 - $0, $1) }
    let verticalMirrorOverlap = overlap(with: oneMask) { ($0, oneLabel.height - 1 - $1) }
    #expect(uprightOverlap > zeroOverlap)
    #expect(uprightOverlap > horizontalMirrorOverlap)
    #expect(uprightOverlap > verticalMirrorOverlap)
}

@Test @MainActor func artifactMarkerPreviewKeepsAllPathsVisibleAndLabelsOnlySelectedItems() throws {
    let service = RecordingProcessingService()
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackMarkerSelection-\(UUID().uuidString)", isDirectory: true)
    let input = directory.appendingPathComponent("input.png")
    defer {
        try? FileManager.default.removeItem(at: directory)
    }
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    try writeSolidPNG(to: input, red: 0.02, green: 0.02, blue: 0.02, width: 320, height: 160)

    let selectedSatellite = ArtifactTrailItem(
        index: 0,
        kind: "satellite",
        confidence: 1,
        x1: 60,
        y1: 40,
        x2: 260,
        y2: 40,
        length: 200,
        width: 2,
        meanBrightness: 0.5,
        weight: 1,
        peakPosition: 0.5,
        taperScore: 0.1,
        path: [
            ArtifactTrailPathPoint(x: 60, y: 40),
            ArtifactTrailPathPoint(x: 160, y: 40),
            ArtifactTrailPathPoint(x: 260, y: 40),
        ]
    )
    let unselectedDrone = ArtifactTrailItem(
        index: 1,
        kind: "drone",
        confidence: 1,
        x1: 60,
        y1: 120,
        x2: 260,
        y2: 120,
        length: 200,
        width: 2,
        meanBrightness: 0.5,
        weight: 0.8,
        peakPosition: 0.5,
        taperScore: 0.1,
        path: [
            ArtifactTrailPathPoint(x: 60, y: 120),
            ArtifactTrailPathPoint(x: 160, y: 120),
            ArtifactTrailPathPoint(x: 260, y: 120),
        ]
    )
    let report = ArtifactTrailReport(
        trails: 2,
        removedTrails: 0,
        protectedMeteors: 0,
        items: [selectedSatellite, unselectedDrone]
    )
    let model = PhotonStackWorkspaceModel(
        project: PhotonStackProject(name: "Marker Selection Test"),
        processingService: service,
        previewDirectory: directory
    )

    let selectedOutputURL = try model.createArtifactMarkedPreview(
        input: input,
        report: report,
        selectedIndices: [0]
    )
    let selectedOutput = try #require(selectedOutputURL)
    let background = try rgbaPixel(in: selectedOutput, x: 20, y: 80)
    let selectedCore = try rgbaPixel(in: selectedOutput, x: 100, y: 120)
    let selectedStart = try rgbaPixel(in: selectedOutput, x: 60, y: 120)
    let selectedEnd = try rgbaPixel(in: selectedOutput, x: 260, y: 120)
    let unselectedCore = try rgbaPixel(in: selectedOutput, x: 100, y: 40)

    #expect(Int(selectedCore.green) > Int(selectedCore.red) + 60)
    #expect(Int(selectedCore.blue) > Int(selectedCore.red) + 80)
    #expect(Int(selectedStart.green) > Int(background.green) + 80)
    #expect(Int(selectedEnd.blue) > Int(background.blue) + 80)
    #expect(Int(unselectedCore.red) > Int(background.red) + 15)
    #expect(Int(unselectedCore.green) > Int(background.green) + 5)
    #expect(Int(unselectedCore.red) > Int(unselectedCore.blue) + 10)
    #expect(Int(selectedCore.red) + Int(selectedCore.green) + Int(selectedCore.blue)
        > Int(unselectedCore.red) + Int(unselectedCore.green) + Int(unselectedCore.blue) + 180)

    let selectedSource = try #require(CGImageSourceCreateWithURL(selectedOutput as CFURL, nil))
    let selectedImage = try #require(CGImageSourceCreateImageAtIndex(selectedSource, 0, nil))

    func brightPixelCount(in image: CGImage, xRange: Range<Int>, yRange: Range<Int>) throws -> Int {
        let provider = try #require(image.dataProvider)
        let data = try #require(provider.data)
        let bytes = try #require(CFDataGetBytePtr(data))
        let bytesPerPixel = image.bitsPerPixel / 8
        var count = 0
        for y in yRange where y >= 0 && y < image.height {
            for x in xRange where x >= 0 && x < image.width {
                let offset = y * image.bytesPerRow + x * bytesPerPixel
                if bytes[offset] > 205 && bytes[offset + 1] > 205 && bytes[offset + 2] > 205 {
                    count += 1
                }
            }
        }
        return count
    }

    let selectedLabelPixels = try brightPixelCount(in: selectedImage, xRange: 168..<210, yRange: 48..<80)
    let unselectedLabelPixels = try brightPixelCount(in: selectedImage, xRange: 168..<210, yRange: 132..<158)
    #expect(selectedLabelPixels > 20)
    #expect(unselectedLabelPixels == 0)

    let clearedOutputURL = try model.createArtifactMarkedPreview(
        input: input,
        report: report,
        selectedIndices: []
    )
    let clearedOutput = try #require(clearedOutputURL)
    let clearedSatellite = try rgbaPixel(in: clearedOutput, x: 100, y: 120)
    let clearedDrone = try rgbaPixel(in: clearedOutput, x: 100, y: 40)
    #expect(Int(clearedSatellite.green) > Int(background.green) + 8)
    #expect(Int(clearedDrone.red) > Int(background.red) + 15)

    let clearedSource = try #require(CGImageSourceCreateWithURL(clearedOutput as CFURL, nil))
    let clearedImage = try #require(CGImageSourceCreateImageAtIndex(clearedSource, 0, nil))
    #expect(try brightPixelCount(in: clearedImage, xRange: 168..<210, yRange: 48..<80) == 0)
    #expect(try brightPixelCount(in: clearedImage, xRange: 168..<210, yRange: 132..<158) == 0)
}

@Test @MainActor func artifactMarkerLabelsAvoidEachOtherAndStayInsideDenseCanvas() {
    let model = PhotonStackWorkspaceModel(
        project: PhotonStackProject(name: "Dense Marker Labels"),
        processingService: RecordingProcessingService()
    )
    let imageSize = CGSize(width: 480, height: 320)
    let center = CGPoint(x: 240, y: 160)
    var items: [ArtifactTrailItem] = []
    for index in 0..<10 {
        let angle = Double(index) * .pi / 10
        let dx = cos(angle) * 205
        let dy = sin(angle) * 135
        items.append(ArtifactTrailItem(
            index: index,
            kind: index.isMultiple(of: 2) ? "drone" : "satellite",
            confidence: 1,
            x1: Double(center.x) - dx,
            y1: Double(center.y) - dy,
            x2: Double(center.x) + dx,
            y2: Double(center.y) + dy,
            length: hypot(dx * 2, dy * 2),
            width: 2,
            meanBrightness: 0.5,
            weight: 1 - Double(index) * 0.01,
            peakPosition: 0.5,
            taperScore: 0.1,
            path: [
                ArtifactTrailPathPoint(x: Double(center.x) - dx, y: Double(center.y) - dy),
                ArtifactTrailPathPoint(x: Double(center.x), y: Double(center.y)),
                ArtifactTrailPathPoint(x: Double(center.x) + dx, y: Double(center.y) + dy),
            ]
        ))
    }
    items.append(ArtifactTrailItem(
        index: 10,
        kind: "airplane",
        confidence: 1,
        x1: 0,
        y1: 2,
        x2: 8,
        y2: 2,
        length: 8,
        width: 2,
        meanBrightness: 0.5,
        weight: 0.8,
        peakPosition: 0.5,
        taperScore: 0.1
    ))
    items.append(ArtifactTrailItem(
        index: 11,
        kind: "airplane",
        confidence: 1,
        x1: 472,
        y1: 318,
        x2: 480,
        y2: 318,
        length: 8,
        width: 2,
        meanBrightness: 0.5,
        weight: 0.79,
        peakPosition: 0.5,
        taperScore: 0.1
    ))
    let selectedIndices = Set(items.map(\.index))

    let placements = model.artifactMarkerLabelPlacements(
        for: items,
        selectedIndices: selectedIndices,
        imageSize: imageSize
    )
    let repeatedPlacements = model.artifactMarkerLabelPlacements(
        for: items,
        selectedIndices: selectedIndices,
        imageSize: imageSize
    )

    #expect(placements == repeatedPlacements)
    #expect(placements.count == items.count)
    let imageBounds = CGRect(origin: .zero, size: imageSize)
    let rects = items.compactMap { placements[$0.index]?.rect }
    #expect(rects.allSatisfy(imageBounds.contains))
    for leftIndex in rects.indices {
        for rightIndex in rects.indices where rightIndex > leftIndex {
            let intersection = rects[leftIndex].intersection(rects[rightIndex])
            #expect(intersection.isNull || intersection.isEmpty)
        }
    }
    #expect(placements.values.contains { $0.drawsLeader })
}

@Test func artifactTopItemsUseStableWeightConfidenceAndIndexOrdering() {
    func item(index: Int, weight: Double, confidence: Double) -> ArtifactTrailItem {
        ArtifactTrailItem(
            index: index,
            kind: "drone",
            confidence: confidence,
            x1: 0,
            y1: 0,
            x2: 10,
            y2: 0,
            length: 10,
            width: 2,
            meanBrightness: 0.1,
            weight: weight,
            peakPosition: 0.5,
            taperScore: 0.1
        )
    }

    let items = [
        item(index: 3, weight: 0.8, confidence: 0.7),
        item(index: 1, weight: 0.80005, confidence: 0.1),
        item(index: 2, weight: 0.8, confidence: 0.7),
        item(index: 0, weight: 0.8, confidence: 0.9),
    ]
    let expected = [1, 0, 2, 3]
    for permutation in [items, Array(items.reversed()), [items[2], items[0], items[3], items[1]]] {
        let report = ArtifactTrailReport(
            trails: permutation.count,
            removedTrails: 0,
            protectedMeteors: 0,
            items: permutation
        )
        #expect(report.topItems.map(\.index) == expected)
    }
}

@Test func cloudTopItemsUseStableConfidenceAndIndexOrdering() {
    func item(index: Int, confidence: Double) -> CloudRegionItem {
        CloudRegionItem(
            index: index,
            confidence: confidence,
            x: 0,
            y: 0,
            width: 10,
            height: 10,
            coverage: 0.1,
            meanLuminance: 0.5,
            backgroundLuminance: 0.2,
            maskRects: []
        )
    }

    let items = [
        item(index: 3, confidence: 0.8),
        item(index: 1, confidence: 0.9),
        item(index: 2, confidence: 0.8),
        item(index: 0, confidence: 0.9),
    ]
    let expected = [0, 1, 2, 3]
    for permutation in [items, Array(items.reversed()), [items[2], items[0], items[3], items[1]]] {
        let report = CloudRegionReport(
            clouds: permutation.count,
            removedClouds: 0,
            items: permutation
        )
        #expect(report.topItems.map(\.index) == expected)
    }
}

@Test @MainActor func cloudMarkerLabelsAvoidRegionsStayInsideAndPrioritizeSelection() {
    let model = PhotonStackWorkspaceModel(
        project: PhotonStackProject(name: "Dense Cloud Labels"),
        processingService: RecordingProcessingService()
    )
    let items = (0..<20).map { index in
        CloudRegionItem(
            index: index,
            confidence: 1 - Double(index) * 0.01,
            x: 140,
            y: 85,
            width: 80,
            height: 30,
            coverage: 0.1,
            meanLuminance: 0.5,
            backgroundLuminance: 0.2,
            maskRects: []
        )
    }
    let imageSize = CGSize(width: 360, height: 200)
    let selectedIndices: Set<Int> = [18, 19]
    let placements = model.cloudMarkerLabelPlacements(
        for: items,
        selectedIndices: selectedIndices,
        imageSize: imageSize
    )

    #expect(placements.count == items.count)
    #expect(placements[18] != nil)
    #expect(placements[19] != nil)
    let imageBounds = CGRect(origin: .zero, size: imageSize)
    let rects = items.compactMap { placements[$0.index]?.rect }
    #expect(rects.allSatisfy { imageBounds.contains($0) })
    for leftIndex in rects.indices {
        for rightIndex in rects.indices where rightIndex > leftIndex {
            let intersection = rects[leftIndex].intersection(rects[rightIndex])
            #expect(intersection.isNull || intersection.isEmpty)
        }
    }

    let tinyPlacements = model.cloudMarkerLabelPlacements(
        for: items,
        selectedIndices: selectedIndices,
        imageSize: CGSize(width: 60, height: 34)
    )
    #expect(tinyPlacements[18] != nil)
    #expect(tinyPlacements[19] == nil)
    #expect(tinyPlacements.keys.allSatisfy { selectedIndices.contains($0) })
}

@Test @MainActor func cloudMarkedPreviewDrawsLabelsAtDeclutteredFinalJPEGPositions() throws {
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackCloudLabelPreview-\(UUID().uuidString)", isDirectory: true)
    let input = directory.appendingPathComponent("input.png")
    defer {
        try? FileManager.default.removeItem(at: directory)
    }
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    try writeSolidPNG(to: input, red: 0.02, green: 0.02, blue: 0.02, width: 360, height: 200)
    let model = PhotonStackWorkspaceModel(
        project: PhotonStackProject(name: "Cloud Label JPEG"),
        processingService: RecordingProcessingService(),
        previewDirectory: directory
    )
    let items = (0..<6).map { index in
        CloudRegionItem(
            index: index,
            confidence: 1 - Double(index) * 0.05,
            x: 145,
            y: 160,
            width: 70,
            height: 30,
            coverage: 0.1,
            meanLuminance: 0.5,
            backgroundLuminance: 0.2,
            maskRects: []
        )
    }
    let selectedIndices: Set<Int> = [4, 5]
    let placements = model.cloudMarkerLabelPlacements(
        for: items,
        selectedIndices: selectedIndices,
        imageSize: CGSize(width: 360, height: 200)
    )
    let renderedOutput = try model.createCloudMarkedPreview(
        input: input,
        report: CloudRegionReport(clouds: items.count, removedClouds: 0, items: items),
        selectedIndices: selectedIndices
    )
    let output = try #require(renderedOutput)
    #expect(output.lastPathComponent.contains("labels-v3-global-declutter-overflow-summary"))

    let source = try #require(CGImageSourceCreateWithURL(output as CFURL, nil))
    let image = try #require(CGImageSourceCreateImageAtIndex(source, 0, nil))
    let provider = try #require(image.dataProvider)
    let data = try #require(provider.data)
    let bytes = try #require(CFDataGetBytePtr(data))
    let bytesPerPixel = image.bitsPerPixel / 8
    #expect(bytesPerPixel >= 3)

    for item in items {
        let placement = try #require(placements[item.index])
        let xRange = max(0, Int(floor(placement.rect.minX)))..<min(image.width, Int(ceil(placement.rect.maxX)))
        let top = image.height - Int(ceil(placement.rect.maxY))
        let bottom = image.height - Int(floor(placement.rect.minY))
        let yRange = max(0, top)..<min(image.height, bottom)
        var markedPixels = 0
        for y in yRange {
            for x in xRange {
                let offset = y * image.bytesPerRow + x * bytesPerPixel
                if bytes[offset + 1] > 70 || bytes[offset + 2] > 70 {
                    markedPixels += 1
                }
            }
        }
        #expect(markedPixels > 20)
    }
}

@Test @MainActor func artifactMarkerLabelsUseWholeCanvasWhenLocalCandidatesAreCrowded() {
    let model = PhotonStackWorkspaceModel(
        project: PhotonStackProject(name: "Crowded Marker Labels"),
        processingService: RecordingProcessingService()
    )
    let imageSize = CGSize(width: 360, height: 200)
    let items = (0..<20).map { index in
        ArtifactTrailItem(
            index: index,
            kind: "drone",
            confidence: 1,
            x1: 120,
            y1: 100,
            x2: 240,
            y2: 100,
            length: 120,
            width: 2,
            meanBrightness: 0.5,
            weight: 1 - Double(index) * 0.01,
            peakPosition: 0.5,
            taperScore: 0.1,
            path: [
                ArtifactTrailPathPoint(x: 120, y: 100),
                ArtifactTrailPathPoint(x: 180, y: 100),
                ArtifactTrailPathPoint(x: 240, y: 100),
            ]
        )
    }
    let placements = model.artifactMarkerLabelPlacements(
        for: items,
        selectedIndices: Set(items.map(\.index)),
        imageSize: imageSize
    )

    #expect(placements.count == 20)
    let imageBounds = CGRect(origin: .zero, size: imageSize)
    let rects = items.compactMap { placements[$0.index]?.rect }
    #expect(rects.allSatisfy { imageBounds.contains($0) })
    for leftIndex in rects.indices {
        for rightIndex in rects.indices where rightIndex > leftIndex {
            let intersection = rects[leftIndex].intersection(rects[rightIndex])
            #expect(intersection.isNull || intersection.isEmpty)
        }
    }
}

@Test @MainActor func artifactMarkerLabelsNeverOverlapWhenCanvasCannotFitEveryLabel() {
    let model = PhotonStackWorkspaceModel(
        project: PhotonStackProject(name: "Tiny Marker Canvas"),
        processingService: RecordingProcessingService()
    )
    let imageSize = CGSize(width: 90, height: 45)
    let items = (0..<20).map { index in
        ArtifactTrailItem(
            index: index,
            kind: "satellite",
            confidence: 1,
            x1: 10,
            y1: 22,
            x2: 80,
            y2: 22,
            length: 70,
            width: 2,
            meanBrightness: 0.5,
            weight: 1 - Double(index) * 0.01,
            peakPosition: 0.5,
            taperScore: 0.1
        )
    }
    let placements = model.artifactMarkerLabelPlacements(
        for: items,
        selectedIndices: Set(items.map(\.index)),
        imageSize: imageSize
    )

    #expect(placements.isEmpty == false)
    #expect(placements.count < items.count)
    let imageBounds = CGRect(origin: .zero, size: imageSize)
    let rects = placements.values.map(\.rect)
    #expect(rects.allSatisfy { imageBounds.contains($0) })
    for leftIndex in rects.indices {
        for rightIndex in rects.indices where rightIndex > leftIndex {
            let intersection = rects[leftIndex].intersection(rects[rightIndex])
            #expect(intersection.isNull || intersection.isEmpty)
        }
    }
}

@Test @MainActor func artifactMarkerOverflowSummaryCountsHiddenLabelsAndRendersInFinalJPEG() throws {
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackMarkerOverflow-\(UUID().uuidString)", isDirectory: true)
    let input = directory.appendingPathComponent("input.png")
    defer {
        try? FileManager.default.removeItem(at: directory)
    }
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    try writeSolidPNG(to: input, red: 0.01, green: 0.01, blue: 0.01, width: 110, height: 45)

    let model = PhotonStackWorkspaceModel(
        project: PhotonStackProject(name: "Marker Overflow Summary"),
        processingService: RecordingProcessingService(),
        previewDirectory: directory
    )
    let items = (0..<20).map { index in
        ArtifactTrailItem(
            index: index,
            kind: "satellite",
            confidence: 1,
            x1: 10,
            y1: 22,
            x2: 100,
            y2: 22,
            length: 90,
            width: 2,
            meanBrightness: 0.5,
            weight: 1 - Double(index) * 0.01,
            peakPosition: 0.5,
            taperScore: 0.1
        )
    }
    let selectedIndices = Set(items.map(\.index))
    let layout = model.artifactMarkerLabelLayout(
        for: items,
        selectedIndices: selectedIndices,
        imageSize: CGSize(width: 110, height: 45)
    )

    #expect(layout.placements.isEmpty == false)
    #expect(layout.omittedCount == items.count - layout.placements.count)
    #expect(layout.overflowText == "+\(layout.omittedCount)")
    let overflowPlacement = try #require(layout.overflowPlacement)
    let imageBounds = CGRect(x: 0, y: 0, width: 110, height: 45)
    #expect(imageBounds.contains(overflowPlacement.rect))
    #expect(layout.placements.values.allSatisfy {
        let intersection = $0.rect.intersection(overflowPlacement.rect)
        return intersection.isNull || intersection.isEmpty
    })

    let renderedOutput = try model.createArtifactMarkedPreview(
        input: input,
        report: ArtifactTrailReport(
            trails: items.count,
            removedTrails: 0,
            protectedMeteors: 0,
            items: items
        ),
        selectedIndices: selectedIndices
    )
    let output = try #require(renderedOutput)
    #expect(output.lastPathComponent.contains("labels-v12-global-declutter-overflow-summary"))
    let source = try #require(CGImageSourceCreateWithURL(output as CFURL, nil))
    let image = try #require(CGImageSourceCreateImageAtIndex(source, 0, nil))
    let pixelRect = CGRect(
        x: overflowPlacement.rect.minX,
        y: CGFloat(image.height) - overflowPlacement.rect.maxY,
        width: overflowPlacement.rect.width,
        height: overflowPlacement.rect.height
    ).integral
    let summaryImage = try #require(image.cropping(to: pixelRect))
    let provider = try #require(summaryImage.dataProvider)
    let data = try #require(provider.data)
    let bytes = try #require(CFDataGetBytePtr(data))
    let bytesPerPixel = summaryImage.bitsPerPixel / 8
    var brightPixels = 0
    for y in 0..<summaryImage.height {
        for x in 0..<summaryImage.width {
            let offset = y * summaryImage.bytesPerRow + x * bytesPerPixel
            if bytes[offset] > 200 && bytes[offset + 1] > 200 && bytes[offset + 2] > 200 {
                brightPixels += 1
            }
        }
    }
    #expect(brightPixels > 20)
}

@Test @MainActor func registerBatchUsesDistortionAndPreviewsAlignedMovingFrame() async throws {
    let service = RecordingProcessingService()
    var project = PhotonStackProject(name: "Batch Register Test")
    project.addAssets(from: [
        URL(fileURLWithPath: "/tmp/photonstack-batch-a.nef"),
        URL(fileURLWithPath: "/tmp/photonstack-batch-b.nef"),
        URL(fileURLWithPath: "/tmp/photonstack-batch-c.nef"),
    ])
    let previewDirectory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackBatchRegisterTest-\(UUID().uuidString)", isDirectory: true)
    defer {
        try? FileManager.default.removeItem(at: previewDirectory)
    }
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: service,
        previewDirectory: previewDirectory
    )
    let reference = try #require(model.project.assets.first)

    await model.registerBatch(reference: reference, alignment: .none)

    let snapshot = await service.snapshot()
    #expect(snapshot.registerBatchReference?.lastPathComponent == "photonstack-batch-a.nef")
    #expect(snapshot.registerBatchInputs.map(\.lastPathComponent) == [
        "photonstack-batch-b.nef",
        "photonstack-batch-c.nef",
    ])
    #expect(snapshot.registerBatchAlignment == .distortion)
    #expect(snapshot.registerBatchOutputFormat == "fits")
    #expect(model.previewURL?.pathExtension == "png")

    let artifact = try #require(model.project.artifacts.first { $0.kind == .registeredSequence })
    #expect(artifact.frameCount == 3)
    #expect(artifact.outputURLs.allSatisfy { $0.pathExtension == "fits" })
    #expect(artifact.scientificURL?.pathExtension == "fits")
    #expect(artifact.previewURL == model.previewURL)
    #expect(artifact.metrics["failedFrames"] == "0")
    #expect(artifact.frames.dropFirst().first?.metrics["matches"] == "42")
    #expect(model.project.layers.contains {
        $0.sourceArtifactID == artifact.id &&
            $0.kind == .baseImage &&
            $0.inputURL == artifact.previewURL &&
            $0.inputURL?.pathExtension == "png"
    })

    let operation = try #require(model.project.editGraph.operations.first { $0.kind == .register })
    #expect(operation.parameters["batch"] == "true")
    #expect(operation.parameters["frames"] == "3")

    let replayService = RecordingProcessingService()
    let replayModel = PhotonStackWorkspaceModel(
        project: model.project,
        processingService: replayService,
        previewDirectory: previewDirectory.appendingPathComponent("replay", isDirectory: true)
    )
    replayModel.startReplayEditGraph()
    try await waitForStartedProcessingCompletion(in: replayModel)
    let replaySnapshot = await replayService.snapshot()
    #expect(replaySnapshot.registerBatchReference == reference.originalURL)
    #expect(replaySnapshot.registerBatchInputs.map(\.lastPathComponent) == [
        "photonstack-batch-b.nef",
        "photonstack-batch-c.nef",
    ])
    #expect(replaySnapshot.registerBatchAlignment == .distortion)
    #expect(replaySnapshot.registerBatchOutputFormat == "tiff")
}

@Test @MainActor func registerBatchUsesSelectedMovingFramesOnly() async throws {
    let service = RecordingProcessingService()
    var project = PhotonStackProject(name: "Selected Batch Register Test")
    project.addAssets(from: [
        URL(fileURLWithPath: "/tmp/photonstack-selected-a.nef"),
        URL(fileURLWithPath: "/tmp/photonstack-selected-b.nef"),
        URL(fileURLWithPath: "/tmp/photonstack-selected-c.nef"),
        URL(fileURLWithPath: "/tmp/photonstack-selected-d.nef"),
    ])
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: service
    )
    let reference = try #require(model.project.assets.first)
    let second = try #require(model.project.assets.dropFirst().first)
    let third = try #require(model.project.assets.dropFirst(2).first)
    let fourth = try #require(model.project.assets.dropFirst(3).first)

    #expect(model.batchRegistrationSelectedMovingCount(reference: reference) == 3)

    model.setBatchRegistrationFrame(third, isSelected: false)
    await model.registerBatch(reference: reference, alignment: .distortion)

    let snapshot = await service.snapshot()
    #expect(snapshot.registerBatchInputs.map(\.lastPathComponent) == [
        second.originalURL.lastPathComponent,
        fourth.originalURL.lastPathComponent,
    ])

    let operation = try #require(model.project.editGraph.operations.first { $0.kind == .register })
    #expect(operation.parameters["frames"] == "3")
    #expect(operation.parameters["movingFrames"] == "2")
    #expect(operation.parameters["selectedMovingFrameIDs"]?.contains(second.id.uuidString) == true)
    #expect(operation.parameters["selectedMovingFrameIDs"]?.contains(third.id.uuidString) == false)
    #expect(operation.parameters["selectedMovingFrameIDs"]?.contains(fourth.id.uuidString) == true)
}

@Test @MainActor func registerBatchRejectsIncompleteSuccessReport() async throws {
    let service = RecordingProcessingService(
        registerBatchOutputOverride: #"{"type":"complete","command":"register-batch","items":[]}"#
    )
    var project = PhotonStackProject(name: "Invalid Batch Register Report Test")
    project.addAssets(from: [
        URL(fileURLWithPath: "/tmp/photonstack-invalid-report-reference.nef"),
        URL(fileURLWithPath: "/tmp/photonstack-invalid-report-moving.nef"),
    ])
    let model = PhotonStackWorkspaceModel(project: project, processingService: service)
    let reference = try #require(model.project.assets.first)

    await model.registerBatch(reference: reference, alignment: .distortion)

    #expect(model.latestJob?.status == .failed)
    #expect(model.errorMessage?.contains("register-batch") == true)
    #expect(model.project.artifacts.contains { $0.kind == .registeredSequence } == false)
    #expect(model.project.layers.contains { $0.sourceArtifactID != nil } == false)
    #expect(model.project.editGraph.operations.contains { $0.kind == .register } == false)
}

@Test @MainActor func registerBatchRejectsReportWhenOneOutputFileIsMissing() async throws {
    let service = RecordingProcessingService(omitLastRegisterBatchOutputFile: true)
    var project = PhotonStackProject(name: "Missing Batch Output Test")
    project.addAssets(from: [
        URL(fileURLWithPath: "/tmp/photonstack-missing-output-reference.nef"),
        URL(fileURLWithPath: "/tmp/photonstack-missing-output-moving.nef"),
    ])
    let previewDirectory = FileManager.default.temporaryDirectory.appendingPathComponent(
        "PhotonStackMissingBatchOutput-\(UUID().uuidString)",
        isDirectory: true
    )
    defer { try? FileManager.default.removeItem(at: previewDirectory) }
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: service,
        previewDirectory: previewDirectory
    )
    let reference = try #require(model.project.assets.first)

    await model.registerBatch(reference: reference, alignment: .distortion)

    #expect(model.latestJob?.status == .failed)
    #expect(model.errorMessage?.contains("register-batch") == true)
    #expect(model.project.artifacts.contains { $0.kind == .registeredSequence } == false)
    #expect(model.project.editGraph.operations.contains { $0.kind == .register } == false)
}

@Test @MainActor func registerBatchRejectsIncorrectReferenceFlags() async throws {
    let service = RecordingProcessingService(invalidRegisterBatchReferenceFlags: true)
    var project = PhotonStackProject(name: "Invalid Batch Reference Flags")
    project.addAssets(from: [
        URL(fileURLWithPath: "/tmp/photonstack-reference-flag-reference.nef"),
        URL(fileURLWithPath: "/tmp/photonstack-reference-flag-moving.nef"),
    ])
    let previewDirectory = FileManager.default.temporaryDirectory.appendingPathComponent(
        "PhotonStackInvalidBatchReferenceFlags-\(UUID().uuidString)",
        isDirectory: true
    )
    defer { try? FileManager.default.removeItem(at: previewDirectory) }
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: service,
        previewDirectory: previewDirectory
    )
    let reference = try #require(model.project.assets.first)

    await model.registerBatch(reference: reference, alignment: .distortion)

    #expect(model.latestJob?.status == .failed)
    #expect(model.errorMessage?.contains("register-batch") == true)
    #expect(model.project.artifacts.contains { $0.kind == .registeredSequence } == false)
    #expect(model.project.editGraph.operations.contains { $0.kind == .register } == false)
}

@Test @MainActor func cancellingBatchRegistrationHistogramPreservesWorkspaceState() async throws {
    let service = RecordingProcessingService(
        histogramDelayNanoseconds: 300_000_000,
        ignoreHistogramCancellation: true
    )
    var project = PhotonStackProject(name: "Cancelled Batch Registration")
    project.addAssets(from: [
        URL(fileURLWithPath: "/tmp/photonstack-cancel-register-reference.nef"),
        URL(fileURLWithPath: "/tmp/photonstack-cancel-register-moving.nef"),
    ])
    let previewDirectory = FileManager.default.temporaryDirectory.appendingPathComponent(
        "PhotonStackCancelledBatchRegistration-\(UUID().uuidString)",
        isDirectory: true
    )
    defer { try? FileManager.default.removeItem(at: previewDirectory) }
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: service,
        previewDirectory: previewDirectory
    )
    let reference = try #require(model.project.assets.first)
    let initialPreview = model.previewURL
    let initialArtifacts = model.project.artifacts
    let initialLayers = model.project.layers

    model.startRegisterBatch(reference: reference, alignment: .distortion)
    try await waitForHistogramCall(from: service)
    model.cancelCurrentTask()
    try await waitForProcessingCompletion(in: model)

    #expect(model.latestJob?.status == .cancelled)
    #expect(model.previewURL == initialPreview)
    #expect(model.project.artifacts == initialArtifacts)
    #expect(model.project.layers == initialLayers)
    #expect(model.project.editGraph.operations.isEmpty)
}

@Test @MainActor func registerBatchReplayPreservesRecordedMovingFrameOrder() async throws {
    let service = RecordingProcessingService()
    var project = PhotonStackProject(name: "Ordered Batch Register Replay Test")
    project.addAssets(from: [
        URL(fileURLWithPath: "/tmp/photonstack-register-order-reference.nef"),
        URL(fileURLWithPath: "/tmp/photonstack-register-order-second.nef"),
        URL(fileURLWithPath: "/tmp/photonstack-register-order-third.nef"),
    ])
    let reference = project.assets[0]
    let second = project.assets[1]
    let third = project.assets[2]
    project.editGraph = EditGraph(operations: [
        EditOperation(
            kind: .register,
            parameters: [
                "reference": reference.displayName,
                "referenceID": reference.id.uuidString,
                "selectedMovingFrameIDs": [third.id, second.id].map(\.uuidString).joined(separator: ","),
                "alignment": AlignmentMethod.distortion.rawValue,
                "batch": "true",
            ]
        ),
    ])
    let model = PhotonStackWorkspaceModel(project: project, processingService: service)

    model.startReplayEditGraph()
    try await waitForStartedProcessingCompletion(in: model)

    let snapshot = await service.snapshot()
    #expect(snapshot.registerBatchReference == reference.originalURL)
    #expect(snapshot.registerBatchInputs == [third.originalURL, second.originalURL])
    #expect(model.latestJob?.status == .succeeded)
}

@Test @MainActor func registerBatchReplayFailsInsteadOfSubstitutingMissingRecordedFrame() async throws {
    let service = RecordingProcessingService()
    var project = PhotonStackProject(name: "Missing Batch Register Replay Test")
    project.addAssets(from: [
        URL(fileURLWithPath: "/tmp/photonstack-register-missing-reference.nef"),
        URL(fileURLWithPath: "/tmp/photonstack-register-current-moving.nef"),
    ])
    let reference = project.assets[0]
    project.editGraph = EditGraph(operations: [
        EditOperation(
            kind: .register,
            parameters: [
                "reference": reference.displayName,
                "referenceID": reference.id.uuidString,
                "selectedMovingFrameIDs": UUID().uuidString,
                "alignment": AlignmentMethod.distortion.rawValue,
                "batch": "true",
            ]
        ),
    ])
    let model = PhotonStackWorkspaceModel(project: project, processingService: service)

    model.startReplayEditGraph()
    try await waitForStartedProcessingCompletion(in: model)

    let snapshot = await service.snapshot()
    #expect(snapshot.registerBatchReference == nil)
    #expect(snapshot.registerBatchInputs.isEmpty)
    #expect(model.latestJob?.status == .failed)
    #expect(model.errorMessage?.contains("register") == true)
}

@Test @MainActor func registerBatchRequiresAtLeastOneSelectedMovingFrame() async throws {
    let service = RecordingProcessingService()
    var project = PhotonStackProject(name: "Empty Batch Register Selection Test")
    project.addAssets(from: [
        URL(fileURLWithPath: "/tmp/photonstack-empty-a.nef"),
        URL(fileURLWithPath: "/tmp/photonstack-empty-b.nef"),
    ])
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: service
    )
    let reference = try #require(model.project.assets.first)

    model.clearBatchRegistrationFrames()
    await model.registerBatch(reference: reference, alignment: .distortion)

    let snapshot = await service.snapshot()
    #expect(snapshot.registerBatchReference == nil)
    #expect(model.errorMessage == model.localized(.registrationNoFramesSelected))
}

@Test @MainActor func singleRegistrationCreatesSmartObjectAndBaseLayer() async throws {
    let service = RecordingProcessingService()
    var project = PhotonStackProject(name: "Single Registration Product Test")
    project.addAssets(from: [
        URL(fileURLWithPath: "/tmp/photonstack-register-reference.tiff"),
        URL(fileURLWithPath: "/tmp/photonstack-register-moving.tiff"),
    ])
    let model = PhotonStackWorkspaceModel(project: project, processingService: service)
    let reference = try #require(model.project.assets.first)
    let moving = try #require(model.project.assets.last)

    await model.register(reference: reference, moving: moving, alignment: .translation)

    let artifact = try #require(model.project.artifacts.last { $0.operationKind == .register })
    #expect(artifact.kind == .registeredSequence)
    #expect(artifact.outputURLs.count == 1)
    #expect(artifact.outputURLs.first?.pathExtension == "fits")
    #expect(artifact.scientificURL == artifact.outputURLs.first)
    #expect(artifact.previewURL == model.previewURL)
    #expect(artifact.previewURL?.pathExtension == "png")
    #expect(artifact.metrics["matches"] == "8")
    let layer = try #require(model.project.layers.last)
    #expect(layer.kind == .baseImage)
    #expect(layer.sourceArtifactID == artifact.id)
    #expect(layer.inputURL == artifact.previewURL)
    #expect(layer.inputURL != artifact.scientificURL)
    #expect(model.activeLayerID == layer.id)
}

@Test @MainActor func singleRegistrationRejectsAReportWhoseModeDoesNotMatchTheRequest() async throws {
    let service = RecordingProcessingService(
        registerOutputOverride: #"{"type":"complete","command":"register","mode":"affine","dx":1,"dy":2,"scale":1,"rotationRadians":0,"matches":8}"#
    )
    var project = PhotonStackProject(name: "Invalid Registration Report")
    project.addAssets(from: [
        URL(fileURLWithPath: "/tmp/photonstack-invalid-register-reference.tiff"),
        URL(fileURLWithPath: "/tmp/photonstack-invalid-register-moving.tiff"),
    ])
    let model = PhotonStackWorkspaceModel(project: project, processingService: service)
    let reference = try #require(model.project.assets.first)
    let moving = try #require(model.project.assets.last)

    await model.register(reference: reference, moving: moving, alignment: .translation)

    #expect(model.latestJob?.status == .failed)
    #expect(model.registrationReport == nil)
    #expect(model.project.artifacts.isEmpty)
    #expect(model.project.layers.isEmpty)
    #expect(model.project.editGraph.operations.isEmpty)
}

@Test @MainActor func drizzleCreatesSmartObjectAndBaseLayer() async throws {
    let service = RecordingProcessingService()
    var project = PhotonStackProject(name: "Drizzle Product Test")
    project.addAssets(from: [
        URL(fileURLWithPath: "/tmp/photonstack-drizzle-a.tiff"),
        URL(fileURLWithPath: "/tmp/photonstack-drizzle-b.tiff"),
    ])
    let model = PhotonStackWorkspaceModel(project: project, processingService: service)

    await model.runDrizzle(scale: 2, pixfrac: 0.8, alignment: .distortion)

    let artifact = try #require(model.project.artifacts.last { $0.operationKind == .drizzle })
    #expect(artifact.kind == .editedImage)
    #expect(artifact.metrics["frames"] == "2")
    #expect(artifact.metrics["scale"] == "2")
    #expect(artifact.metrics["pixfrac"] == "0.8")
    #expect(artifact.metrics["alignment"] == "distortion")
    #expect(artifact.parameters["pixfrac"] == "0.8")
    #expect(artifact.parameters["alignment"] == "distortion")
    #expect(artifact.previewURL?.pathExtension == "tiff")
    let snapshot = await service.snapshot()
    #expect(snapshot.drizzleScale == 2)
    #expect(snapshot.drizzlePixfrac == 0.8)
    #expect(snapshot.drizzleAlignment == .distortion)
    let layer = try #require(model.project.layers.last)
    #expect(layer.kind == .baseImage)
    #expect(layer.sourceArtifactID == artifact.id)
}

@Test @MainActor func drizzleReplayKeepsRecordedInputsAfterAssetRolesChange() async throws {
    let service = RecordingProcessingService()
    let inputs = [
        URL(fileURLWithPath: "/tmp/photonstack-drizzle-lineage-a.tiff"),
        URL(fileURLWithPath: "/tmp/photonstack-drizzle-lineage-b.tiff"),
        URL(fileURLWithPath: "/tmp/photonstack-drizzle-lineage-c.tiff"),
    ]
    var project = PhotonStackProject(name: "Drizzle Input Lineage Test")
    project.addAssets(from: inputs)
    let model = PhotonStackWorkspaceModel(project: project, processingService: service)

    await model.runDrizzle(scale: 2, pixfrac: 0.8, alignment: .distortion)
    let operation = try #require(model.project.editGraph.operations.last { $0.kind == .drizzle })
    #expect(operation.parameters["lightIDs"] == model.project.assets.map(\.id.uuidString).joined(separator: ","))

    model.setRole(.dark, for: model.project.assets[1])
    model.startReplayEditGraph()
    try await waitForStartedProcessingCompletion(in: model, minimumJobCount: 2)

    let snapshot = await service.snapshot()
    #expect(snapshot.drizzleInputGroups == [inputs, inputs])
}

@Test @MainActor func drizzleReplayFailsInsteadOfSubstitutingMissingRecordedAsset() async throws {
    let service = RecordingProcessingService()
    let input = URL(fileURLWithPath: "/tmp/photonstack-drizzle-missing-lineage-a.tiff")
    var project = PhotonStackProject(name: "Drizzle Missing Input Test")
    project.addAssets(from: [input])
    let recordedAssetID = try #require(project.assets.first?.id)
    project.editGraph = EditGraph(operations: [
        EditOperation(
            kind: .drizzle,
            parameters: [
                "scale": "2",
                "pixfrac": "0.8",
                "alignment": AlignmentMethod.distortion.rawValue,
                "lightIDs": [recordedAssetID, UUID()].map(\.uuidString).joined(separator: ","),
            ]
        ),
    ])
    let model = PhotonStackWorkspaceModel(project: project, processingService: service)

    model.startReplayEditGraph()
    try await waitForStartedProcessingCompletion(in: model)

    let snapshot = await service.snapshot()
    #expect(snapshot.drizzleInputGroups.isEmpty)
    #expect(model.latestJob?.status == .failed)
    #expect(model.errorMessage?.contains("drizzle") == true)
}

@Test @MainActor func legacyDrizzleReplayWithoutRecordedAssetIDsUsesCurrentLights() async throws {
    let service = RecordingProcessingService()
    let inputs = [
        URL(fileURLWithPath: "/tmp/photonstack-drizzle-legacy-a.tiff"),
        URL(fileURLWithPath: "/tmp/photonstack-drizzle-legacy-b.tiff"),
    ]
    var project = PhotonStackProject(name: "Legacy Drizzle Replay Test")
    project.addAssets(from: inputs)
    project.editGraph = EditGraph(operations: [
        EditOperation(
            kind: .drizzle,
            parameters: [
                "scale": "2",
                "pixfrac": "0.8",
                "alignment": AlignmentMethod.distortion.rawValue,
            ]
        ),
    ])
    let model = PhotonStackWorkspaceModel(project: project, processingService: service)

    model.startReplayEditGraph()
    try await waitForStartedProcessingCompletion(in: model)

    let snapshot = await service.snapshot()
    #expect(snapshot.drizzleInputGroups == [inputs])
    #expect(model.latestJob?.status == .succeeded)
}

@Test @MainActor func generalEditGraphReplayBindsPreviewToTerminalOperation() async throws {
    let service = RecordingProcessingService()
    let inputs = [
        URL(fileURLWithPath: "/tmp/photonstack-drizzle-general-replay-a.tiff"),
        URL(fileURLWithPath: "/tmp/photonstack-drizzle-general-replay-b.tiff"),
    ]
    var project = PhotonStackProject(name: "General Replay Preview Identity Test")
    project.addAssets(from: inputs)
    let operation = EditOperation(
        kind: .drizzle,
        parameters: [
            "scale": "2",
            "pixfrac": "0.8",
            "alignment": AlignmentMethod.distortion.rawValue,
            "lightIDs": project.assets.map(\.id.uuidString).joined(separator: ","),
        ]
    )
    project.editGraph = EditGraph(operations: [operation])
    let model = PhotonStackWorkspaceModel(project: project, processingService: service)
    #expect(model.previewOperationID == nil)

    model.startReplayEditGraph()
    try await waitForStartedProcessingCompletion(in: model)

    #expect(model.latestJob?.status == .succeeded)
    #expect(model.previewOperationID == operation.id)
}

@Test @MainActor func mosaicCreatesSmartObjectAndBaseLayer() async throws {
    let service = RecordingProcessingService()
    var project = PhotonStackProject(name: "Mosaic Product Test")
    project.addAssets(from: [
        URL(fileURLWithPath: "/tmp/photonstack-mosaic-a.tiff"),
        URL(fileURLWithPath: "/tmp/photonstack-mosaic-b.tiff"),
    ])
    for asset in project.assets {
        project.updateRole(for: asset.id, role: .mosaic)
    }
    let model = PhotonStackWorkspaceModel(project: project, processingService: service)

    await model.runMosaic()

    let artifact = try #require(model.project.artifacts.last { $0.operationKind == .mosaic })
    #expect(artifact.kind == .editedImage)
    #expect(artifact.outputDirectory != nil)
    #expect(artifact.metrics["panels"] == "2")
    #expect(model.previewURL?.lastPathComponent == "mosaic-preview.png")
    let report = try #require(model.mosaicQualityReport)
    #expect(report.autoAligned)
    #expect(report.fallbackPanels == 0)
    let placement = try #require(report.placements.last)
    #expect(placement.model == "affine")
    #expect(placement.references == 1)
    #expect(placement.reducedModel == false)
    #expect(placement.coarseAlignment == true)
    #expect(abs(placement.scale - 1.01) < 0.001)
    #expect(abs(placement.rotationDegrees - 1.701) < 0.01)
    let layer = try #require(model.project.layers.last)
    #expect(layer.kind == .baseImage)
    #expect(layer.sourceArtifactID == artifact.id)
    #expect(model.currentInputURL == artifact.previewURL)
    let operation = try #require(model.project.editGraph.operations.last { $0.kind == .mosaic })
    #expect(model.previewOperationID == operation.id)
}

@Test @MainActor func mosaicPreviewCacheRequiresItsQualityReport() async throws {
    let directory = FileManager.default.temporaryDirectory.appendingPathComponent(
        "PhotonStackMosaicPreviewReportCache-\(UUID().uuidString)",
        isDirectory: true
    )
    let firstInput = directory.appendingPathComponent("panel-a.png")
    let secondInput = directory.appendingPathComponent("panel-b.png")
    let previewDirectory = directory.appendingPathComponent("previews", isDirectory: true)
    defer { try? FileManager.default.removeItem(at: directory) }
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    try writeSolidPNG(to: firstInput, red: 0.2, green: 0.3, blue: 0.4, width: 32, height: 24)
    try writeSolidPNG(to: secondInput, red: 0.4, green: 0.3, blue: 0.2, width: 32, height: 24)

    var project = PhotonStackProject(name: "Mosaic Preview Report Cache")
    project.addAssets(from: [firstInput, secondInput])
    for asset in project.assets {
        project.updateRole(for: asset.id, role: .mosaic)
    }
    let service = RecordingProcessingService()
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: service,
        previewDirectory: previewDirectory,
        cacheBuildIdentity: "mosaic-report-cache-test"
    )

    await model.runMosaicPreview()
    let firstReport = try #require(model.mosaicQualityReport)
    #expect(await service.snapshot().mosaicInputGroups.count == 1)

    await model.runMosaicPreview()
    #expect(await service.snapshot().mosaicInputGroups.count == 1)
    #expect(model.commandOutput.contains("Using cached preview"))
    #expect(model.mosaicQualityReport == firstReport)

    let restoredService = RecordingProcessingService()
    let restoredModel = PhotonStackWorkspaceModel(
        project: project,
        processingService: restoredService,
        previewDirectory: previewDirectory,
        cacheBuildIdentity: "mosaic-report-cache-test"
    )
    await restoredModel.runMosaicPreview()

    #expect(await restoredService.snapshot().mosaicInputGroups.count == 1)
    #expect(restoredModel.commandOutput.contains("Using cached preview") == false)
    #expect(restoredModel.mosaicQualityReport != nil)
}

@Test @MainActor func mosaicRejectsAQualityReportWithMissingPanelPlacements() async {
    let service = RecordingProcessingService(
        mosaicOutputOverride: #"{"type":"complete","command":"mosaic","autoAligned":true,"matches":12,"fallbackPanels":0,"exposureMatched":true,"blend":"multiband","placements":[{"index":0,"x":0,"y":0,"a":1,"b":0,"c":0,"d":1,"dx":0,"dy":0,"model":"manual","matches":0,"references":0,"autoAligned":false,"fallback":false,"reducedModel":false,"coarseAlignment":false}]}"#
    )
    var project = PhotonStackProject(name: "Invalid Mosaic Report")
    project.addAssets(from: [
        URL(fileURLWithPath: "/tmp/photonstack-invalid-mosaic-a.tiff"),
        URL(fileURLWithPath: "/tmp/photonstack-invalid-mosaic-b.tiff"),
    ])
    for asset in project.assets {
        project.updateRole(for: asset.id, role: .mosaic)
    }
    let model = PhotonStackWorkspaceModel(project: project, processingService: service)

    await model.runMosaic()

    #expect(model.latestJob?.status == .failed)
    #expect(model.mosaicQualityReport == nil)
    #expect(model.project.artifacts.isEmpty)
    #expect(model.project.layers.isEmpty)
    #expect(model.project.editGraph.operations.isEmpty)
}

@Test @MainActor func mosaicReplayKeepsRecordedPanelOrderAfterRolesAndOrderChange() async throws {
    let service = RecordingProcessingService()
    let inputs = [
        URL(fileURLWithPath: "/tmp/photonstack-mosaic-lineage-a.tiff"),
        URL(fileURLWithPath: "/tmp/photonstack-mosaic-lineage-b.tiff"),
        URL(fileURLWithPath: "/tmp/photonstack-mosaic-lineage-c.tiff"),
    ]
    var project = PhotonStackProject(name: "Mosaic Input Lineage Test")
    project.addAssets(from: inputs)
    for asset in project.assets {
        project.updateRole(for: asset.id, role: .mosaic)
    }
    let model = PhotonStackWorkspaceModel(project: project, processingService: service)

    await model.runMosaic()
    let operation = try #require(model.project.editGraph.operations.last { $0.kind == .mosaic })
    #expect(operation.parameters["panelIDs"] == model.project.assets.map(\.id.uuidString).joined(separator: ","))

    let first = model.project.assets[0]
    let second = model.project.assets[1]
    let third = model.project.assets[2]
    model.setRole(.light, for: second)
    model.moveMosaicPanel(third.id, before: first.id)
    model.startReplayEditGraph()
    try await waitForStartedProcessingCompletion(in: model, minimumJobCount: 2)

    let snapshot = await service.snapshot()
    #expect(snapshot.mosaicInputGroups == [inputs, inputs])
}

@Test @MainActor func meteorExtractionCreatesSmartObjectAndScreenLayer() async throws {
    let service = RecordingProcessingService()
    var project = PhotonStackProject(name: "Meteor Product Test")
    project.addAssets(from: [URL(fileURLWithPath: "/tmp/photonstack-meteor-source.tiff")])
    let model = PhotonStackWorkspaceModel(project: project, processingService: service)

    await model.extractMeteorLayer()

    let artifact = try #require(model.project.artifacts.last { $0.kind == .meteorLayer })
    #expect(artifact.operationKind == .meteorRestore)
    let layer = try #require(model.project.layers.last)
    #expect(layer.kind == .meteors)
    #expect(layer.blendMode == .screen)
    #expect(layer.sourceArtifactID == artifact.id)
}

@Test @MainActor func processingLayerControlsMutateProjectLayerStack() {
    let service = RecordingProcessingService()
    let bottomLayer = ProcessingLayer(
        name: "Registered Base",
        kind: .baseImage,
        inputURL: URL(fileURLWithPath: "/tmp/registered-base.tiff"),
        opacity: 0.6
    )
    let topLayer = ProcessingLayer(
        name: "Edited Result",
        kind: .adjustment,
        inputURL: URL(fileURLWithPath: "/tmp/edited-result.png"),
        blendMode: .screen,
        opacity: 0.8
    )
    var project = PhotonStackProject(name: "Layer Test")
    project.appendLayer(bottomLayer)
    project.appendLayer(topLayer)

    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: service
    )

    model.setLayerOpacity(bottomLayer.id, opacity: .nan)
    #expect(model.layerOpacity(bottomLayer.id) == 0.6)
    model.setLayerVisibility(bottomLayer.id, isVisible: false)
    model.setLayerOpacity(bottomLayer.id, opacity: 1.5)
    model.setLayerBlendMode(bottomLayer.id, blendMode: .lighten)

    #expect(model.layerIsVisible(bottomLayer.id) == false)
    #expect(model.layerOpacity(bottomLayer.id) == 1.0)
    #expect(model.layerBlendMode(bottomLayer.id) == .lighten)
    #expect(model.canMoveProcessingLayer(bottomLayer.id, direction: .up))
    #expect(model.canMoveProcessingLayer(bottomLayer.id, direction: .down) == false)

    model.moveProcessingLayer(bottomLayer.id, direction: .up)

    #expect(model.project.layers.map(\.id) == [topLayer.id, bottomLayer.id])
    #expect(model.canMoveProcessingLayer(bottomLayer.id, direction: .up) == false)
    #expect(model.canMoveProcessingLayer(bottomLayer.id, direction: .down))

    model.deleteProcessingLayer(bottomLayer.id)
    #expect(model.project.layers.map(\.id) == [topLayer.id])
}

@Test @MainActor func malformedPersistedMaskParametersFallBackToFiniteValues() throws {
    let directory = FileManager.default.temporaryDirectory.appendingPathComponent(
        "PhotonStackMalformedMaskParameters-\(UUID().uuidString)",
        isDirectory: true
    )
    let input = directory.appendingPathComponent("input.png")
    let mask = directory.appendingPathComponent("mask.png")
    defer {
        try? FileManager.default.removeItem(at: directory)
    }
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    try writeSolidPNG(to: input, red: 0.1, green: 0.1, blue: 0.1, width: 24, height: 16)
    try writeSolidPNG(to: mask, red: 1, green: 1, blue: 1, width: 24, height: 16)

    let maskArtifact = ProcessingArtifact(kind: .mask, name: "Mask", outputURLs: [mask])
    let layer = ProcessingLayer(
        name: "Masked Layer",
        kind: .adjustment,
        inputURL: input,
        maskArtifactID: maskArtifact.id,
        opacity: 0.4,
        parameters: [
            "maskDensity": "nan",
            "maskFeatherFraction": "infinity",
        ]
    )
    var project = PhotonStackProject(name: "Malformed Mask Parameters")
    project.artifacts = [maskArtifact]
    project.layers = [layer]
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: RecordingProcessingService()
    )

    #expect(model.layerMaskDensity(layer.id) == 1)
    #expect(model.layerMaskFeatherRadius(layer.id) == 0)
    model.setLayerOpacity(layer.id, opacity: .nan)
    #expect(model.layerOpacity(layer.id) == 0.4)
}

@Test @MainActor func malformedPersistedMaskParametersRenderUsingAccessorFallbacks() async throws {
    let directory = FileManager.default.temporaryDirectory.appendingPathComponent(
        "PhotonStackMalformedMaskRendering-\(UUID().uuidString)",
        isDirectory: true
    )
    defer { try? FileManager.default.removeItem(at: directory) }
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)

    let baseURL = directory.appendingPathComponent("base.png")
    let overlayURL = directory.appendingPathComponent("overlay.png")
    let maskURL = directory.appendingPathComponent("mask.png")
    try writeSolidPNG(to: baseURL, red: 1, green: 0, blue: 0, width: 48, height: 24)
    try writeSolidPNG(to: overlayURL, red: 0, green: 0, blue: 1, width: 48, height: 24)
    try writeSplitMaskPNG(to: maskURL, width: 48, height: 24)

    let mask = ProcessingArtifact(kind: .mask, name: "Split Mask", outputURLs: [maskURL])
    let base = ProcessingLayer(name: "Base", kind: .baseImage, inputURL: baseURL)
    let overlay = ProcessingLayer(
        name: "Overlay",
        kind: .adjustment,
        inputURL: overlayURL,
        maskArtifactID: mask.id,
        parameters: [
            "maskDensity": "nan",
            "maskFeatherFraction": "infinity",
        ]
    )
    var project = PhotonStackProject(name: "Malformed Mask Rendering")
    project.appendArtifact(mask)
    project.appendLayer(base)
    project.appendLayer(overlay)
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: RecordingProcessingService(),
        previewDirectory: directory.appendingPathComponent("previews", isDirectory: true)
    )

    #expect(model.layerMaskDensity(overlay.id) == 1)
    #expect(model.layerMaskFeatherRadius(overlay.id) == 0)
    await model.previewLayerStack()

    let preview = try #require(model.previewURL)
    let maskedPixel = try rgbaPixel(in: preview, x: 4, y: 12)
    let revealedPixel = try rgbaPixel(in: preview, x: 43, y: 12)
    #expect(maskedPixel.red > 240 && maskedPixel.blue < 15)
    #expect(revealedPixel.blue > 240 && revealedPixel.red < 15)
}

@Test @MainActor func maskLayerRejectsCompositionOnlyControls() {
    let bottomLayer = ProcessingLayer(
        name: "Bottom",
        kind: .baseImage,
        inputURL: URL(fileURLWithPath: "/tmp/mask-controls-bottom.png")
    )
    let maskLayer = ProcessingLayer(
        name: "Mask Resource",
        kind: .mask,
        inputURL: URL(fileURLWithPath: "/tmp/mask-resource.png"),
        isVisible: false
    )
    let topLayer = ProcessingLayer(
        name: "Top",
        kind: .adjustment,
        inputURL: URL(fileURLWithPath: "/tmp/mask-controls-top.png")
    )
    var project = PhotonStackProject(name: "Mask Composition Controls")
    project.appendLayer(bottomLayer)
    project.appendLayer(maskLayer)
    project.appendLayer(topLayer)
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: RecordingProcessingService()
    )

    model.setLayerVisibility(maskLayer.id, isVisible: true)
    model.beginLayerOpacityAdjustment(maskLayer.id)
    model.setLayerOpacity(maskLayer.id, opacity: 0.25)
    model.setLayerBlendMode(maskLayer.id, blendMode: .screen)

    #expect(model.layerIsVisible(maskLayer.id) == false)
    #expect(model.layerOpacity(maskLayer.id) == 1)
    #expect(model.layerBlendMode(maskLayer.id) == .normal)
    #expect(model.canStepBack == false)
    #expect(model.canMoveProcessingLayer(maskLayer.id, direction: .up) == false)
    #expect(model.canMoveProcessingLayer(maskLayer.id, direction: .down) == false)

    model.moveProcessingLayer(maskLayer.id, direction: .down)
    #expect(model.project.layers.map(\.id) == [bottomLayer.id, maskLayer.id, topLayer.id])

    #expect(model.canMoveProcessingLayer(bottomLayer.id, direction: .up))
    model.moveProcessingLayer(bottomLayer.id, direction: .up)
    #expect(model.project.layers.map(\.id) == [topLayer.id, maskLayer.id, bottomLayer.id])

    model.selectLayer(maskLayer)
    #expect(model.activeLayerID == maskLayer.id)
    #expect(model.canvasMode == .sourcePreview)
    #expect(model.previewURL == maskLayer.inputURL)
}

@Test @MainActor func processingLayerStackChangesAreUndoable() {
    let service = RecordingProcessingService()
    let bottomLayer = ProcessingLayer(
        name: "Base",
        kind: .baseImage,
        inputURL: URL(fileURLWithPath: "/tmp/layer-history-base.tiff")
    )
    let topLayer = ProcessingLayer(
        name: "Adjustment",
        kind: .adjustment,
        inputURL: URL(fileURLWithPath: "/tmp/layer-history-adjustment.tiff")
    )
    var project = PhotonStackProject(name: "Layer History Test")
    project.appendLayer(bottomLayer)
    project.appendLayer(topLayer)
    let model = PhotonStackWorkspaceModel(project: project, processingService: service)

    model.setLayerVisibility(bottomLayer.id, isVisible: false)
    model.stepBackPreview()
    #expect(model.layerIsVisible(bottomLayer.id))

    model.setLayerBlendMode(bottomLayer.id, blendMode: .screen)
    model.stepBackPreview()
    #expect(model.layerBlendMode(bottomLayer.id) == .normal)

    model.moveProcessingLayer(bottomLayer.id, direction: .up)
    model.stepBackPreview()
    #expect(model.project.layers.map(\.id) == [bottomLayer.id, topLayer.id])

    model.beginLayerOpacityAdjustment(bottomLayer.id)
    model.setLayerOpacity(bottomLayer.id, opacity: 0.75)
    model.setLayerOpacity(bottomLayer.id, opacity: 0.35)
    model.stepBackPreview()
    #expect(model.layerOpacity(bottomLayer.id) == 1.0)
}

@Test @MainActor func deletingProcessingLayerRewiresChildrenAndUndoRestoresLineage() {
    let service = RecordingProcessingService()
    let baseLayer = ProcessingLayer(
        name: "Base",
        kind: .baseImage,
        inputURL: URL(fileURLWithPath: "/tmp/delete-layer-base.tiff")
    )
    let middleLayer = ProcessingLayer(
        name: "Middle",
        kind: .adjustment,
        inputURL: URL(fileURLWithPath: "/tmp/delete-layer-middle.tiff"),
        parameters: ["sourceLayerID": baseLayer.id.uuidString]
    )
    let topLayer = ProcessingLayer(
        name: "Top",
        kind: .adjustment,
        inputURL: URL(fileURLWithPath: "/tmp/delete-layer-top.tiff"),
        parameters: ["sourceLayerID": middleLayer.id.uuidString]
    )
    let maskLayer = ProcessingLayer(
        name: "Mask",
        kind: .mask,
        inputURL: URL(fileURLWithPath: "/tmp/delete-layer-mask.png")
    )
    var project = PhotonStackProject(name: "Delete Layer Lineage Test")
    project.appendLayer(baseLayer)
    project.appendLayer(middleLayer)
    project.appendLayer(topLayer)
    project.appendLayer(maskLayer)
    let model = PhotonStackWorkspaceModel(project: project, processingService: service)
    model.selectLayer(middleLayer)

    model.deleteProcessingLayer(middleLayer.id)

    #expect(model.project.layers.map(\.id) == [baseLayer.id, topLayer.id, maskLayer.id])
    #expect(model.project.layers[1].parameters["sourceLayerID"] == baseLayer.id.uuidString)
    #expect(model.activeLayerID == topLayer.id)

    model.stepBackPreview()

    #expect(model.project.layers.map(\.id) == [baseLayer.id, middleLayer.id, topLayer.id, maskLayer.id])
    #expect(model.project.layers[2].parameters["sourceLayerID"] == middleLayer.id.uuidString)
    #expect(model.activeLayerID == middleLayer.id)
}

@Test @MainActor func creatingProcessingLayerCanBeUndone() {
    let service = RecordingProcessingService()
    let asset = PhotonStackAsset(
        originalURL: URL(fileURLWithPath: "/tmp/create-layer-history.nef"),
        kind: .raw
    )
    let model = PhotonStackWorkspaceModel(
        project: PhotonStackProject(name: "Create Layer History Test", assets: [asset]),
        processingService: service
    )

    model.createLayer(from: asset)
    #expect(model.project.layers.count == 1)

    model.stepBackPreview()
    #expect(model.project.layers.isEmpty)
    #expect(model.activeLayerID == nil)
}

@Test @MainActor func workspaceCreatesLayersFromAssetsAndArtifacts() {
    let service = RecordingProcessingService()
    let asset = PhotonStackAsset(
        originalURL: URL(fileURLWithPath: "/tmp/workspace-source.nef"),
        kind: .raw
    )
    let artifact = ProcessingArtifact(
        kind: .registeredSequence,
        name: "Registered Workspace",
        operationKind: .register,
        outputURLs: [URL(fileURLWithPath: "/tmp/registered-workspace.tiff")]
    )
    var project = PhotonStackProject(name: "Workspace Layers", assets: [asset])
    project.appendArtifact(artifact)
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: service
    )

    model.createLayer(from: asset)
    model.createLayer(from: artifact)

    #expect(model.project.layers.count == 2)
    #expect(model.project.layers[0].name == "workspace-source.nef")
    #expect(model.project.layers[0].inputURL == asset.originalURL)
    #expect(model.project.layers[0].parameters["source"] == "asset")
    #expect(model.project.layers[1].sourceArtifactID == artifact.id)
    #expect(model.project.layers[1].inputURL == artifact.previewURL)
    #expect(model.project.layers[1].parameters["source"] == "artifact")
    #expect(model.activeLayerID == model.project.layers[1].id)
    #expect(model.currentInputURL == artifact.previewURL)
}

@Test @MainActor func removingAssetUpdatesSelectionAndDerivedLayers() {
    let service = RecordingProcessingService()
    let removed = PhotonStackAsset(
        originalURL: URL(fileURLWithPath: "/tmp/remove-selected.nef"),
        kind: .raw
    )
    let kept = PhotonStackAsset(
        originalURL: URL(fileURLWithPath: "/tmp/keep-selected.nef"),
        kind: .raw
    )
    let model = PhotonStackWorkspaceModel(
        project: PhotonStackProject(name: "Remove Asset", assets: [removed, kept]),
        processingService: service
    )

    model.select(removed)
    model.createLayer(from: removed)
    model.selectOnlyBatchRegistrationFrame(removed)

    model.removeAsset(removed)

    #expect(model.project.assets.map(\.id) == [kept.id])
    #expect(model.selectedAssetID == kept.id)
    #expect(model.previewURL == kept.originalURL)
    #expect(model.project.layers.isEmpty)
    #expect(model.currentInputURL == kept.originalURL)
    #expect(model.isBatchRegistrationFrameSelected(removed) == false)
}

@Test @MainActor func removingAssetClearsAliasedWorkspacePreviewAndQueue() async throws {
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackWorkspaceRemoveAssetAlias-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    let removedURL = directory.appendingPathComponent("removed.png")
    let removedAlias = directory.appendingPathComponent("removed-link.png")
    let keptURL = directory.appendingPathComponent("kept.png")
    try writeSolidPNG(to: removedURL, red: 0.8, green: 0.2, blue: 0.1)
    try writeSolidPNG(to: keptURL, red: 0.1, green: 0.5, blue: 0.8)
    try FileManager.default.createSymbolicLink(at: removedAlias, withDestinationURL: removedURL)

    var project = PhotonStackProject(name: "Aliased Workspace Asset")
    project.addAssets(from: [removedURL, keptURL])
    let removed = try #require(project.assets.first)
    let kept = try #require(project.assets.last)
    project.batchQueue = [BatchQueueItem(title: "Aliased", kind: .denoise, inputURL: removedAlias)]
    project.workspaceState = ProjectWorkspaceState(
        selectedAssetID: removed.id,
        previewURL: removedAlias,
        canvasMode: .sourcePreview
    )
    let repository = ProjectRepository()
    let projectDirectory = directory.appendingPathComponent("project", isDirectory: true)
    try await repository.save(project, to: projectDirectory)
    let model = PhotonStackWorkspaceModel(
        processingService: RecordingProcessingService(requiresExistingInputFiles: true),
        projectRepository: repository
    )
    await model.loadProject(from: projectDirectory)
    let loadedRemoved = try #require(model.project.assets.first { $0.id == removed.id })
    #expect(model.previewURL == removedAlias)
    #expect(model.batchQueue.first?.inputURL == removedAlias)

    model.removeAsset(loadedRemoved)

    #expect(model.project.assets.map(\.id) == [kept.id])
    #expect(model.previewURL == keptURL)
    #expect(model.batchQueue.isEmpty)
}

@Test @MainActor func assetRemovalRequestRequiresConfirmationAndSupportsCancellation() throws {
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackAssetRemovalConfirmation-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }
    let firstURL = directory.appendingPathComponent("remove-confirm-first.png")
    let secondURL = directory.appendingPathComponent("remove-confirm-second.png")
    try Data([0x01]).write(to: firstURL)
    try Data([0x02]).write(to: secondURL)
    let first = PhotonStackAsset(
        originalURL: firstURL,
        kind: .png
    )
    let second = PhotonStackAsset(
        originalURL: secondURL,
        kind: .png
    )
    let model = PhotonStackWorkspaceModel(
        project: PhotonStackProject(name: "Confirm Asset Removal", assets: [first, second]),
        processingService: RecordingProcessingService()
    )
    model.createLayer(from: first)
    let sourceLayerID = try #require(model.project.layers.first?.id)

    model.requestAssetRemoval(first)

    #expect(model.project.assets.map(\.id) == [first.id, second.id])
    #expect(model.project.layers.map(\.id) == [sourceLayerID])
    #expect(model.pendingAssetRemovalIDs == [first.id])
    #expect(model.assetsPendingRemoval.map(\.id) == [first.id])

    model.cancelAssetRemoval()

    #expect(model.pendingAssetRemovalIDs.isEmpty)
    #expect(model.project.assets.map(\.id) == [first.id, second.id])
    #expect(model.project.layers.map(\.id) == [sourceLayerID])

    model.requestAssetRemoval([second, first, second])

    #expect(model.pendingAssetRemovalIDs == [second.id, first.id])
    let dialogSnapshot = model.assetsPendingRemoval
    model.cancelAssetRemoval()
    model.confirmAssetRemoval(dialogSnapshot)
    #expect(model.pendingAssetRemovalIDs.isEmpty)
    #expect(model.project.assets.isEmpty)
    #expect(model.project.layers.isEmpty)
    #expect(model.selectedAssetID == nil)
    #expect(model.activeLayerID == nil)
    #expect(model.previewURL == nil)
    #expect(FileManager.default.fileExists(atPath: firstURL.path))
    #expect(FileManager.default.fileExists(atPath: secondURL.path))
}

@Test @MainActor func artifactRemovalRequiresConfirmationCascadesAndPreservesFiles() throws {
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackArtifactRemovalConfirmation-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }
    let sourceURL = directory.appendingPathComponent("source.png")
    let targetURL = directory.appendingPathComponent("target.png")
    let childURL = directory.appendingPathComponent("child.png")
    try Data([0x01]).write(to: sourceURL)
    try Data([0x02]).write(to: targetURL)
    try Data([0x03]).write(to: childURL)
    let asset = PhotonStackAsset(originalURL: sourceURL, kind: .png)
    let target = ProcessingArtifact(kind: .mask, name: "Target", outputURLs: [targetURL])
    let child = ProcessingArtifact(
        kind: .editedImage,
        name: "Child",
        sourceArtifactIDs: [target.id],
        outputURLs: [childURL]
    )
    let keptLayer = ProcessingLayer(
        name: "Kept Source",
        kind: .baseImage,
        inputURL: sourceURL,
        maskArtifactID: target.id,
        parameters: ["assetID": asset.id.uuidString]
    )
    let removedLayer = ProcessingLayer(
        name: "Removed Child",
        kind: .adjustment,
        sourceArtifactID: child.id,
        inputURL: childURL
    )
    var project = PhotonStackProject(name: "Confirm Artifact Removal", assets: [asset])
    project.artifacts = [target, child]
    project.layers = [keptLayer, removedLayer]
    let model = PhotonStackWorkspaceModel(project: project, processingService: RecordingProcessingService())
    model.previewArtifact(child)

    model.requestArtifactRemoval(target)

    #expect(model.pendingArtifactRemovalID == target.id)
    #expect(model.artifactPendingRemoval?.id == target.id)
    #expect(model.project.artifacts.count == 2)
    model.cancelArtifactRemoval()
    #expect(model.pendingArtifactRemovalID == nil)
    #expect(model.project.artifacts.count == 2)

    model.requestArtifactRemoval(target)
    let dialogSnapshot = try #require(model.artifactPendingRemoval)
    model.cancelArtifactRemoval()
    model.confirmArtifactRemoval(dialogSnapshot)

    #expect(model.pendingArtifactRemovalID == nil)
    #expect(model.project.artifacts.isEmpty)
    #expect(model.project.layers.map(\.id) == [keptLayer.id])
    #expect(model.project.layers.first?.maskArtifactID == nil)
    #expect(model.previewURL == sourceURL)
    #expect(model.activeLayerID == nil)
    #expect(FileManager.default.fileExists(atPath: targetURL.path))
    #expect(FileManager.default.fileExists(atPath: childURL.path))
}

@Test @MainActor func removingTheLastAssetPreservesAnIndependentLayerComposite() async throws {
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackRemoveLastAssetComposite-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    let assetURL = directory.appendingPathComponent("asset.png")
    let layerURL = directory.appendingPathComponent("independent-layer.png")
    try writeSolidPNG(to: assetURL, red: 1, green: 0, blue: 0, width: 8, height: 4)
    try writeSolidPNG(to: layerURL, red: 0, green: 0, blue: 1, width: 8, height: 4)

    var project = PhotonStackProject(name: "Remove Last Asset Composite")
    project.addAssets(from: [assetURL])
    let asset = try #require(project.assets.first)
    let independentLayer = ProcessingLayer(name: "Independent", kind: .baseImage, inputURL: layerURL)
    project.appendLayer(independentLayer)
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: RecordingProcessingService(requiresExistingInputFiles: true),
        previewDirectory: directory.appendingPathComponent("previews", isDirectory: true)
    )

    await model.previewLayerStack()
    let compositePreview = try #require(model.previewURL)

    model.removeAsset(asset)

    #expect(model.project.assets.isEmpty)
    #expect(model.project.layers.map(\.id) == [independentLayer.id])
    #expect(model.canvasMode == .layerComposite)
    #expect(model.previewURL == compositePreview)
    #expect(model.canExportCurrentImage)
    #expect(try rgbaPixel(in: compositePreview, x: 1, y: 1).blue > 240)
}

@Test @MainActor func removingMaskArtifactRerendersTheSurvivingLayerComposite() async throws {
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackRemoveMaskComposite-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    let baseURL = directory.appendingPathComponent("base.png")
    let overlayURL = directory.appendingPathComponent("overlay.png")
    let maskURL = directory.appendingPathComponent("mask.png")
    try writeSolidPNG(to: baseURL, red: 1, green: 0, blue: 0, width: 8, height: 4)
    try writeSolidPNG(to: overlayURL, red: 0, green: 0, blue: 1, width: 8, height: 4)
    try writeSplitMaskPNG(to: maskURL, width: 8, height: 4)

    let mask = ProcessingArtifact(kind: .mask, name: "Half Mask", outputURLs: [maskURL])
    let baseLayer = ProcessingLayer(name: "Base", kind: .baseImage, inputURL: baseURL)
    let overlayLayer = ProcessingLayer(
        name: "Overlay",
        kind: .adjustment,
        inputURL: overlayURL,
        maskArtifactID: mask.id
    )
    var project = PhotonStackProject(name: "Remove Mask Composite")
    project.appendArtifact(mask)
    project.appendLayer(baseLayer)
    project.appendLayer(overlayLayer)
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: RecordingProcessingService(requiresExistingInputFiles: true),
        previewDirectory: directory.appendingPathComponent("previews", isDirectory: true)
    )

    await model.previewLayerStack()
    let maskedPreview = try #require(model.previewURL)
    #expect(try rgbaPixel(in: maskedPreview, x: 1, y: 1).red > 240)
    #expect(try rgbaPixel(in: maskedPreview, x: 6, y: 1).blue > 240)

    model.removeArtifact(mask)

    let unmaskedPreview = try #require(model.previewURL)
    #expect(model.project.layers.last?.maskArtifactID == nil)
    #expect(unmaskedPreview != maskedPreview)
    #expect(try rgbaPixel(in: unmaskedPreview, x: 1, y: 1).blue > 240)
    #expect(try rgbaPixel(in: unmaskedPreview, x: 6, y: 1).blue > 240)
    #expect(model.errorMessage == nil)
}

@Test @MainActor func destructiveLayerMutationClearsStaleCompositeWhenRemainingSourceIsMissing() async throws {
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackRemoveMaskMissingComposite-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    let baseURL = directory.appendingPathComponent("base.png")
    let overlayURL = directory.appendingPathComponent("overlay.png")
    let maskURL = directory.appendingPathComponent("mask.png")
    try writeSolidPNG(to: baseURL, red: 1, green: 0, blue: 0, width: 8, height: 4)
    try writeSolidPNG(to: overlayURL, red: 0, green: 0, blue: 1, width: 8, height: 4)
    try writeSplitMaskPNG(to: maskURL, width: 8, height: 4)

    let mask = ProcessingArtifact(kind: .mask, name: "Half Mask", outputURLs: [maskURL])
    let baseLayer = ProcessingLayer(
        name: "Base",
        kind: .baseImage,
        inputURL: baseURL,
        maskArtifactID: mask.id
    )
    let overlayLayer = ProcessingLayer(name: "Overlay", kind: .adjustment, inputURL: overlayURL)
    var project = PhotonStackProject(name: "Remove Mask Missing Composite")
    project.appendArtifact(mask)
    project.appendLayer(baseLayer)
    project.appendLayer(overlayLayer)
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: RecordingProcessingService(requiresExistingInputFiles: true),
        previewDirectory: directory.appendingPathComponent("previews", isDirectory: true)
    )

    await model.previewLayerStack()
    #expect(model.previewURL != nil)
    try FileManager.default.removeItem(at: overlayURL)

    model.removeArtifact(mask)

    #expect(model.project.layers.first?.maskArtifactID == nil)
    #expect(model.previewURL == nil)
    #expect(model.canvasMode == .layerComposite)
    #expect(model.errorMessage?.contains("Overlay") == true)
}

@Test @MainActor func removingArtifactClearsAliasedWorkspacePreviewAndQueue() async throws {
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackWorkspaceRemoveArtifactAlias-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    let sourceURL = directory.appendingPathComponent("source.png")
    let artifactURL = directory.appendingPathComponent("artifact.png")
    let artifactAlias = directory.appendingPathComponent("artifact-link.png")
    try writeSolidPNG(to: sourceURL, red: 0.2, green: 0.5, blue: 0.8)
    try writeSolidPNG(to: artifactURL, red: 0.8, green: 0.4, blue: 0.2)
    try FileManager.default.createSymbolicLink(at: artifactAlias, withDestinationURL: artifactURL)

    var project = PhotonStackProject(name: "Aliased Workspace Artifact")
    project.addAssets(from: [sourceURL])
    let asset = try #require(project.assets.first)
    let artifact = ProcessingArtifact(kind: .editedImage, name: "Result", outputURLs: [artifactURL])
    project.artifacts = [artifact]
    project.batchQueue = [BatchQueueItem(title: "Aliased", kind: .sharpen, inputURL: artifactAlias)]
    project.workspaceState = ProjectWorkspaceState(
        selectedAssetID: asset.id,
        previewURL: artifactAlias,
        canvasMode: .sourcePreview
    )
    let repository = ProjectRepository()
    let projectDirectory = directory.appendingPathComponent("project", isDirectory: true)
    try await repository.save(project, to: projectDirectory)
    let model = PhotonStackWorkspaceModel(
        processingService: RecordingProcessingService(requiresExistingInputFiles: true),
        projectRepository: repository
    )
    await model.loadProject(from: projectDirectory)
    let loadedArtifact = try #require(model.project.artifacts.first)
    #expect(model.previewURL == artifactAlias)
    #expect(model.batchQueue.first?.inputURL == artifactAlias)

    model.removeArtifact(loadedArtifact)

    #expect(model.project.artifacts.isEmpty)
    #expect(model.previewURL == sourceURL)
    #expect(model.batchQueue.isEmpty)
}

@Test @MainActor func relinkingMissingSelectedAssetRestoresPreviewQueueAndExport() async throws {
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackRelinkAsset-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    let missingURL = directory.appendingPathComponent("missing.png")
    let replacementURL = directory.appendingPathComponent("replacement.png")
    try writeSolidPNG(to: replacementURL, red: 0.2, green: 0.4, blue: 0.8, width: 16, height: 8)
    var project = PhotonStackProject(name: "Relink Missing")
    project.addAssets(from: [missingURL])
    let asset = try #require(project.assets.first)
    let service = RecordingProcessingService()
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: service,
        previewDirectory: directory.appendingPathComponent("previews", isDirectory: true)
    )
    model.enqueueCurrent(.stretch)

    #expect(model.isAssetMissing(asset))
    #expect(model.selectedAssetIsMissing)
    #expect(model.canExportCurrentImage == false)

    #expect(model.relinkAsset(asset, to: replacementURL))
    #expect(model.selectedAsset?.originalURL == replacementURL)
    #expect(model.selectedAsset?.kind == .png)
    #expect(model.previewURL == replacementURL)
    #expect(model.batchQueue.map(\.inputURL) == [replacementURL])
    #expect(model.isAssetMissing(try #require(model.selectedAsset)) == false)
    #expect(model.selectedAssetIsMissing == false)
    #expect(model.canExportCurrentImage)

    await model.exportCurrentImage(to: directory.appendingPathComponent("export.tiff"))
    let snapshot = await service.snapshot()
    #expect(snapshot.convertInput == replacementURL)
}

@Test @MainActor func derivedVisibleLayerBlocksExportUntilItsMissingSourceIsRelinked() throws {
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackRelinkDerivedLayer-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    let missingURL = directory.appendingPathComponent("missing-source.png")
    let replacementURL = directory.appendingPathComponent("replacement-source.png")
    let proxyURL = directory.appendingPathComponent("derived-preview.png")
    try writeSolidPNG(to: replacementURL, red: 0.2, green: 0.4, blue: 0.8, width: 16, height: 8)
    try writeSolidPNG(to: proxyURL, red: 0.4, green: 0.5, blue: 0.6, width: 16, height: 8)

    var project = PhotonStackProject(name: "Relink Derived Layer")
    project.addAssets(from: [missingURL])
    let asset = try #require(project.assets.first)
    let sourceLayer = ProcessingLayer(
        name: "Missing Source",
        kind: .baseImage,
        inputURL: missingURL,
        isVisible: false,
        parameters: ["assetID": asset.id.uuidString]
    )
    project.appendLayer(sourceLayer)
    project.appendLayer(
        ProcessingLayer(
            name: "Derived Preview",
            kind: .adjustment,
            inputURL: proxyURL,
            parameters: ["sourceLayerID": sourceLayer.id.uuidString]
        )
    )
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: RecordingProcessingService(),
        previewDirectory: directory.appendingPathComponent("previews", isDirectory: true)
    )

    #expect(model.canExportCurrentImage == false)
    #expect(model.relinkAsset(asset, to: replacementURL))
    #expect(model.project.layers.first { $0.id == sourceLayer.id }?.inputURL == replacementURL)
    #expect(model.canExportCurrentImage)
}

@Test @MainActor func derivedOperationLayerWithMissingOperationBlocksExportInsteadOfUsingProxy() async throws {
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackMissingLayerOperation-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    let sourceURL = directory.appendingPathComponent("source.png")
    let proxyURL = directory.appendingPathComponent("derived-preview.png")
    try writeSolidPNG(to: sourceURL, red: 0.2, green: 0.4, blue: 0.8, width: 16, height: 8)
    try writeSolidPNG(to: proxyURL, red: 0.5, green: 0.5, blue: 0.5, width: 8, height: 4)

    var project = PhotonStackProject(name: "Missing Layer Operation")
    project.addAssets(from: [sourceURL])
    let pathMatchedOperation = EditOperation(
        kind: .stretch,
        parameters: ["input": sourceURL.path, "output": proxyURL.path]
    )
    project.appendOperation(pathMatchedOperation)
    project.appendLayer(
        ProcessingLayer(
            name: "Broken derived layer",
            kind: .adjustment,
            inputURL: proxyURL,
            parameters: [
                "source": "operation",
                "operationID": UUID().uuidString,
            ]
        )
    )
    let service = RecordingProcessingService(requiresExistingInputFiles: true)
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: service,
        previewDirectory: directory.appendingPathComponent("previews", isDirectory: true)
    )

    #expect(model.canvasMode == .layerComposite)
    #expect(model.canExportCurrentImage == false)
    await model.exportCurrentImage(to: directory.appendingPathComponent("blocked.tiff"))

    #expect(await service.snapshot().convertInputs.isEmpty)
    #expect(model.latestJob == nil)
    #expect(model.errorMessage?.contains("Broken derived layer") == true)
}

@Test @MainActor func derivedOperationLayerWithMissingSourceLayerBlocksExport() async throws {
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackMissingOperationSourceLayer-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    let sourceURL = directory.appendingPathComponent("source.png")
    let proxyURL = directory.appendingPathComponent("derived-preview.png")
    try writeSolidPNG(to: sourceURL, red: 0.2, green: 0.4, blue: 0.8, width: 16, height: 8)
    try writeSolidPNG(to: proxyURL, red: 0.5, green: 0.5, blue: 0.5, width: 8, height: 4)

    var project = PhotonStackProject(name: "Missing Operation Source Layer")
    project.addAssets(from: [sourceURL])
    let operation = EditOperation(
        kind: .stretch,
        parameters: ["input": sourceURL.path, "output": proxyURL.path]
    )
    project.appendOperation(operation)
    project.appendLayer(
        ProcessingLayer(
            name: "Orphaned adjustment",
            kind: .adjustment,
            inputURL: proxyURL,
            parameters: [
                "source": "operation",
                "sourceLayerID": UUID().uuidString,
                "operationID": operation.id.uuidString,
            ]
        )
    )
    let service = RecordingProcessingService(requiresExistingInputFiles: true)
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: service,
        previewDirectory: directory.appendingPathComponent("previews", isDirectory: true)
    )

    #expect(model.canExportCurrentImage == false)
    await model.exportCurrentImage(to: directory.appendingPathComponent("blocked.tiff"))

    #expect(await service.snapshot().convertInputs.isEmpty)
    #expect(model.latestJob == nil)
    #expect(model.errorMessage?.contains("Orphaned adjustment") == true)
}

@Test @MainActor func externalOperationLayerBlocksExportWhenOriginalInputIsMissing() async throws {
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackMissingExternalOperationInput-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    let missingSource = directory.appendingPathComponent("missing-external-source.png")
    let proxyURL = directory.appendingPathComponent("external-proxy.png")
    try writeSolidPNG(to: proxyURL, red: 0.4, green: 0.5, blue: 0.6, width: 8, height: 4)
    let operation = EditOperation(
        kind: .stretch,
        parameters: ["input": missingSource.path, "output": proxyURL.path]
    )
    let layer = ProcessingLayer(
        name: "Missing external object",
        kind: .adjustment,
        inputURL: proxyURL,
        parameters: [
            "source": "operation",
            "operationID": operation.id.uuidString,
        ]
    )
    var project = PhotonStackProject(name: "Missing External Operation Input")
    project.appendOperation(operation)
    project.appendLayer(layer)
    let service = RecordingProcessingService(requiresExistingInputFiles: true)
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: service,
        previewDirectory: directory.appendingPathComponent("previews", isDirectory: true)
    )

    #expect(model.canExportCurrentImage == false)
    await model.exportCurrentImage(to: directory.appendingPathComponent("blocked.tiff"))

    #expect(model.latestJob == nil)
    #expect(await service.snapshot().convertInputs.isEmpty)
    #expect(model.errorMessage?.contains(layer.name) == true)
}

@Test @MainActor func operationLayerBlocksExportWhenHiddenDirectSourceIsMissing() async throws {
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackMissingHiddenLayerSource-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    let missingSource = directory.appendingPathComponent("missing-hidden-source.png")
    let proxyURL = directory.appendingPathComponent("derived-proxy.png")
    try writeSolidPNG(to: proxyURL, red: 0.4, green: 0.5, blue: 0.6, width: 8, height: 4)
    let sourceLayer = ProcessingLayer(
        name: "Missing hidden source",
        kind: .baseImage,
        inputURL: missingSource,
        isVisible: false
    )
    let operation = EditOperation(
        kind: .stretch,
        parameters: ["input": missingSource.path, "output": proxyURL.path]
    )
    let derivedLayer = ProcessingLayer(
        name: "Derived from hidden source",
        kind: .adjustment,
        inputURL: proxyURL,
        parameters: [
            "source": "operation",
            "sourceLayerID": sourceLayer.id.uuidString,
            "operationID": operation.id.uuidString,
        ]
    )
    var project = PhotonStackProject(name: "Missing Hidden Layer Source")
    project.appendOperation(operation)
    project.appendLayer(sourceLayer)
    project.appendLayer(derivedLayer)
    let service = RecordingProcessingService(requiresExistingInputFiles: true)
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: service,
        previewDirectory: directory.appendingPathComponent("previews", isDirectory: true)
    )

    #expect(model.canExportCurrentImage == false)
    await model.exportCurrentImage(to: directory.appendingPathComponent("blocked.tiff"))
    #expect(model.latestJob == nil)
    #expect(await service.snapshot().autoStretchInputs.isEmpty)
    #expect(model.errorMessage?.contains(derivedLayer.name) == true)
}

@Test @MainActor func operationLayerExportRequiresUniquePathSourceOrExplicitSourceLayer() async throws {
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackAmbiguousLayerExportSource-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    let firstSource = directory.appendingPathComponent("first.png")
    let secondSource = directory.appendingPathComponent("second.png")
    let sharedProxy = directory.appendingPathComponent("shared-proxy.png")
    let finalProxy = directory.appendingPathComponent("final-proxy.png")
    try writeSolidPNG(to: firstSource, red: 0.8, green: 0.1, blue: 0.1, width: 16, height: 8)
    try writeSolidPNG(to: secondSource, red: 0.1, green: 0.1, blue: 0.8, width: 16, height: 8)
    try writeSolidPNG(to: finalProxy, red: 0.5, green: 0.5, blue: 0.5, width: 8, height: 4)

    let firstOperation = EditOperation(
        kind: .stretch,
        parameters: ["input": firstSource.path, "output": sharedProxy.path]
    )
    let secondOperation = EditOperation(
        kind: .stretch,
        parameters: ["input": secondSource.path, "output": sharedProxy.path]
    )
    let finalOperation = EditOperation(
        kind: .denoise,
        parameters: ["input": sharedProxy.path, "output": finalProxy.path]
    )
    let firstLayer = ProcessingLayer(
        name: "First branch",
        kind: .adjustment,
        inputURL: sharedProxy,
        isVisible: false,
        parameters: [
            "source": "operation",
            "operationID": firstOperation.id.uuidString,
        ]
    )
    let secondLayer = ProcessingLayer(
        name: "Second branch",
        kind: .adjustment,
        inputURL: sharedProxy,
        isVisible: false,
        parameters: [
            "source": "operation",
            "operationID": secondOperation.id.uuidString,
        ]
    )
    let ambiguousLayer = ProcessingLayer(
        name: "Ambiguous final branch",
        kind: .adjustment,
        inputURL: finalProxy,
        parameters: [
            "source": "operation",
            "operationID": finalOperation.id.uuidString,
        ]
    )
    var ambiguousProject = PhotonStackProject(name: "Ambiguous Layer Export Source")
    [firstOperation, secondOperation, finalOperation].forEach {
        ambiguousProject.appendOperation($0)
    }
    [firstLayer, secondLayer, ambiguousLayer].forEach {
        ambiguousProject.appendLayer($0)
    }
    let blockedService = RecordingProcessingService(requiresExistingInputFiles: true)
    let blockedModel = PhotonStackWorkspaceModel(
        project: ambiguousProject,
        processingService: blockedService,
        previewDirectory: directory.appendingPathComponent("blocked-previews", isDirectory: true)
    )

    #expect(blockedModel.canExportCurrentImage == false)
    await blockedModel.exportCurrentImage(to: directory.appendingPathComponent("blocked.tiff"))
    #expect(await blockedService.snapshot().autoStretchInputs.isEmpty)
    #expect(await blockedService.snapshot().denoiseInput == nil)

    var explicitProject = ambiguousProject
    explicitProject.layers[2].parameters["sourceLayerID"] = firstLayer.id.uuidString
    let explicitService = RecordingProcessingService(requiresExistingInputFiles: true)
    let explicitModel = PhotonStackWorkspaceModel(
        project: explicitProject,
        processingService: explicitService,
        previewDirectory: directory.appendingPathComponent("explicit-previews", isDirectory: true)
    )

    #expect(explicitModel.canExportCurrentImage)
    await explicitModel.exportCurrentImage(to: directory.appendingPathComponent("explicit.tiff"))
    let snapshot = await explicitService.snapshot()
    #expect(snapshot.autoStretchInputs == [firstSource])
    #expect(snapshot.denoiseInput == snapshot.autoStretchOutputs.last)
    #expect(snapshot.convertInput != nil)
    #expect(explicitModel.latestJob?.status == .succeeded)
}

@Test @MainActor func relinkingAssetUpdatesAliasedPreviewAndBatchQueueReferences() throws {
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackAliasedRelinkWorkspace-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    let original = directory.appendingPathComponent("original.png")
    let alias = directory.appendingPathComponent("original-link.png")
    let replacement = directory.appendingPathComponent("replacement.png")
    try writeSolidPNG(to: original, red: 0.8, green: 0.2, blue: 0.1, width: 16, height: 8)
    try writeSolidPNG(to: replacement, red: 0.1, green: 0.4, blue: 0.8, width: 16, height: 8)
    try FileManager.default.createSymbolicLink(at: alias, withDestinationURL: original)

    var project = PhotonStackProject(name: "Aliased Relink Workspace")
    project.addAssets(from: [original])
    let asset = try #require(project.assets.first)
    project.batchQueue = [
        BatchQueueItem(
            title: "Aliased queue item",
            kind: .denoise,
            inputURL: alias,
            outputURL: alias
        ),
    ]
    project.workspaceState = ProjectWorkspaceState(
        selectedAssetID: asset.id,
        previewURL: alias,
        canvasMode: .sourcePreview
    )
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: RecordingProcessingService(requiresExistingInputFiles: true)
    )

    #expect(model.previewURL == alias)
    #expect(model.batchQueue.first?.inputURL == alias)
    #expect(model.relinkAsset(asset, to: replacement))
    #expect(model.previewURL == replacement)
    #expect(model.batchQueue.first?.inputURL == replacement)
    #expect(model.batchQueue.first?.outputURL == replacement)
    #expect(model.project.batchQueue.first?.inputURL == replacement)
    #expect(model.project.batchQueue.first?.outputURL == replacement)
}

@Test @MainActor func aliasedRawOperationSourceUsesProjectAssetDecodeForLayerExport() async throws {
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackAliasedRawLayerExport-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    let raw = directory.appendingPathComponent("source.nef")
    let rawAlias = directory.appendingPathComponent("source-link.nef")
    let proxy = directory.appendingPathComponent("stretch-proxy.png")
    try Data([0x01, 0x02, 0x03]).write(to: raw)
    try FileManager.default.createSymbolicLink(at: rawAlias, withDestinationURL: raw)
    try writeSolidPNG(to: proxy, red: 0.4, green: 0.5, blue: 0.6, width: 8, height: 4)

    var project = PhotonStackProject(name: "Aliased RAW Layer Export")
    project.addAssets(from: [raw])
    let operation = EditOperation(
        kind: .stretch,
        parameters: ["input": rawAlias.path, "output": proxy.path]
    )
    project.appendOperation(operation)
    project.appendLayer(
        ProcessingLayer(
            name: "Aliased RAW stretch",
            kind: .adjustment,
            inputURL: proxy,
            parameters: [
                "source": "operation",
                "operationID": operation.id.uuidString,
            ]
        )
    )
    let service = RecordingProcessingService(requiresExistingInputFiles: true)
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: service,
        previewDirectory: directory.appendingPathComponent("previews", isDirectory: true)
    )

    #expect(model.canExportCurrentImage)
    await model.exportCurrentImage(to: directory.appendingPathComponent("final.tiff"))

    let snapshot = await service.snapshot()
    #expect(snapshot.convertInputs.count == 2)
    #expect(snapshot.convertInputs.first == raw)
    #expect(snapshot.autoStretchInputs == [snapshot.convertOutputs[0]])
    #expect(snapshot.convertInputs.last == snapshot.autoStretchOutputs.last)
    #expect(model.latestJob?.status == .succeeded)
}

@Test @MainActor func editGraphReplayConnectsAliasedOutputsAndRestoresAliasedPreviewIdentity() async throws {
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackAliasedEditGraph-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    let source = directory.appendingPathComponent("source.png")
    let stretched = directory.appendingPathComponent("stretched.png")
    let stretchedAlias = directory.appendingPathComponent("stretched-link.png")
    let denoised = directory.appendingPathComponent("denoised.png")
    let denoisedAlias = directory.appendingPathComponent("denoised-link.png")
    try writeSolidPNG(to: source, red: 0.2, green: 0.4, blue: 0.8, width: 16, height: 8)
    try writeSolidPNG(to: stretched, red: 0.3, green: 0.5, blue: 0.7, width: 16, height: 8)
    try writeSolidPNG(to: denoised, red: 0.4, green: 0.6, blue: 0.7, width: 16, height: 8)
    try FileManager.default.createSymbolicLink(at: stretchedAlias, withDestinationURL: stretched)
    try FileManager.default.createSymbolicLink(at: denoisedAlias, withDestinationURL: denoised)

    var project = PhotonStackProject(name: "Aliased Edit Graph")
    project.addAssets(from: [source])
    let asset = try #require(project.assets.first)
    let stretch = EditOperation(
        kind: .stretch,
        parameters: ["input": source.path, "output": stretched.path]
    )
    let denoise = EditOperation(
        kind: .denoise,
        parameters: ["input": stretchedAlias.path, "output": denoised.path]
    )
    project.editGraph = EditGraph(operations: [stretch, denoise])
    project.workspaceState = ProjectWorkspaceState(
        selectedAssetID: asset.id,
        previewURL: denoisedAlias,
        canvasMode: .sourcePreview
    )
    let service = RecordingProcessingService(requiresExistingInputFiles: true)
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: service,
        previewDirectory: directory.appendingPathComponent("previews", isDirectory: true)
    )

    #expect(model.previewOperationID == denoise.id)
    model.startReplayEditGraph()
    try await waitForStartedProcessingCompletion(in: model)

    let snapshot = await service.snapshot()
    #expect(snapshot.autoStretchInputs == [source])
    #expect(snapshot.denoiseInput == snapshot.autoStretchOutputs.last)
    #expect(model.previewOperationID == denoise.id)
    #expect(model.latestJob?.status == .succeeded)
}

@Test @MainActor func stackLayerBlocksExportWhenRecordedLightIsMissing() throws {
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackMissingStackLightExport-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    let availableURL = directory.appendingPathComponent("available.png")
    let missingURL = directory.appendingPathComponent("missing.png")
    let stackPreviewURL = directory.appendingPathComponent("stack-preview.tiff")
    let editedPreviewURL = directory.appendingPathComponent("edited-preview.png")
    try writeSolidPNG(to: availableURL, red: 0.2, green: 0.4, blue: 0.8, width: 16, height: 8)
    try writeSolidPNG(to: editedPreviewURL, red: 0.5, green: 0.5, blue: 0.5, width: 16, height: 8)

    var project = PhotonStackProject(name: "Missing Stack Light Export")
    project.addAssets(from: [availableURL, missingURL])
    let available = project.assets[0]
    let missing = project.assets[1]
    let stackOperation = EditOperation(
        kind: .stack,
        parameters: [
            "calibrationLightIDs": [available.id, missing.id].map(\.uuidString).joined(separator: ","),
            "output": stackPreviewURL.path,
        ]
    )
    let stretchOperation = EditOperation(
        kind: .stretch,
        parameters: [
            "input": stackPreviewURL.path,
            "output": editedPreviewURL.path,
        ]
    )
    project.editGraph = EditGraph(operations: [stackOperation, stretchOperation])
    project.appendLayer(
        ProcessingLayer(
            name: "Edited Stack",
            kind: .adjustment,
            inputURL: editedPreviewURL,
            parameters: [
                "source": "operation",
                "operationID": stretchOperation.id.uuidString,
            ]
        )
    )
    let model = PhotonStackWorkspaceModel(project: project, processingService: RecordingProcessingService())

    #expect(model.isAssetMissing(missing))
    #expect(model.canExportCurrentImage == false)

    try writeSolidPNG(to: missingURL, red: 0.8, green: 0.4, blue: 0.2, width: 16, height: 8)
    #expect(model.canExportCurrentImage)
}

@Test @MainActor func nestedArtifactLayerBlocksExportWhenSourceAssetIsMissing() throws {
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackMissingArtifactSourceExport-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    let missingURL = directory.appendingPathComponent("missing-source.png")
    let proxyURL = directory.appendingPathComponent("artifact-preview.png")
    try writeSolidPNG(to: proxyURL, red: 0.3, green: 0.5, blue: 0.7, width: 16, height: 8)
    var project = PhotonStackProject(name: "Missing Artifact Source Export")
    project.addAssets(from: [missingURL])
    let asset = project.assets[0]
    let rawSequence = ProcessingArtifact(
        kind: .rawSequence,
        name: "RAW Sequence",
        outputURLs: [missingURL],
        frames: [
            ProcessingArtifactFrame(
                sourceURL: missingURL,
                outputURL: missingURL,
                metrics: ["assetID": asset.id.uuidString]
            ),
        ]
    )
    let stackArtifact = ProcessingArtifact(
        kind: .stackMaster,
        name: "Stack",
        sourceArtifactIDs: [rawSequence.id],
        outputURLs: [proxyURL]
    )
    let editedArtifact = ProcessingArtifact(
        kind: .editedImage,
        name: "Edited",
        sourceArtifactIDs: [stackArtifact.id],
        outputURLs: [proxyURL]
    )
    project.appendArtifact(rawSequence)
    project.appendArtifact(stackArtifact)
    project.appendArtifact(editedArtifact)
    project.appendLayer(
        ProcessingLayer(
            name: "Edited Artifact",
            kind: .baseImage,
            sourceArtifactID: editedArtifact.id,
            inputURL: proxyURL
        )
    )
    let model = PhotonStackWorkspaceModel(project: project, processingService: RecordingProcessingService())

    #expect(model.isAssetMissing(asset))
    #expect(model.canExportCurrentImage == false)
}

@Test @MainActor func unrelatedHiddenMissingLayerDoesNotBlockVisibleLayerExport() throws {
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackUnrelatedMissingLayerExport-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    let visibleURL = directory.appendingPathComponent("visible.png")
    let missingURL = directory.appendingPathComponent("unrelated-missing.png")
    try writeSolidPNG(to: visibleURL, red: 0.2, green: 0.6, blue: 0.8, width: 16, height: 8)
    var project = PhotonStackProject(name: "Unrelated Missing Layer Export")
    project.addAssets(from: [visibleURL, missingURL])
    project.appendLayer(
        ProcessingLayer(
            name: "Visible",
            kind: .baseImage,
            inputURL: visibleURL,
            parameters: ["assetID": project.assets[0].id.uuidString]
        )
    )
    project.appendLayer(
        ProcessingLayer(
            name: "Hidden Missing",
            kind: .baseImage,
            inputURL: missingURL,
            isVisible: false,
            parameters: ["assetID": project.assets[1].id.uuidString]
        )
    )
    let model = PhotonStackWorkspaceModel(project: project, processingService: RecordingProcessingService())

    #expect(model.isAssetMissing(project.assets[1]))
    #expect(model.canExportCurrentImage)
}

@Test @MainActor func missingVisibleWorkspaceLayerBlocksExportBeforeStartingAJob() async throws {
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackMissingVisibleLayerExport-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    let sourceURL = directory.appendingPathComponent("source.png")
    let missingLayerURL = directory.appendingPathComponent("missing-layer.png")
    try writeSolidPNG(to: sourceURL, red: 0.2, green: 0.4, blue: 0.8, width: 16, height: 8)
    var project = PhotonStackProject(name: "Missing Visible Workspace Layer")
    project.addAssets(from: [sourceURL])
    project.appendLayer(
        ProcessingLayer(name: "Missing external layer", kind: .baseImage, inputURL: missingLayerURL)
    )
    let service = RecordingProcessingService(requiresExistingInputFiles: true)
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: service,
        previewDirectory: directory.appendingPathComponent("previews", isDirectory: true)
    )

    #expect(model.canvasMode == .layerComposite)
    #expect(model.canExportCurrentImage == false)
    await model.exportCurrentImage(to: directory.appendingPathComponent("blocked.tiff"))
    #expect(await service.snapshot().convertInput == nil)
    #expect(model.latestJob == nil)
    #expect(model.errorMessage?.contains("Missing external layer") == true)

    try writeSolidPNG(to: missingLayerURL, red: 0.8, green: 0.3, blue: 0.1, width: 16, height: 8)
    #expect(model.canExportCurrentImage)
    await model.exportCurrentImage(to: directory.appendingPathComponent("restored.tiff"))
    #expect(await service.snapshot().convertInput == missingLayerURL)
}

@Test @MainActor func missingVisibleLayerMaskBlocksExportAndCurrentBatchQueue() async throws {
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackMissingMaskExport-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    let inputURL = directory.appendingPathComponent("input.png")
    let missingMaskURL = directory.appendingPathComponent("missing-mask.png")
    try writeSolidPNG(to: inputURL, red: 0.2, green: 0.4, blue: 0.8, width: 16, height: 8)
    let mask = ProcessingArtifact(kind: .mask, name: "Missing export mask", outputURLs: [missingMaskURL])
    let maskedLayer = ProcessingLayer(
        name: "Masked layer",
        kind: .baseImage,
        inputURL: inputURL,
        maskArtifactID: mask.id
    )
    var project = PhotonStackProject(name: "Missing Visible Mask")
    project.addAssets(from: [inputURL])
    project.appendArtifact(mask)
    project.appendLayer(maskedLayer)
    let service = RecordingProcessingService(requiresExistingInputFiles: true)
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: service,
        previewDirectory: directory.appendingPathComponent("previews", isDirectory: true)
    )
    model.selectLayer(maskedLayer)

    #expect(model.canExportCurrentImage == false)
    #expect(model.canEnqueueCurrentBatchItem == false)
    model.enqueueCurrent(.stretch)
    #expect(model.batchQueue.isEmpty)
    #expect(model.errorMessage?.contains("Masked layer") == true)
    await model.exportCurrentImage(to: directory.appendingPathComponent("blocked.tiff"))
    #expect(await service.snapshot().convertInput == nil)
    #expect(model.errorMessage?.contains("Missing export mask") == true)

    try writeSolidPNG(to: missingMaskURL, red: 1, green: 1, blue: 1, width: 16, height: 8)
    #expect(model.canExportCurrentImage)
    #expect(model.canEnqueueCurrentBatchItem)
    await model.exportCurrentImage(to: directory.appendingPathComponent("restored.tiff"))
    let exportInput = try #require(await service.snapshot().convertInput)
    #expect(exportInput.lastPathComponent.hasPrefix("export-layer-stack-"))
}

@Test @MainActor func corruptStarMaskBlocksExportInsteadOfUsingUnitScaleFallback() async throws {
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackCorruptStarMaskExport-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    let inputURL = directory.appendingPathComponent("input.png")
    let corruptMaskURL = directory.appendingPathComponent("corrupt-mask.png")
    try writeSolidPNG(to: inputURL, red: 0.2, green: 0.4, blue: 0.8, width: 16, height: 8)
    try Data([0x89, 0x50, 0x4E, 0x47]).write(to: corruptMaskURL)
    let mask = ProcessingArtifact(
        kind: .mask,
        name: "Corrupt star mask",
        operationKind: .starMask,
        outputURLs: [corruptMaskURL],
        parameters: ["radius": "2", "largeRadius": "4"]
    )
    var project = PhotonStackProject(name: "Corrupt Star Mask")
    project.addAssets(from: [inputURL])
    project.appendArtifact(mask)
    project.appendLayer(
        ProcessingLayer(
            name: "Masked image",
            kind: .baseImage,
            inputURL: inputURL,
            maskArtifactID: mask.id
        )
    )
    let service = RecordingProcessingService(requiresExistingInputFiles: true)
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: service,
        previewDirectory: directory.appendingPathComponent("previews", isDirectory: true)
    )

    #expect(model.canExportCurrentImage)
    await model.exportCurrentImage(to: directory.appendingPathComponent("blocked.tiff"))

    let snapshot = await service.snapshot()
    #expect(snapshot.starMaskInputs.isEmpty)
    #expect(snapshot.convertInputs.isEmpty)
    #expect(model.latestJob?.status == .failed)
    #expect(model.errorMessage?.contains("Masked image") == true)
}

@Test @MainActor func layerCompositeWithoutActiveLayerDoesNotProcessTheSelectedAsset() throws {
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackNoActiveLayerInput-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    let selectedURL = directory.appendingPathComponent("selected.png")
    let layerURL = directory.appendingPathComponent("layer.png")
    try writeSolidPNG(to: selectedURL, red: 1, green: 0, blue: 0, width: 16, height: 8)
    try writeSolidPNG(to: layerURL, red: 0, green: 0, blue: 1, width: 16, height: 8)
    let layer = ProcessingLayer(name: "Visible layer", kind: .baseImage, inputURL: layerURL)
    var project = PhotonStackProject(name: "No Active Layer Input")
    project.addAssets(from: [selectedURL])
    project.appendLayer(layer)
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: RecordingProcessingService(requiresExistingInputFiles: true),
        previewDirectory: directory.appendingPathComponent("previews", isDirectory: true)
    )

    #expect(model.canvasMode == .layerComposite)
    #expect(model.activeLayerID == nil)
    #expect(model.currentInputURL == nil)
    #expect(model.canProcessCurrentInput == false)
    #expect(model.canEnqueueCurrentBatchItem == false)
    #expect(model.canExportCurrentImage)

    model.enqueueCurrent(.stretch)
    #expect(model.batchQueue.isEmpty)
    #expect(model.errorMessage == model.localized(.missingWorkspaceSourceError))

    model.selectLayer(layer)
    #expect(model.currentInputURL == layerURL)
    #expect(model.canProcessCurrentInput)
    #expect(model.canEnqueueCurrentBatchItem)
}

@Test @MainActor func selectedMaskLayerCannotEnterImageProcessingOrCurrentBatchQueue() async throws {
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackMaskInputGate-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    let imageURL = directory.appendingPathComponent("image.png")
    let maskURL = directory.appendingPathComponent("mask.png")
    try writeSolidPNG(to: imageURL, red: 0.2, green: 0.4, blue: 0.8, width: 16, height: 8)
    try writeSolidPNG(to: maskURL, red: 1, green: 1, blue: 1, width: 16, height: 8)
    let maskArtifact = ProcessingArtifact(kind: .mask, name: "Mask", outputURLs: [maskURL])
    let maskLayer = ProcessingLayer(
        name: "Mask Resource",
        kind: .mask,
        sourceArtifactID: maskArtifact.id,
        inputURL: maskURL
    )
    var project = PhotonStackProject(name: "Mask Input Gate")
    project.addAssets(from: [imageURL])
    project.appendArtifact(maskArtifact)
    project.appendLayer(maskLayer)
    let service = RecordingProcessingService(requiresExistingInputFiles: true)
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: service,
        previewDirectory: directory.appendingPathComponent("previews", isDirectory: true)
    )
    model.selectLayer(maskLayer)

    #expect(model.canvasMode == .sourcePreview)
    #expect(model.activeLayerID == maskLayer.id)
    #expect(model.currentInputURL == maskURL)
    #expect(model.canProcessCurrentInput == false)
    #expect(model.canEnqueueCurrentBatchItem == false)
    #expect(model.canEnqueueAllAssets)
    #expect(model.canExportCurrentImage)

    model.enqueueCurrent(.stretch)
    #expect(model.batchQueue.isEmpty)
    #expect(model.errorMessage == model.localized(.imageProcessingRequiresImageLayer))

    let layerCount = model.project.layers.count
    await model.autoStretchPreview()
    #expect(await service.snapshot().autoStretchCalls == 0)
    #expect(model.latestJob == nil)
    #expect(model.project.editGraph.operations.isEmpty)
    #expect(model.project.layers.count == layerCount)

    model.enqueueAllAssets(.stretch)
    #expect(model.batchQueue.count == 1)
    #expect(model.batchQueue[0].inputURL == imageURL)
}

@Test @MainActor func emptyLayerStackExportUsesTheSameAvailableCanvasSourceAsPreview() async throws {
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackEmptyLayerSource-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    let missingURL = directory.appendingPathComponent("missing-first.png")
    let availableURL = directory.appendingPathComponent("available-second.png")
    try writeSolidPNG(to: availableURL, red: 0.1, green: 0.6, blue: 0.9, width: 12, height: 6)
    var project = PhotonStackProject(name: "Empty Layer Canvas Source")
    project.addAssets(from: [missingURL, availableURL])
    project.appendLayer(
        ProcessingLayer(
            name: "Missing first",
            kind: .baseImage,
            inputURL: missingURL,
            isVisible: false,
            parameters: ["assetID": project.assets[0].id.uuidString]
        )
    )
    project.appendLayer(
        ProcessingLayer(
            name: "Available second",
            kind: .baseImage,
            inputURL: availableURL,
            isVisible: false,
            parameters: ["assetID": project.assets[1].id.uuidString]
        )
    )
    let service = RecordingProcessingService(requiresExistingInputFiles: true)
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: service,
        previewDirectory: directory.appendingPathComponent("previews", isDirectory: true)
    )

    #expect(model.canvasMode == .layerComposite)
    #expect(model.canExportCurrentImage)
    await model.exportCurrentImage(to: directory.appendingPathComponent("empty.tiff"))

    let exportInput = try #require(await service.snapshot().convertInput)
    #expect(exportInput.lastPathComponent.hasPrefix("export-layer-stack-empty-"))
    let source = try #require(CGImageSourceCreateWithURL(exportInput as CFURL, nil))
    let image = try #require(CGImageSourceCreateImageAtIndex(source, 0, nil))
    #expect(image.width == 12)
    #expect(image.height == 6)
    #expect(try rgbaPixel(in: exportInput, x: 4, y: 2).alpha == 0)
}

@Test @MainActor func missingInputsBlockStackRegistrationAndBatchQueueUntilRestored() async throws {
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackMissingWorkflowInputs-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    let availableURL = directory.appendingPathComponent("available.png")
    let missingURL = directory.appendingPathComponent("missing.png")
    try writeSolidPNG(to: availableURL, red: 0.2, green: 0.4, blue: 0.8, width: 16, height: 8)
    var project = PhotonStackProject(name: "Missing Workflow Inputs")
    project.addAssets(from: [availableURL, missingURL])
    let available = project.assets[0]
    let missing = project.assets[1]
    project.batchQueue = [
        BatchQueueItem(title: "Missing", kind: .stretch, inputURL: missingURL),
    ]
    let service = RecordingProcessingService(requiresExistingInputFiles: true)
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: service,
        previewDirectory: directory.appendingPathComponent("previews", isDirectory: true)
    )

    #expect(model.canRunStackWorkflow == false)
    #expect(model.canRegister(reference: available, moving: missing) == false)
    #expect(model.canRegisterBatch(reference: available) == false)
    #expect(model.canEnqueueAllAssets == false)
    #expect(model.batchQueueInputsAvailable == false)

    await model.runStackWorkflow()
    await model.registerBatch(reference: available, alignment: .distortion)
    model.enqueueAllAssets(.denoise)
    model.startBatchQueue()

    let snapshot = await service.snapshot()
    #expect(snapshot.stackInputs.isEmpty)
    #expect(snapshot.registerBatchCalls == 0)
    #expect(model.batchQueue.count == 1)
    #expect(model.isBatchRunning == false)
    #expect(model.errorMessage?.contains(missing.displayName) == true)

    try writeSolidPNG(to: missingURL, red: 0.8, green: 0.4, blue: 0.2, width: 16, height: 8)
    #expect(model.canRunStackWorkflow)
    #expect(model.canRegister(reference: available, moving: missing))
    #expect(model.canRegisterBatch(reference: available))
    #expect(model.canEnqueueAllAssets)
    #expect(model.batchQueueInputsAvailable)
}

@Test @MainActor func missingMosaicPanelBlocksPreviewAndFullMosaic() async throws {
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackMissingMosaicInputs-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    let availableURL = directory.appendingPathComponent("available.png")
    let missingURL = directory.appendingPathComponent("missing.png")
    try writeSolidPNG(to: availableURL, red: 0.2, green: 0.4, blue: 0.8, width: 16, height: 8)
    var project = PhotonStackProject(name: "Missing Mosaic Inputs")
    project.addAssets(from: [availableURL, missingURL], role: .mosaic)
    let service = RecordingProcessingService(requiresExistingInputFiles: true)
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: service,
        previewDirectory: directory.appendingPathComponent("previews", isDirectory: true)
    )

    #expect(model.canRunMosaic == false)
    await model.runMosaicPreview()
    await model.runMosaic()
    #expect(await service.snapshot().mosaicInputGroups.isEmpty)
    #expect(model.errorMessage?.contains("missing.png") == true)

    try writeSolidPNG(to: missingURL, red: 0.8, green: 0.4, blue: 0.2, width: 16, height: 8)
    #expect(model.canRunMosaic)
}

@Test @MainActor func missingSequenceFrameBlocksDrizzleAndTimelapseCleanup() async throws {
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackMissingSequenceInputs-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    let availableURL = directory.appendingPathComponent("available.png")
    let missingURL = directory.appendingPathComponent("missing.png")
    try writeSolidPNG(to: availableURL, red: 0.2, green: 0.4, blue: 0.8, width: 16, height: 8)
    var project = PhotonStackProject(name: "Missing Sequence Inputs")
    project.addAssets(from: [availableURL, missingURL])
    let service = RecordingProcessingService(requiresExistingInputFiles: true)
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: service,
        previewDirectory: directory.appendingPathComponent("previews", isDirectory: true)
    )

    #expect(model.canRunDrizzle == false)
    #expect(model.canCleanTimelapseArtifactSequence == false)

    await model.runDrizzle(scale: 2, pixfrac: 0.8, alignment: .distortion)
    await model.cleanTimelapseArtifactSequence()

    let snapshot = await service.snapshot()
    #expect(snapshot.drizzleInputGroups.isEmpty)
    #expect(snapshot.cleanSequenceInputs.isEmpty)
    #expect(model.errorMessage?.contains("missing.png") == true)

    try writeSolidPNG(to: missingURL, red: 0.8, green: 0.4, blue: 0.2, width: 16, height: 8)
    #expect(model.canRunDrizzle)
    #expect(model.canCleanTimelapseArtifactSequence)
}

@Test @MainActor func missingCurrentInputBlocksSingleImageToolsAndMeteorRestore() async throws {
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackMissingCurrentInput-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    let missingURL = directory.appendingPathComponent("missing.png")
    var project = PhotonStackProject(name: "Missing Current Input")
    project.addAssets(from: [missingURL])
    let service = RecordingProcessingService(requiresExistingInputFiles: true)
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: service,
        previewDirectory: directory.appendingPathComponent("previews", isDirectory: true)
    )

    #expect(model.canProcessCurrentInput == false)
    #expect(model.canRestoreMeteorsFromSelectedAsset == false)

    await model.autoStretchPreview()
    await model.detectArtifactTrails()
    await model.restoreMeteorsFromSelectedAsset()

    let snapshot = await service.snapshot()
    #expect(snapshot.autoStretchCalls == 0)
    #expect(snapshot.artifactDetectCalls == 0)
    #expect(snapshot.restoreMeteorCalls == 0)
    #expect(model.errorMessage?.contains("missing.png") == true)

    try writeSolidPNG(to: missingURL, red: 0.2, green: 0.6, blue: 0.8, width: 16, height: 8)
    #expect(model.canProcessCurrentInput)
    #expect(model.canRestoreMeteorsFromSelectedAsset)
}

@Test @MainActor func missingOriginalCannotResetAnAvailableWorkspacePreview() throws {
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackMissingOriginalReset-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    let missingOriginal = directory.appendingPathComponent("missing-original.nef")
    let availablePreview = directory.appendingPathComponent("available-preview.png")
    try writeSolidPNG(to: availablePreview, red: 0.2, green: 0.6, blue: 0.8, width: 16, height: 8)
    let layer = ProcessingLayer(name: "Available Preview", kind: .baseImage, inputURL: availablePreview)
    var project = PhotonStackProject(name: "Missing Original Reset")
    project.addAssets(from: [missingOriginal])
    project.appendLayer(layer)
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: RecordingProcessingService(requiresExistingInputFiles: true),
        previewDirectory: directory.appendingPathComponent("previews", isDirectory: true)
    )
    model.selectLayer(layer)
    let previewBeforeReset = try #require(model.previewURL)

    #expect(model.canResetPreviewToOriginal == false)
    #expect(FileManager.default.fileExists(atPath: previewBeforeReset.path))

    model.resetPreviewToOriginal()

    #expect(model.previewURL == previewBeforeReset)
    #expect(model.activeLayerID == layer.id)
    #expect(model.layerIsVisible(layer.id))
    #expect(model.errorMessage?.contains("missing-original.nef") == true)
}

@Test @MainActor func resetToOriginalDoesNotReactivateACompositedSourceLayer() throws {
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackResetCompositedSource-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    let inputURL = directory.appendingPathComponent("input.png")
    let maskURL = directory.appendingPathComponent("mask.png")
    try writeSolidPNG(to: inputURL, red: 0.2, green: 0.6, blue: 0.8, width: 16, height: 8)
    try writeSolidPNG(to: maskURL, red: 1, green: 1, blue: 1, width: 16, height: 8)
    let mask = ProcessingArtifact(kind: .mask, name: "Mask", outputURLs: [maskURL])
    let layer = ProcessingLayer(
        name: "Composited Source",
        kind: .baseImage,
        inputURL: inputURL,
        maskArtifactID: mask.id,
        blendMode: .screen,
        opacity: 0.5
    )
    var project = PhotonStackProject(name: "Reset Composited Source")
    project.addAssets(from: [inputURL])
    project.appendArtifact(mask)
    project.appendLayer(layer)
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: RecordingProcessingService(requiresExistingInputFiles: true),
        previewDirectory: directory.appendingPathComponent("previews", isDirectory: true)
    )
    model.selectLayer(layer)

    model.resetPreviewToOriginal()

    #expect(model.previewURL == inputURL)
    #expect(model.activeLayerID == nil)
    #expect(model.layerIsVisible(layer.id) == false)
    #expect(model.canProcessCurrentInput)
}

@Test @MainActor func missingArtifactPreviewCannotBePreviewedOrPromotedToLayer() throws {
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackMissingArtifactPreview-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    let missingURL = directory.appendingPathComponent("missing-artifact.png")
    let artifact = ProcessingArtifact(
        kind: .editedImage,
        name: "Missing Artifact",
        operationKind: .stretch,
        outputURLs: [missingURL]
    )
    var project = PhotonStackProject(name: "Missing Artifact Preview")
    project.appendArtifact(artifact)
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: RecordingProcessingService(requiresExistingInputFiles: true),
        previewDirectory: directory.appendingPathComponent("previews", isDirectory: true)
    )

    #expect(model.isArtifactPreviewAvailable(artifact) == false)
    #expect(model.hasAvailableLayerSource == false)

    model.previewArtifact(artifact)
    model.createLayer(from: artifact)

    #expect(model.previewURL == nil)
    #expect(model.project.layers.isEmpty)
    #expect(model.errorMessage?.contains(artifact.name) == true)

    try writeSolidPNG(to: missingURL, red: 0.2, green: 0.6, blue: 0.8, width: 16, height: 8)
    #expect(model.isArtifactPreviewAvailable(artifact))
    #expect(model.hasAvailableLayerSource)
}

@Test @MainActor func missingLayerInputCannotBeSelectedShownOrComposited() async throws {
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackMissingLayerInput-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    let missingURL = directory.appendingPathComponent("missing-layer.png")
    let layer = ProcessingLayer(
        name: "Missing Layer",
        kind: .baseImage,
        inputURL: missingURL,
        isVisible: false
    )
    var project = PhotonStackProject(name: "Missing Layer Input")
    project.appendLayer(layer)
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: RecordingProcessingService(requiresExistingInputFiles: true),
        previewDirectory: directory.appendingPathComponent("previews", isDirectory: true)
    )

    #expect(model.isLayerInputAvailable(layer) == false)
    #expect(model.canPreviewVisibleLayerStack == false)

    model.selectLayer(layer)
    model.setLayerVisibility(layer.id, isVisible: true)
    await model.previewLayerStack()

    #expect(model.activeLayerID == nil)
    #expect(model.layerIsVisible(layer.id) == false)
    #expect(model.previewURL == nil)
    #expect(model.errorMessage?.contains(model.localized(.missingWorkspaceSourceError)) == true)

    try writeSolidPNG(to: missingURL, red: 0.8, green: 0.4, blue: 0.2, width: 16, height: 8)
    model.setLayerVisibility(layer.id, isVisible: true)
    #expect(model.isLayerInputAvailable(layer))
    #expect(model.canPreviewVisibleLayerStack)
}

@Test @MainActor func visibleLayerWithoutInputCannotBeSilentlySkipped() async throws {
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackNilLayerInput-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    let validURL = directory.appendingPathComponent("valid.png")
    try writeSolidPNG(to: validURL, red: 0.2, green: 0.6, blue: 0.8, width: 16, height: 8)
    let validLayer = ProcessingLayer(name: "Valid", kind: .baseImage, inputURL: validURL)
    let missingLayer = ProcessingLayer(name: "No Input", kind: .adjustment, inputURL: nil)
    var project = PhotonStackProject(name: "Nil Layer Input")
    project.appendLayer(validLayer)
    project.appendLayer(missingLayer)
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: RecordingProcessingService(requiresExistingInputFiles: true),
        previewDirectory: directory.appendingPathComponent("previews", isDirectory: true)
    )

    #expect(model.hasVisiblePreviewLayers)
    #expect(model.canPreviewVisibleLayerStack == false)

    await model.previewLayerStack()

    #expect(model.previewURL == nil)
    #expect(model.latestJob == nil)
    #expect(model.errorMessage?.contains(model.localized(.missingWorkspaceSourceError)) == true)

    model.setLayerVisibility(missingLayer.id, isVisible: false)
    #expect(model.canPreviewVisibleLayerStack)
    #expect(model.previewURL != nil)
}

@Test @MainActor func layerCompositeMutationsRollBackWhenAnotherVisibleLayerIsMissing() throws {
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackLayerMutationRollback-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    let availableURL = directory.appendingPathComponent("available.png")
    let extraURL = directory.appendingPathComponent("extra.png")
    let maskURL = directory.appendingPathComponent("mask.png")
    let missingURL = directory.appendingPathComponent("missing.png")
    try writeSolidPNG(to: availableURL, red: 0.8, green: 0.2, blue: 0.1, width: 16, height: 8)
    try writeSolidPNG(to: extraURL, red: 0.1, green: 0.2, blue: 0.8, width: 16, height: 8)
    try writeSolidPNG(to: maskURL, red: 1, green: 1, blue: 1, width: 16, height: 8)

    let availableLayer = ProcessingLayer(name: "Available", kind: .baseImage, inputURL: availableURL)
    let missingLayer = ProcessingLayer(name: "Missing", kind: .baseImage, inputURL: missingURL)
    let mask = ProcessingArtifact(kind: .mask, name: "Mask", outputURLs: [maskURL])
    var project = PhotonStackProject(name: "Layer Mutation Rollback")
    project.addAssets(from: [availableURL, extraURL])
    project.appendArtifact(mask)
    project.appendLayer(availableLayer)
    project.appendLayer(missingLayer)
    let extraAsset = try #require(project.assets.last)
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: RecordingProcessingService(requiresExistingInputFiles: true),
        previewDirectory: directory.appendingPathComponent("previews", isDirectory: true)
    )
    let projectSnapshot = model.project
    let previewSnapshot = model.previewURL

    model.selectLayer(availableLayer)
    #expect(model.activeLayerID == availableLayer.id)
    let hasUnsavedChangesSnapshot = model.hasUnsavedChanges
    model.setLayerOpacity(availableLayer.id, opacity: 0.4)
    model.setLayerBlendMode(availableLayer.id, blendMode: .screen)
    model.moveProcessingLayer(availableLayer.id, direction: .up)
    model.setLayerMask(availableLayer.id, artifactID: mask.id)
    model.createLayer(from: extraAsset)
    model.deleteProcessingLayer(availableLayer.id)

    #expect(model.project == projectSnapshot)
    #expect(model.previewURL == previewSnapshot)
    #expect(model.activeLayerID == availableLayer.id)
    #expect(model.canStepBack == false)
    #expect(model.hasUnsavedChanges == hasUnsavedChangesSnapshot)
    #expect(model.errorMessage?.contains(model.localized(.missingWorkspaceSourceError)) == true)

    model.setLayerVisibility(missingLayer.id, isVisible: false)

    #expect(model.layerIsVisible(missingLayer.id) == false)
    #expect(model.previewURL?.lastPathComponent.hasPrefix("cache-layer-stack-live-") == true)
    #expect(model.errorMessage == nil)
    #expect(model.canStepBack)
    #expect(model.hasUnsavedChanges)
}

@Test @MainActor func layerAdjustmentSessionsOnlyCommitUndoHistoryAfterSuccessfulChanges() throws {
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackLayerAdjustmentSession-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    let inputURL = directory.appendingPathComponent("input.png")
    let maskURL = directory.appendingPathComponent("mask.png")
    let missingURL = directory.appendingPathComponent("missing.png")
    try writeSolidPNG(to: inputURL, red: 0.8, green: 0.2, blue: 0.1, width: 16, height: 8)
    try writeSolidPNG(to: maskURL, red: 1, green: 1, blue: 1, width: 16, height: 8)

    let mask = ProcessingArtifact(kind: .mask, name: "Mask", outputURLs: [maskURL])
    let layer = ProcessingLayer(
        name: "Adjustable",
        kind: .baseImage,
        inputURL: inputURL,
        maskArtifactID: mask.id
    )
    let missingLayer = ProcessingLayer(name: "Missing", kind: .adjustment, inputURL: missingURL)
    var blockedProject = PhotonStackProject(name: "Blocked Layer Adjustment")
    blockedProject.appendArtifact(mask)
    blockedProject.appendLayer(layer)
    blockedProject.appendLayer(missingLayer)
    let blockedModel = PhotonStackWorkspaceModel(
        project: blockedProject,
        processingService: RecordingProcessingService(requiresExistingInputFiles: true),
        previewDirectory: directory.appendingPathComponent("blocked-previews", isDirectory: true)
    )

    blockedModel.beginLayerOpacityAdjustment(layer.id)
    #expect(blockedModel.canStepBack == false)
    blockedModel.endLayerOpacityAdjustment(layer.id)
    #expect(blockedModel.canStepBack == false)

    blockedModel.beginLayerOpacityAdjustment(layer.id)
    blockedModel.setLayerOpacity(layer.id, opacity: 0.4)
    blockedModel.endLayerOpacityAdjustment(layer.id)
    #expect(blockedModel.layerOpacity(layer.id) == 1)
    #expect(blockedModel.canStepBack == false)

    blockedModel.beginLayerMaskAdjustment(layer.id)
    blockedModel.setLayerMaskDensity(layer.id, density: 0.4)
    blockedModel.endLayerMaskAdjustment(layer.id)
    #expect(blockedModel.layerMaskDensity(layer.id) == 1)
    #expect(blockedModel.canStepBack == false)

    var workingProject = PhotonStackProject(name: "Working Layer Adjustment")
    workingProject.appendArtifact(mask)
    workingProject.appendLayer(layer)
    let workingModel = PhotonStackWorkspaceModel(
        project: workingProject,
        processingService: RecordingProcessingService(requiresExistingInputFiles: true),
        previewDirectory: directory.appendingPathComponent("working-previews", isDirectory: true)
    )

    workingModel.beginLayerOpacityAdjustment(layer.id)
    workingModel.setLayerOpacity(layer.id, opacity: 0.8)
    workingModel.setLayerOpacity(layer.id, opacity: 0.3)
    workingModel.endLayerOpacityAdjustment(layer.id)
    #expect(workingModel.layerOpacity(layer.id) == 0.3)
    #expect(workingModel.canStepBack)
    workingModel.stepBackPreview()
    #expect(workingModel.layerOpacity(layer.id) == 1)
    #expect(workingModel.canStepBack == false)

    workingModel.beginLayerMaskAdjustment(layer.id)
    workingModel.setLayerMaskDensity(layer.id, density: 0.7)
    workingModel.setLayerMaskDensity(layer.id, density: 0.2)
    workingModel.endLayerMaskAdjustment(layer.id)
    #expect(workingModel.layerMaskDensity(layer.id) == 0.2)
    #expect(workingModel.canStepBack)
    workingModel.stepBackPreview()
    #expect(workingModel.layerMaskDensity(layer.id) == 1)
    #expect(workingModel.canStepBack == false)
}

@Test @MainActor func missingLayerMaskCanBeDetachedButNotAdjustedOrReshown() throws {
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackMissingLayerMask-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    let inputURL = directory.appendingPathComponent("input.png")
    let missingMaskURL = directory.appendingPathComponent("missing-mask.png")
    try writeSolidPNG(to: inputURL, red: 0.8, green: 0.4, blue: 0.2, width: 16, height: 8)
    let mask = ProcessingArtifact(kind: .mask, name: "Missing Mask", outputURLs: [missingMaskURL])
    let layer = ProcessingLayer(
        name: "Masked Layer",
        kind: .baseImage,
        inputURL: inputURL,
        maskArtifactID: mask.id
    )
    var project = PhotonStackProject(name: "Missing Layer Mask")
    project.appendArtifact(mask)
    project.appendLayer(layer)
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: RecordingProcessingService(requiresExistingInputFiles: true),
        previewDirectory: directory.appendingPathComponent("previews", isDirectory: true)
    )

    #expect(model.layerMaskArtifact(layer.id)?.id == mask.id)
    #expect(model.availableMaskArtifacts.isEmpty)
    #expect(model.isLayerMaskInputAvailable(layer) == false)
    #expect(model.canPreviewVisibleLayerStack == false)

    model.setLayerMaskInverted(layer.id, inverted: true)
    model.setLayerMaskDensity(layer.id, density: 0.4)
    model.setLayerOpacity(layer.id, opacity: 0.4)
    #expect(model.layerMaskIsInverted(layer.id) == false)
    #expect(model.layerMaskDensity(layer.id) == 1)
    #expect(model.layerOpacity(layer.id) == 1)

    model.setLayerVisibility(layer.id, isVisible: false)
    model.setLayerVisibility(layer.id, isVisible: true)
    #expect(model.layerIsVisible(layer.id) == false)

    model.setLayerMask(layer.id, artifactID: nil)
    #expect(model.layerMaskArtifact(layer.id) == nil)
    model.setLayerVisibility(layer.id, isVisible: true)
    #expect(model.layerIsVisible(layer.id))
    #expect(model.canPreviewVisibleLayerStack)
}

@Test @MainActor func missingActiveLayerMaskBlocksPixelProcessingUntilDetached() async throws {
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackMissingActiveMask-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    let inputURL = directory.appendingPathComponent("input.png")
    let missingMaskURL = directory.appendingPathComponent("missing-mask.png")
    try writeSolidPNG(to: inputURL, red: 0.2, green: 0.6, blue: 0.8, width: 16, height: 8)
    let mask = ProcessingArtifact(kind: .mask, name: "Missing Mask", outputURLs: [missingMaskURL])
    let layer = ProcessingLayer(
        name: "Broken Mask Layer",
        kind: .baseImage,
        inputURL: inputURL,
        maskArtifactID: mask.id
    )
    var project = PhotonStackProject(name: "Missing Active Mask")
    project.appendArtifact(mask)
    project.appendLayer(layer)
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: RecordingProcessingService(requiresExistingInputFiles: true),
        previewDirectory: directory.appendingPathComponent("previews", isDirectory: true)
    )
    model.selectLayer(layer)

    #expect(model.canProcessCurrentInput == false)
    await model.autoStretchPreview()
    #expect(model.latestJob == nil)
    #expect(model.project.editGraph.operations.isEmpty)
    #expect(model.errorMessage?.contains("Broken Mask Layer") == true)

    model.setLayerMask(layer.id, artifactID: nil)
    #expect(model.canProcessCurrentInput)
}

@Test @MainActor func loadingProjectFallsBackFromMissingActiveLayerAndPreview() async throws {
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackMissingWorkspaceRestore-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    let validURL = directory.appendingPathComponent("valid.png")
    let missingLayerURL = directory.appendingPathComponent("missing-layer.png")
    let missingPreviewURL = directory.appendingPathComponent("missing-preview.png")
    try writeSolidPNG(to: validURL, red: 0.2, green: 0.6, blue: 0.8, width: 16, height: 8)
    var project = PhotonStackProject(name: "Missing Workspace Restore")
    project.addAssets(from: [validURL])
    let validLayer = ProcessingLayer(name: "Valid", kind: .baseImage, inputURL: validURL)
    let missingLayer = ProcessingLayer(name: "Missing", kind: .adjustment, inputURL: missingLayerURL)
    project.appendLayer(validLayer)
    project.appendLayer(missingLayer)
    project.workspaceState = ProjectWorkspaceState(
        selectedAssetID: project.assets.first?.id,
        activeLayerID: missingLayer.id,
        previewURL: missingPreviewURL,
        editGraphNeedsReplay: false
    )

    let repository = ProjectRepository()
    let projectDirectory = directory.appendingPathComponent("project", isDirectory: true)
    try await repository.save(project, to: projectDirectory)
    let model = PhotonStackWorkspaceModel(
        processingService: RecordingProcessingService(requiresExistingInputFiles: true),
        projectRepository: repository,
        previewDirectory: directory.appendingPathComponent("previews", isDirectory: true)
    )

    await model.loadProject(from: projectDirectory)

    #expect(model.activeLayerID == nil)
    #expect(model.previewURL == validURL)
    #expect(model.canProcessCurrentInput)
    #expect(model.editGraphNeedsReplay == false)
}

@Test @MainActor func removingAssetCascadesLayersQueueAndRawSequenceReferences() async throws {
    let service = RecordingProcessingService()
    let removed = PhotonStackAsset(
        originalURL: URL(fileURLWithPath: "/tmp/remove-derived.nef"),
        kind: .raw
    )
    let kept = PhotonStackAsset(
        originalURL: URL(fileURLWithPath: "/tmp/keep-derived.nef"),
        kind: .raw
    )
    let rawSequence = ProcessingArtifact(
        kind: .rawSequence,
        name: "RAW Sequence",
        outputURLs: [removed.originalURL, kept.originalURL],
        frames: [
            ProcessingArtifactFrame(
                sourceURL: removed.originalURL,
                outputURL: removed.originalURL,
                metrics: ["assetID": removed.id.uuidString]
            ),
            ProcessingArtifactFrame(
                sourceURL: kept.originalURL,
                outputURL: kept.originalURL,
                metrics: ["assetID": kept.id.uuidString]
            ),
        ]
    )
    var project = PhotonStackProject(name: "Cascade Asset Removal", assets: [removed, kept])
    project.appendArtifact(rawSequence)
    let model = PhotonStackWorkspaceModel(project: project, processingService: service)

    model.select(removed)
    model.enqueueCurrent(.denoise)
    await model.autoStretchPreview()
    #expect(model.project.layers.count == 2)

    model.select(kept)
    model.enqueueCurrent(.sharpen)
    model.select(removed)
    model.removeAsset(removed)

    #expect(model.project.assets.map(\.id) == [kept.id])
    #expect(model.project.layers.isEmpty)
    #expect(model.batchQueue.map(\.inputURL) == [kept.originalURL])
    let updatedSequence = try #require(model.project.artifacts.first { $0.kind == .rawSequence })
    #expect(updatedSequence.frameCount == 1)
    #expect(updatedSequence.frames.first?.metrics["assetID"] == kept.id.uuidString)
    #expect(updatedSequence.outputURLs == [kept.originalURL])
    #expect(model.selectedAssetID == kept.id)
    #expect(model.previewURL == kept.originalURL)
}

@Test @MainActor func removingOnlyRawDecodeOwnerPreservesProjectRawSettingsForRemainingAsset() throws {
    let removed = PhotonStackAsset(
        originalURL: URL(fileURLWithPath: "/tmp/remove-raw-owner.nef"),
        kind: .raw
    )
    let kept = PhotonStackAsset(
        originalURL: URL(fileURLWithPath: "/tmp/keep-project-raw-settings.nef"),
        kind: .raw
    )
    var project = PhotonStackProject(name: "Preserve RAW Settings", assets: [removed, kept])
    project.editGraph = EditGraph(operations: [
        EditOperation(
            kind: .rawDecode,
            parameters: [
                "assetID": removed.id.uuidString,
                "rawWhiteBalance": RawWhiteBalanceMode.auto.rawValue,
                "rawExposureBias": "1.25",
                "rawBlackLevel": RawBlackLevelMode.auto.rawValue,
                "rawDemosaic": RawDemosaicQuality.high.rawValue,
                "rawLinear": "false",
                "width": "1200",
            ]
        ),
    ])
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: RecordingProcessingService()
    )

    model.removeAsset(removed)

    #expect(model.selectedAssetID == kept.id)
    let rawOperation = try #require(model.project.editGraph.operations.first { $0.kind == .rawDecode })
    #expect(rawOperation.parameters["assetID"] == kept.id.uuidString)
    #expect(rawOperation.parameters["rawWhiteBalance"] == RawWhiteBalanceMode.auto.rawValue)
    #expect(rawOperation.parameters["rawExposureBias"] == "1.25")
    #expect(rawOperation.parameters["rawBlackLevel"] == RawBlackLevelMode.auto.rawValue)
    #expect(rawOperation.parameters["rawDemosaic"] == RawDemosaicQuality.high.rawValue)
    #expect(rawOperation.parameters["rawLinear"] == "false")
    #expect(model.parameters.rawProcessingOptions.exposureBias == 1.25)
}

@Test @MainActor func layerTargetPromotesAdjustmentsToNewActiveLayer() async throws {
    let service = RecordingProcessingService()
    let source = URL(fileURLWithPath: "/tmp/layer-source.tiff")
    let baseLayer = ProcessingLayer(
        name: "Base",
        kind: .baseImage,
        inputURL: source
    )
    var project = PhotonStackProject(name: "Layer Target")
    project.appendLayer(baseLayer)
    let previewDirectory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackLayerTargetTest-\(UUID().uuidString)", isDirectory: true)
    defer {
        try? FileManager.default.removeItem(at: previewDirectory)
    }
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: service,
        previewDirectory: previewDirectory
    )

    model.selectLayer(baseLayer)
    #expect(model.activeLayerID == baseLayer.id)
    #expect(model.currentInputURL == source)

    await model.autoStretchPreview()

    let promotedLayer = try #require(model.project.layers.last)
    #expect(model.project.layers.count == 2)
    #expect(promotedLayer.id == model.activeLayerID)
    #expect(promotedLayer.kind == .adjustment)
    #expect(promotedLayer.inputURL?.lastPathComponent.hasPrefix("cache-stretch-") == true)
    #expect(promotedLayer.inputURL?.pathExtension == "png")
    #expect(promotedLayer.parameters["source"] == "operation")
    #expect(promotedLayer.parameters["sourceLayerID"] == baseLayer.id.uuidString)
    #expect(promotedLayer.parameters["operation"] == ProcessingOperationKind.stretch.rawValue)
    #expect(model.currentInputURL == promotedLayer.inputURL)

    let snapshot = await service.snapshot()
    #expect(snapshot.autoStretchCalls == 1)
}

@Test @MainActor func processingSelectedAssetCreatesSourceAndResultLayers() async throws {
    let service = RecordingProcessingService()
    let source = URL(fileURLWithPath: "/tmp/automatic-layer-source.tiff")
    var project = PhotonStackProject(name: "Automatic Layer Target")
    project.addAssets(from: [source])
    let model = PhotonStackWorkspaceModel(project: project, processingService: service)

    #expect(model.project.layers.isEmpty)
    await model.autoStretchPreview()

    #expect(model.project.layers.count == 2)
    let sourceLayer = model.project.layers[0]
    let resultLayer = model.project.layers[1]
    #expect(sourceLayer.kind == .baseImage)
    #expect(sourceLayer.inputURL == source)
    #expect(sourceLayer.parameters["source"] == "automatic")
    #expect(resultLayer.kind == .adjustment)
    #expect(resultLayer.parameters["sourceLayerID"] == sourceLayer.id.uuidString)
    #expect(resultLayer.parameters["operation"] == ProcessingOperationKind.stretch.rawValue)
    #expect(model.activeLayerID == resultLayer.id)
    #expect(model.currentInputURL == resultLayer.inputURL)
}

@Test @MainActor func productManifestExportsArtifactsLayersAndJobs() async throws {
    let service = RecordingProcessingService()
    var project = PhotonStackProject(name: "Manifest Test")
    project.addAssets(from: [URL(fileURLWithPath: "/tmp/manifest-light.nef")])
    let artifact = ProcessingArtifact(
        kind: .registeredSequence,
        name: "Registered Manifest",
        operationKind: .register,
        outputDirectory: URL(fileURLWithPath: "/tmp/registered", isDirectory: true),
        outputURLs: [URL(fileURLWithPath: "/tmp/registered/aligned-manifest.tiff")],
        metrics: ["alignedFrames": "1"]
    )
    project.appendArtifact(artifact)
    project.appendLayer(
        ProcessingLayer(
            name: "Registered Layer",
            kind: .baseImage,
            sourceArtifactID: artifact.id,
            inputURL: URL(fileURLWithPath: "/tmp/registered/aligned-manifest.tiff"),
            opacity: 0.75
        )
    )
    let outputDirectory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackManifestTest-\(UUID().uuidString)", isDirectory: true)
    let output = outputDirectory.appendingPathComponent("manifest.md")
    defer {
        try? FileManager.default.removeItem(at: outputDirectory)
    }

    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: service
    )

    await model.exportProductManifest(to: output)

    let manifest = try String(contentsOf: output, encoding: .utf8)
    #expect(manifest.contains("# PhotonStack Product Manifest"))
    #expect(manifest.contains("Manifest Test"))
    #expect(manifest.contains("Registered Manifest"))
    #expect(manifest.contains("Registered Layer"))
    #expect(manifest.contains("Export Product Manifest"))
    #expect(model.latestJob?.status == .succeeded)
}

@Test @MainActor func previewLayerStackRendersVisibleLayers() async throws {
    let service = RecordingProcessingService()
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackLayerStackTest-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    let baseURL = directory.appendingPathComponent("base.png")
    let overlayURL = directory.appendingPathComponent("overlay.png")
    try writeSolidPNG(to: baseURL, red: 1, green: 0, blue: 0)
    try writeSolidPNG(to: overlayURL, red: 0, green: 0, blue: 1)

    let baseLayer = ProcessingLayer(
        name: "Base",
        kind: .baseImage,
        inputURL: baseURL
    )
    let overlayLayer = ProcessingLayer(
        name: "Overlay",
        kind: .adjustment,
        inputURL: overlayURL,
        blendMode: .screen,
        opacity: 0.5
    )
    var project = PhotonStackProject(name: "Layer Stack Preview Test")
    project.appendLayer(baseLayer)
    project.appendLayer(overlayLayer)

    let previewDirectory = directory.appendingPathComponent("previews", isDirectory: true)
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: service,
        previewDirectory: previewDirectory
    )

    #expect(model.hasVisiblePreviewLayers)

    await model.previewLayerStack()

    let previewURL = try #require(model.previewURL)
    #expect(previewURL.lastPathComponent.hasPrefix("layer-stack-"))
    #expect(FileManager.default.fileExists(atPath: previewURL.path))
    #expect(model.project.editGraph.operations.contains { $0.kind == .preview && $0.parameters["source"] == "layerStack" } == false)
    #expect(model.latestJob?.status == .succeeded)

    model.setLayerVisibility(overlayLayer.id, isVisible: false)

    let refreshedPreviewURL = try #require(model.previewURL)
    #expect(refreshedPreviewURL.lastPathComponent.hasPrefix("cache-layer-stack-live-"))
    #expect(refreshedPreviewURL != previewURL)
    #expect(FileManager.default.fileExists(atPath: refreshedPreviewURL.path))
    #expect(model.layerIsVisible(overlayLayer.id) == false)
    #expect(model.project.editGraph.operations.filter { $0.kind == .preview && $0.parameters["source"] == "layerStack" }.isEmpty)

    model.setLayerVisibility(baseLayer.id, isVisible: false)

    let emptyPreviewURL = try #require(model.previewURL)
    #expect(emptyPreviewURL.lastPathComponent.hasPrefix("cache-layer-stack-empty-"))
    #expect(emptyPreviewURL != refreshedPreviewURL)
    #expect(FileManager.default.fileExists(atPath: emptyPreviewURL.path))
    let emptyPixel = try rgbaPixel(in: emptyPreviewURL, x: 1, y: 1)
    #expect(emptyPixel.alpha == 0)

    model.stepBackPreview()
    #expect(model.layerIsVisible(baseLayer.id))
    #expect(model.previewURL == refreshedPreviewURL)

    model.selectLayer(overlayLayer)
    model.setLayerVisibility(baseLayer.id, isVisible: false)
    let emptyPreviewBeforeDeletion = try #require(model.previewURL)
    model.deleteProcessingLayer(overlayLayer.id)

    #expect(model.activeLayerID == baseLayer.id)
    #expect(model.layerIsVisible(baseLayer.id) == false)
    #expect(try rgbaPixel(in: try #require(model.previewURL), x: 1, y: 1).alpha == 0)

    model.stepBackPreview()
    #expect(model.project.layers.map(\.id) == [baseLayer.id, overlayLayer.id])
    #expect(model.activeLayerID == overlayLayer.id)
    #expect(model.previewURL == emptyPreviewBeforeDeletion)
}

@Test @MainActor func blendModesPreserveBackgroundUnderTransparentAndZeroOpacityPixels() async throws {
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackBlendAlphaTest-\(UUID().uuidString)", isDirectory: true)
    defer { try? FileManager.default.removeItem(at: directory) }
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)

    let baseURL = directory.appendingPathComponent("base.png")
    let overlayURL = directory.appendingPathComponent("half-transparent.png")
    try writeSolidPNG(to: baseURL, red: 0.2, green: 0.4, blue: 0.6, width: 16, height: 8)
    try writeHalfTransparentPNG(
        to: overlayURL,
        red: 0.8,
        green: 0.3,
        blue: 0.1,
        width: 16,
        height: 8
    )
    let expectedBackground = try rgbaPixel(in: baseURL, x: 2, y: 4)

    for mode in ProcessingLayerBlendMode.allCases {
        let base = ProcessingLayer(name: "Base", kind: .baseImage, inputURL: baseURL)
        let overlay = ProcessingLayer(
            name: "Overlay",
            kind: .adjustment,
            inputURL: overlayURL,
            blendMode: mode,
            opacity: 0.5
        )
        var project = PhotonStackProject(name: "Blend Alpha \(mode.rawValue)")
        project.appendLayer(base)
        project.appendLayer(overlay)
        let model = PhotonStackWorkspaceModel(
            project: project,
            processingService: RecordingProcessingService(),
            previewDirectory: directory.appendingPathComponent(mode.rawValue, isDirectory: true)
        )

        await model.previewLayerStack()
        let halfOpacityPreview = try #require(model.previewURL)
        let transparentPixel = try rgbaPixel(in: halfOpacityPreview, x: 2, y: 4)
        #expect(transparentPixel == expectedBackground)

        model.setLayerOpacity(overlay.id, opacity: 0)
        let zeroOpacityPreview = try #require(model.previewURL)
        for x in [2, 13] {
            let pixel = try rgbaPixel(in: zeroOpacityPreview, x: x, y: 4)
            #expect(pixel == expectedBackground)
        }
    }
}

@Test @MainActor func smallerLayerMaskUsesTheLayerCoordinateSpaceInsteadOfCanvasStretching() async throws {
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackSmallerLayerMaskTest-\(UUID().uuidString)", isDirectory: true)
    defer { try? FileManager.default.removeItem(at: directory) }
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)

    let baseURL = directory.appendingPathComponent("base.png")
    let overlayURL = directory.appendingPathComponent("small-overlay.png")
    let maskURL = directory.appendingPathComponent("small-mask.png")
    try writeSolidPNG(to: baseURL, red: 1, green: 0, blue: 0, width: 8, height: 4)
    try writeSolidPNG(to: overlayURL, red: 0, green: 0, blue: 1, width: 4, height: 4)
    try writeSplitMaskPNG(to: maskURL, width: 4, height: 4)

    let mask = ProcessingArtifact(kind: .mask, name: "Small Split Mask", outputURLs: [maskURL])
    let base = ProcessingLayer(name: "Base", kind: .baseImage, inputURL: baseURL)
    let overlay = ProcessingLayer(
        name: "Small Overlay",
        kind: .adjustment,
        inputURL: overlayURL,
        maskArtifactID: mask.id
    )
    var project = PhotonStackProject(name: "Smaller Layer Mask")
    project.appendArtifact(mask)
    project.appendLayer(base)
    project.appendLayer(overlay)
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: RecordingProcessingService(),
        previewDirectory: directory.appendingPathComponent("previews", isDirectory: true)
    )

    await model.previewLayerStack()
    let preview = try #require(model.previewURL)
    let hiddenSmallLayerPixel = try rgbaPixel(in: preview, x: 1, y: 2)
    let revealedSmallLayerPixel = try rgbaPixel(in: preview, x: 3, y: 2)
    let outsideSmallLayerPixel = try rgbaPixel(in: preview, x: 6, y: 2)
    #expect(hiddenSmallLayerPixel.red > 240 && hiddenSmallLayerPixel.blue < 15)
    #expect(revealedSmallLayerPixel.blue > 240 && revealedSmallLayerPixel.red < 15)
    #expect(outsideSmallLayerPixel.red > 240 && outsideSmallLayerPixel.blue < 15)
}

@Test @MainActor func layerMaskControlsCompositeAndMaskLayerDoesNotRenderDirectly() async throws {
    let service = RecordingProcessingService()
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackLayerMaskTest-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    let baseURL = directory.appendingPathComponent("base.png")
    let overlayURL = directory.appendingPathComponent("overlay.png")
    let maskURL = directory.appendingPathComponent("mask.png")
    try writeSolidPNG(to: baseURL, red: 1, green: 0, blue: 0, width: 8, height: 4)
    try writeSolidPNG(to: overlayURL, red: 0, green: 0, blue: 1, width: 8, height: 4)
    try writeSplitMaskPNG(to: maskURL, width: 8, height: 4)

    let maskArtifact = ProcessingArtifact(
        kind: .mask,
        name: "Half Mask",
        operationKind: .starMask,
        outputURLs: [maskURL]
    )
    let baseLayer = ProcessingLayer(name: "Base", kind: .baseImage, inputURL: baseURL)
    let overlayLayer = ProcessingLayer(
        name: "Overlay",
        kind: .adjustment,
        inputURL: overlayURL,
        maskArtifactID: maskArtifact.id
    )
    let standaloneMaskLayer = ProcessingLayer(
        name: "Mask Resource",
        kind: .mask,
        sourceArtifactID: maskArtifact.id,
        inputURL: maskURL,
        isVisible: true
    )
    var project = PhotonStackProject(name: "Layer Mask Test")
    project.appendArtifact(maskArtifact)
    project.appendLayer(baseLayer)
    project.appendLayer(overlayLayer)
    project.appendLayer(standaloneMaskLayer)

    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: service,
        previewDirectory: directory.appendingPathComponent("previews", isDirectory: true)
    )
    #expect(model.availableMaskArtifacts.map(\.id) == [maskArtifact.id])
    #expect(model.layerMaskArtifact(overlayLayer.id)?.id == maskArtifact.id)

    await model.previewLayerStack()

    let maskedPreview = try #require(model.previewURL)
    let maskedLeft = try rgbaPixel(in: maskedPreview, x: 1, y: 1)
    let maskedRight = try rgbaPixel(in: maskedPreview, x: 6, y: 1)
    #expect(maskedLeft.red > 240 && maskedLeft.blue < 15)
    #expect(maskedRight.blue > 240 && maskedRight.red < 15)

    model.setLayerMask(overlayLayer.id, artifactID: nil)
    #expect(model.layerMaskArtifact(overlayLayer.id) == nil)
    let unmaskedPreview = try #require(model.previewURL)
    let unmaskedLeft = try rgbaPixel(in: unmaskedPreview, x: 1, y: 1)
    #expect(unmaskedLeft.blue > 240 && unmaskedLeft.red < 15)

    model.setLayerMask(overlayLayer.id, artifactID: maskArtifact.id)
    #expect(model.layerMaskArtifact(overlayLayer.id)?.id == maskArtifact.id)
}

@Test @MainActor func layerMaskAdjustmentsUpdateCompositeAndCanStepBack() async throws {
    let service = RecordingProcessingService()
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackLayerMaskAdjustmentTest-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    let baseURL = directory.appendingPathComponent("base.png")
    let overlayURL = directory.appendingPathComponent("overlay.png")
    let maskURL = directory.appendingPathComponent("mask.png")
    try writeSolidPNG(to: baseURL, red: 1, green: 0, blue: 0, width: 32, height: 16)
    try writeSolidPNG(to: overlayURL, red: 0, green: 0, blue: 1, width: 32, height: 16)
    try writeSplitMaskPNG(to: maskURL, width: 32, height: 16)

    let maskArtifact = ProcessingArtifact(kind: .mask, name: "Half Mask", outputURLs: [maskURL])
    let baseLayer = ProcessingLayer(name: "Base", kind: .baseImage, inputURL: baseURL)
    let overlayLayer = ProcessingLayer(
        name: "Overlay",
        kind: .adjustment,
        inputURL: overlayURL,
        maskArtifactID: maskArtifact.id
    )
    var project = PhotonStackProject(name: "Layer Mask Adjustment Test")
    project.appendArtifact(maskArtifact)
    project.appendLayer(baseLayer)
    project.appendLayer(overlayLayer)
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: service,
        previewDirectory: directory.appendingPathComponent("previews", isDirectory: true)
    )

    model.setLayerMaskInverted(overlayLayer.id, inverted: true)
    let inverted = try #require(model.previewURL)
    let invertedLeft = try rgbaPixel(in: inverted, x: 4, y: 8)
    let invertedRight = try rgbaPixel(in: inverted, x: 27, y: 8)
    #expect(invertedLeft.blue > 240 && invertedLeft.red < 15)
    #expect(invertedRight.red > 240 && invertedRight.blue < 15)
    #expect(model.layerMaskIsInverted(overlayLayer.id))

    model.stepBackPreview()
    #expect(model.layerMaskIsInverted(overlayLayer.id) == false)
    #expect(model.layerMaskDensity(overlayLayer.id) == 1)

    model.beginLayerMaskAdjustment(overlayLayer.id)
    model.setLayerMaskDensity(overlayLayer.id, density: 0)
    let transparentMask = try #require(model.previewURL)
    let formerlyMaskedPixel = try rgbaPixel(in: transparentMask, x: 4, y: 8)
    #expect(formerlyMaskedPixel.blue > 240 && formerlyMaskedPixel.red < 15)
    model.stepBackPreview()
    #expect(model.layerMaskDensity(overlayLayer.id) == 1)

    model.beginLayerMaskAdjustment(overlayLayer.id)
    model.setLayerMaskFeatherRadius(overlayLayer.id, radius: 1.6)
    #expect(abs(model.layerMaskFeatherRadius(overlayLayer.id) - 1.6) < 0.001)
    let feathered = try #require(model.previewURL)
    let boundaryPixel = try rgbaPixel(in: feathered, x: 15, y: 8)
    #expect(boundaryPixel.red > 15 && boundaryPixel.blue > 15)
}

@Test @MainActor func featheringUniformWhiteMaskDoesNotFadeCanvasEdges() async throws {
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackMaskFeatherEdgeTest-\(UUID().uuidString)", isDirectory: true)
    defer { try? FileManager.default.removeItem(at: directory) }
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)

    let baseURL = directory.appendingPathComponent("base.png")
    let overlayURL = directory.appendingPathComponent("overlay.png")
    let maskURL = directory.appendingPathComponent("mask.png")
    try writeSolidPNG(to: baseURL, red: 1, green: 0, blue: 0, width: 64, height: 32)
    try writeSolidPNG(to: overlayURL, red: 0, green: 0, blue: 1, width: 64, height: 32)
    try writeSolidPNG(to: maskURL, red: 1, green: 1, blue: 1, width: 64, height: 32)

    let mask = ProcessingArtifact(kind: .mask, name: "White Mask", outputURLs: [maskURL])
    let base = ProcessingLayer(name: "Base", kind: .baseImage, inputURL: baseURL)
    let overlay = ProcessingLayer(
        name: "Overlay",
        kind: .adjustment,
        inputURL: overlayURL,
        maskArtifactID: mask.id
    )
    var project = PhotonStackProject(name: "Mask Feather Edge Test")
    project.appendArtifact(mask)
    project.appendLayer(base)
    project.appendLayer(overlay)
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: RecordingProcessingService(),
        previewDirectory: directory.appendingPathComponent("previews", isDirectory: true)
    )

    model.setLayerMaskFeatherRadius(overlay.id, radius: 3.2)
    let preview = try #require(model.previewURL)
    for point in [(0, 0), (63, 0), (0, 31), (63, 31), (32, 16)] {
        let pixel = try rgbaPixel(in: preview, x: point.0, y: point.1)
        #expect(pixel.blue > 240 && pixel.red < 15)
    }
}

@Test @MainActor func transparentLayerMaskCanBeInvertedAsOpaqueGrayscale() async throws {
    let service = RecordingProcessingService()
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackTransparentMaskTest-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    let baseURL = directory.appendingPathComponent("base.png")
    let overlayURL = directory.appendingPathComponent("overlay.png")
    let maskURL = directory.appendingPathComponent("transparent-mask.png")
    try writeSolidPNG(to: baseURL, red: 1, green: 0, blue: 0, width: 32, height: 16)
    try writeSolidPNG(to: overlayURL, red: 0, green: 0, blue: 1, width: 32, height: 16)
    try writeTransparentSplitMaskPNG(to: maskURL, width: 32, height: 16)

    let maskArtifact = ProcessingArtifact(kind: .mask, name: "Transparent Mask", outputURLs: [maskURL])
    let baseLayer = ProcessingLayer(name: "Base", kind: .baseImage, inputURL: baseURL)
    let overlayLayer = ProcessingLayer(
        name: "Overlay",
        kind: .adjustment,
        inputURL: overlayURL,
        maskArtifactID: maskArtifact.id
    )
    var project = PhotonStackProject(name: "Transparent Mask Test")
    project.appendArtifact(maskArtifact)
    project.appendLayer(baseLayer)
    project.appendLayer(overlayLayer)
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: service,
        previewDirectory: directory.appendingPathComponent("previews", isDirectory: true)
    )

    await model.previewLayerStack()
    let normal = try #require(model.previewURL)
    #expect(try rgbaPixel(in: normal, x: 4, y: 8).red > 240)
    #expect(try rgbaPixel(in: normal, x: 27, y: 8).blue > 240)

    model.setLayerMaskInverted(overlayLayer.id, inverted: true)
    let inverted = try #require(model.previewURL)
    #expect(try rgbaPixel(in: inverted, x: 4, y: 8).blue > 240)
    #expect(try rgbaPixel(in: inverted, x: 27, y: 8).red > 240)
}

@Test @MainActor func maskBrushUsesTopDownCoordinatesPreservesSourceAndCanStepBack() async throws {
    let service = RecordingProcessingService()
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackMaskBrushTest-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    let baseURL = directory.appendingPathComponent("base.png")
    let overlayURL = directory.appendingPathComponent("overlay.png")
    let maskURL = directory.appendingPathComponent("mask.png")
    try writeSolidPNG(to: baseURL, red: 1, green: 0, blue: 0, width: 40, height: 20)
    try writeSolidPNG(to: overlayURL, red: 0, green: 0, blue: 1, width: 40, height: 20)
    try writeSolidPNG(to: maskURL, red: 1, green: 1, blue: 1, width: 40, height: 20)

    let maskArtifact = ProcessingArtifact(kind: .mask, name: "Shared Mask", outputURLs: [maskURL])
    let baseLayer = ProcessingLayer(name: "Base", kind: .baseImage, inputURL: baseURL)
    let overlayLayer = ProcessingLayer(
        name: "Overlay",
        kind: .adjustment,
        inputURL: overlayURL,
        maskArtifactID: maskArtifact.id
    )
    let hiddenSharedLayer = ProcessingLayer(
        name: "Shared",
        kind: .adjustment,
        inputURL: overlayURL,
        maskArtifactID: maskArtifact.id,
        isVisible: false
    )
    var project = PhotonStackProject(name: "Mask Brush Test")
    project.appendArtifact(maskArtifact)
    project.appendLayer(baseLayer)
    project.appendLayer(overlayLayer)
    project.appendLayer(hiddenSharedLayer)
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: service,
        previewDirectory: directory.appendingPathComponent("previews", isDirectory: true)
    )

    let stroke = MaskBrushStroke(
        mode: .hide,
        points: [MaskBrushPoint(x: 0.5, y: 0.25)],
        diameterFraction: 0.25
    )
    #expect(model.applyMaskBrushEdits([stroke], to: overlayLayer.id))

    let editedArtifact = try #require(model.layerMaskArtifact(overlayLayer.id))
    let editedMaskURL = try #require(editedArtifact.previewURL)
    #expect(editedArtifact.id != maskArtifact.id)
    #expect(editedArtifact.sourceArtifactIDs == [maskArtifact.id])
    #expect(editedArtifact.parameters["brushStrokes"]?.contains("\"y\":0.25") == true)
    #expect(model.layerMaskArtifact(hiddenSharedLayer.id)?.id == maskArtifact.id)
    #expect(try rgbaPixel(in: maskURL, x: 20, y: 15).red > 240)
    #expect(try rgbaPixel(in: editedMaskURL, x: 20, y: 15).red < 15)
    #expect(try rgbaPixel(in: editedMaskURL, x: 20, y: 5).red > 240)

    let preview = try #require(model.previewURL)
    let topBrushPixel = try rgbaPixel(in: preview, x: 20, y: 15)
    let bottomPixel = try rgbaPixel(in: preview, x: 20, y: 5)
    #expect(topBrushPixel.red > 240 && topBrushPixel.blue < 15)
    #expect(bottomPixel.blue > 240 && bottomPixel.red < 15)

    model.stepBackPreview()
    #expect(model.layerMaskArtifact(overlayLayer.id)?.id == maskArtifact.id)
    #expect(model.project.artifacts.contains(where: { $0.id == editedArtifact.id }))
}

@Test func maskBrushCanvasGeometryMapsLetterboxingZoomAndDiameterConsistently() throws {
    let fitted = MaskBrushCanvasGeometry(
        sourceSize: CGSize(width: 6_000, height: 4_000),
        viewportSize: CGSize(width: 1_000, height: 800),
        zoomScale: 1
    )

    #expect(abs(fitted.imageRect.minX) < 0.001)
    #expect(abs(fitted.imageRect.minY - 66.666_666) < 0.001)
    #expect(abs(fitted.imageRect.width - 1_000) < 0.001)
    #expect(abs(fitted.imageRect.height - 666.666_666) < 0.001)
    #expect(fitted.normalizedPoint(at: CGPoint(x: 500, y: 50)) == nil)
    #expect(fitted.normalizedPoint(at: CGPoint(x: 500, y: 400)) == MaskBrushPoint(x: 0.5, y: 0.5))

    let bottomRight = try #require(fitted.normalizedPoint(at: CGPoint(
        x: fitted.imageRect.maxX,
        y: fitted.imageRect.maxY
    )))
    #expect(abs(bottomRight.x - 1) < 0.000_001)
    #expect(abs(bottomRight.y - 1) < 0.000_001)
    let roundTrip = fitted.canvasPoint(for: MaskBrushPoint(x: 0.23, y: 0.71))
    let normalizedRoundTrip = try #require(fitted.normalizedPoint(at: roundTrip))
    #expect(abs(normalizedRoundTrip.x - 0.23) < 0.000_001)
    #expect(abs(normalizedRoundTrip.y - 0.71) < 0.000_001)
    #expect(abs(fitted.displayDiameter(for: 0.025) - 16.666_666) < 0.001)

    let zoomed = MaskBrushCanvasGeometry(
        sourceSize: CGSize(width: 6_000, height: 4_000),
        viewportSize: CGSize(width: 1_000, height: 800),
        zoomScale: 2
    )
    #expect(abs(zoomed.canvasSize.width - 2_000) < 0.001)
    #expect(abs(zoomed.canvasSize.height - 1_333.333_333) < 0.001)
    #expect(abs(zoomed.imageRect.minX) < 0.001)
    #expect(abs(zoomed.imageRect.minY) < 0.001)
    let zoomedCenter = try #require(zoomed.normalizedPoint(at: CGPoint(x: 1_000, y: 666.666_666)))
    #expect(abs(zoomedCenter.x - 0.5) < 0.000_001)
    #expect(abs(zoomedCenter.y - 0.5) < 0.000_001)
    #expect(abs(zoomed.displayDiameter(for: 0.025) - 33.333_333) < 0.001)
}

@Test func maskBrushStrokeSamplerBreaksStrokeWhenPointerLeavesImage() {
    var sampler = MaskBrushStrokeSampler()
    sampler.sample(MaskBrushPoint(x: 0.1, y: 0.2), minimumSpacing: 0.01)
    sampler.sample(MaskBrushPoint(x: 0.105, y: 0.2), minimumSpacing: 0.01)
    sampler.sample(MaskBrushPoint(x: 0.3, y: 0.2), minimumSpacing: 0.01)
    sampler.sample(nil, minimumSpacing: 0.01)
    sampler.sample(nil, minimumSpacing: 0.01)
    sampler.sample(MaskBrushPoint(x: 0.8, y: 0.7), minimumSpacing: 0.01)
    sampler.sample(MaskBrushPoint(x: 0.9, y: 0.7), minimumSpacing: 0.01)

    #expect(sampler.visibleSegments == [
        [MaskBrushPoint(x: 0.1, y: 0.2), MaskBrushPoint(x: 0.3, y: 0.2)],
        [MaskBrushPoint(x: 0.8, y: 0.7), MaskBrushPoint(x: 0.9, y: 0.7)],
    ])
    #expect(sampler.finish() == [
        [MaskBrushPoint(x: 0.1, y: 0.2), MaskBrushPoint(x: 0.3, y: 0.2)],
        [MaskBrushPoint(x: 0.8, y: 0.7), MaskBrushPoint(x: 0.9, y: 0.7)],
    ])
    #expect(sampler.visibleSegments.isEmpty)
}

@Test @MainActor func maskBrushRollsBackWhenLayerCompositeRefreshFails() async throws {
    let service = RecordingProcessingService()
    let directory = FileManager.default.temporaryDirectory.appendingPathComponent(
        "PhotonStackMaskBrushRollback-\(UUID().uuidString)",
        isDirectory: true
    )
    let previewDirectory = directory.appendingPathComponent("previews", isDirectory: true)
    defer { try? FileManager.default.removeItem(at: directory) }
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)

    let baseURL = directory.appendingPathComponent("base.png")
    let overlayURL = directory.appendingPathComponent("overlay.png")
    let missingURL = directory.appendingPathComponent("will-disappear.png")
    let maskURL = directory.appendingPathComponent("mask.png")
    try writeSolidPNG(to: baseURL, red: 1, green: 0, blue: 0, width: 40, height: 20)
    try writeSolidPNG(to: overlayURL, red: 0, green: 0, blue: 1, width: 40, height: 20)
    try writeSolidPNG(to: missingURL, red: 0, green: 1, blue: 0, width: 40, height: 20)
    try writeSolidPNG(to: maskURL, red: 1, green: 1, blue: 1, width: 40, height: 20)

    let maskArtifact = ProcessingArtifact(kind: .mask, name: "Mask", outputURLs: [maskURL])
    let baseLayer = ProcessingLayer(name: "Base", kind: .baseImage, inputURL: baseURL)
    let overlayLayer = ProcessingLayer(
        name: "Overlay",
        kind: .adjustment,
        inputURL: overlayURL,
        maskArtifactID: maskArtifact.id
    )
    let disappearingLayer = ProcessingLayer(
        name: "Unavailable",
        kind: .adjustment,
        inputURL: missingURL
    )
    var project = PhotonStackProject(name: "Mask Brush Rollback")
    project.appendArtifact(maskArtifact)
    project.appendLayer(baseLayer)
    project.appendLayer(overlayLayer)
    project.appendLayer(disappearingLayer)
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: service,
        previewDirectory: previewDirectory
    )
    let initialProject = model.project
    let initialPreview = model.previewURL
    let initialCanvasMode = model.canvasMode
    let initialCanStepBack = model.canStepBack
    try FileManager.default.removeItem(at: missingURL)

    let applied = model.applyMaskBrushEdits(
        [MaskBrushStroke(
            mode: .hide,
            points: [MaskBrushPoint(x: 0.5, y: 0.5)],
            diameterFraction: 0.2
        )],
        to: overlayLayer.id
    )

    #expect(applied == false)
    #expect(model.project == initialProject)
    #expect(model.previewURL == initialPreview)
    #expect(model.canvasMode == initialCanvasMode)
    #expect(model.canStepBack == initialCanStepBack)
    #expect(model.layerMaskArtifact(overlayLayer.id)?.id == maskArtifact.id)
    #expect(model.errorMessage?.contains("Unavailable") == true)
    let previewFiles = (try? FileManager.default.contentsOfDirectory(
        at: previewDirectory,
        includingPropertiesForKeys: nil
    )) ?? []
    #expect(previewFiles.contains { $0.lastPathComponent.hasPrefix("mask-brush-") } == false)
}

@Test @MainActor func exportUsesVisibleLayerCompositeInsteadOfActiveLayer() async throws {
    let service = RecordingProcessingService()
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackLayerExportTest-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    let baseURL = directory.appendingPathComponent("base.png")
    let overlayURL = directory.appendingPathComponent("overlay.png")
    try writeSolidPNG(to: baseURL, red: 1, green: 0, blue: 0, width: 8, height: 4)
    try writeSolidPNG(to: overlayURL, red: 0, green: 0, blue: 1, width: 8, height: 4)
    let baseLayer = ProcessingLayer(name: "Base", kind: .baseImage, inputURL: baseURL)
    let overlayLayer = ProcessingLayer(name: "Overlay", kind: .adjustment, inputURL: overlayURL, opacity: 0.5)
    var project = PhotonStackProject(name: "Layer Export Test")
    project.appendLayer(baseLayer)
    project.appendLayer(overlayLayer)
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: service,
        previewDirectory: directory.appendingPathComponent("previews", isDirectory: true)
    )
    model.selectLayer(baseLayer)

    await model.exportCurrentImage(to: directory.appendingPathComponent("final.tiff"))

    let exportInput = try #require(await service.snapshot().convertInput)
    #expect(exportInput.lastPathComponent.hasPrefix("export-layer-stack-"))
    #expect(FileManager.default.fileExists(atPath: exportInput.path))
    let exportedPixel = try rgbaPixel(in: exportInput, x: 2, y: 1)
    #expect(exportedPixel.red > 110 && exportedPixel.red < 145)
    #expect(exportedPixel.blue > 110 && exportedPixel.blue < 145)

    model.setLayerVisibility(baseLayer.id, isVisible: false)
    model.setLayerVisibility(overlayLayer.id, isVisible: false)
    await model.exportCurrentImage(to: directory.appendingPathComponent("empty-final.tiff"))

    let emptyExportInput = try #require(await service.snapshot().convertInput)
    #expect(emptyExportInput.lastPathComponent.hasPrefix("export-layer-stack-empty-"))
    let emptySource = try #require(CGImageSourceCreateWithURL(emptyExportInput as CFURL, nil))
    let emptyImage = try #require(CGImageSourceCreateImageAtIndex(emptySource, 0, nil))
    #expect(emptyImage.width == 8)
    #expect(emptyImage.height == 4)
    #expect(try rgbaPixel(in: emptyExportInput, x: 2, y: 1).alpha == 0)
}

@Test @MainActor func selectingSourceAssetExportsTheDisplayedAssetInsteadOfVisibleLayers() async throws {
    let service = RecordingProcessingService()
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackSourceCanvasExport-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    let redURL = directory.appendingPathComponent("red.png")
    let blueURL = directory.appendingPathComponent("blue.png")
    try writeSolidPNG(to: redURL, red: 1, green: 0, blue: 0, width: 8, height: 4)
    try writeSolidPNG(to: blueURL, red: 0, green: 0, blue: 1, width: 8, height: 4)
    var project = PhotonStackProject(name: "Source Canvas Export")
    project.addAssets(from: [redURL, blueURL])
    let redLayer = ProcessingLayer(name: "Red layer", kind: .baseImage, inputURL: redURL)
    project.appendLayer(redLayer)
    let blueAsset = try #require(project.assets.last)
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: service,
        previewDirectory: directory.appendingPathComponent("previews", isDirectory: true)
    )

    model.selectLayer(redLayer)
    #expect(model.canvasMode == .layerComposite)
    model.select(blueAsset)
    #expect(model.canvasMode == .sourcePreview)
    #expect(model.activeLayerID == nil)
    #expect(model.currentInputURL == blueURL)

    await model.exportCurrentImage(to: directory.appendingPathComponent("blue-export.tiff"))

    #expect(await service.snapshot().convertInput == blueURL)
}

@Test @MainActor func previewingArtifactProcessesAndExportsTheDisplayedArtifact() async throws {
    let service = RecordingProcessingService()
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackArtifactCanvas-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    let redURL = directory.appendingPathComponent("red.png")
    let blueURL = directory.appendingPathComponent("artifact-blue.png")
    try writeSolidPNG(to: redURL, red: 1, green: 0, blue: 0, width: 8, height: 4)
    try writeSolidPNG(to: blueURL, red: 0, green: 0, blue: 1, width: 8, height: 4)
    let redLayer = ProcessingLayer(name: "Red layer", kind: .baseImage, inputURL: redURL)
    let artifact = ProcessingArtifact(kind: .editedImage, name: "Blue artifact", outputURLs: [blueURL])
    var project = PhotonStackProject(name: "Artifact Canvas")
    project.addAssets(from: [redURL])
    project.appendLayer(redLayer)
    project.appendArtifact(artifact)
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: service,
        previewDirectory: directory.appendingPathComponent("previews", isDirectory: true)
    )

    model.selectLayer(redLayer)
    model.previewArtifact(artifact)
    #expect(model.canvasMode == .sourcePreview)
    #expect(model.activeLayerID == nil)
    #expect(model.currentInputURL == blueURL)

    await model.exportCurrentImage(to: directory.appendingPathComponent("artifact-export.tiff"))
    #expect(await service.snapshot().convertInput == blueURL)

    await model.autoStretchPreview()
    let snapshot = await service.snapshot()
    #expect(snapshot.autoStretchInputs == [blueURL])
    #expect(model.canvasMode == .layerComposite)
    let resultLayer = try #require(model.activeLayer)
    let sourceLayerID = try #require(resultLayer.parameters["sourceLayerID"].flatMap(UUID.init(uuidString:)))
    let sourceLayer = try #require(model.project.layers.first(where: { $0.id == sourceLayerID }))
    #expect(sourceLayer.inputURL == blueURL)
    #expect(sourceLayer.sourceArtifactID == artifact.id)
}

@Test @MainActor func previewChangeClearsStaleHistogramWhenAutomaticRefreshFails() async throws {
    let service = RecordingProcessingService(failingHistogramCallIndices: [2])
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackHistogramFailure-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    let input = directory.appendingPathComponent("input.png")
    try writeSolidPNG(to: input, red: 0.1, green: 0.2, blue: 0.3, width: 8, height: 4)
    var project = PhotonStackProject(name: "Histogram Failure")
    project.addAssets(from: [input])
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: service,
        previewDirectory: directory.appendingPathComponent("previews", isDirectory: true)
    )

    await model.refreshHistogram()
    #expect(model.histogram?.bins.count == 64)
    #expect(model.histogram?.bins.first == 1)

    await model.autoStretchPreview()

    #expect(model.previewURL != input)
    #expect(model.histogram == nil)
    #expect(model.latestJob?.status == .succeeded)
    #expect(model.project.editGraph.operations.contains(where: { $0.kind == .stretch }))
}

@Test @MainActor func histogramRejectsMalformedSuccessReport() async throws {
    let service = RecordingProcessingService(
        histogramOutputOverride: #"{"type":"complete","command":"histogram","bins":[1],"minimum":0,"maximum":1,"mean":0.5}"#
    )
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackMalformedHistogram-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    let input = directory.appendingPathComponent("input.png")
    try writeSolidPNG(to: input, red: 0.1, green: 0.2, blue: 0.3, width: 8, height: 4)
    var project = PhotonStackProject(name: "Malformed Histogram")
    project.addAssets(from: [input])
    let model = PhotonStackWorkspaceModel(project: project, processingService: service)

    await model.refreshHistogram()

    #expect(model.latestJob?.status == .failed)
    #expect(model.histogram == nil)
    #expect(model.errorMessage?.contains("histogram") == true)
}

@Test @MainActor func histogramAcceptsFractionalCoverageBins() async throws {
    let binValues = Array(repeating: "0.25", count: 64).joined(separator: ",")
    let service = RecordingProcessingService(
        histogramOutputOverride: "{\"type\":\"complete\",\"command\":\"histogram\",\"bins\":[\(binValues)],\"minimum\":0,\"maximum\":1,\"mean\":0.5}"
    )
    let input = URL(fileURLWithPath: "/tmp/photonstack-fractional-histogram.png")
    var project = PhotonStackProject(name: "Fractional Histogram")
    project.addAssets(from: [input])
    let model = PhotonStackWorkspaceModel(project: project, processingService: service)

    await model.refreshHistogram()

    #expect(model.latestJob?.status == .succeeded)
    #expect(model.histogram?.bins.count == 64)
    #expect(model.histogram?.bins.first == 0.25)
}

@Test @MainActor func curveInteractivePreviewUsesCombinedCurvePreviewProcessing() async throws {
    let service = RecordingProcessingService()
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackCurveInteractivePreview-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    let input = directory.appendingPathComponent("input.png")
    try writeSolidPNG(to: input, red: 0.2, green: 0.3, blue: 0.4, width: 12, height: 8)
    var project = PhotonStackProject(name: "Curve Interactive Preview")
    project.addAssets(from: [input])
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: service,
        previewDirectory: directory.appendingPathComponent("previews", isDirectory: true)
    )

    var points = model.parameters.curveEditorPoints
    points[1].output = 0.38
    model.scheduleCurvePreview(points: points)

    try await waitForCurvePreviewCall(from: service)

    let snapshot = await service.snapshot()
    #expect(snapshot.makeCurvePreviewCalls == 1)
    #expect(snapshot.makePreviewCalls == 0)
}

@Test @MainActor func explicitCurvePreviewShowsPendingMultiPointEditWithoutCommittingIt() async throws {
    let service = RecordingProcessingService()
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackExplicitCurvePreview-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    let input = directory.appendingPathComponent("input.png")
    try writeSolidPNG(to: input, red: 0.2, green: 0.3, blue: 0.4, width: 12, height: 8)
    var project = PhotonStackProject(name: "Explicit Curve Preview")
    project.addAssets(from: [input])
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: service,
        previewDirectory: directory.appendingPathComponent("previews", isDirectory: true)
    )
    var updated = model.parameters
    updated.replaceCurvePoints([
        CurveEditorPoint(input: 0.0, output: 0.0),
        CurveEditorPoint(input: 0.25, output: 0.18),
        CurveEditorPoint(input: 0.62, output: 0.74),
        CurveEditorPoint(input: 1.0, output: 1.0),
    ])
    model.setProcessingParameters(updated)

    model.startCurvePreview()
    try await waitForCurvePreviewCompletion(in: model)

    #expect(model.previewURL != input)
    #expect(model.project.editGraph.operations.isEmpty)
    #expect(model.project.layers.isEmpty)

    model.resetCurveEditor()

    #expect(model.previewURL == input)
    #expect(model.project.editGraph.operations.isEmpty)
}

@Test @MainActor func curveInteractivePreviewDownsamplesInteractiveWidth() async throws {
    let service = RecordingProcessingService()
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackCurveInteractiveWidth-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    let input = directory.appendingPathComponent("input.png")
    try writeSolidPNG(to: input, red: 0.2, green: 0.3, blue: 0.4, width: 12, height: 8)
    var project = PhotonStackProject(name: "Curve Interactive Width")
    project.addAssets(from: [input])
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: service,
        previewDirectory: directory.appendingPathComponent("previews", isDirectory: true)
    )

    var updatedParameters = model.parameters
    updatedParameters.previewWidth = 2400
    model.setProcessingParameters(updatedParameters)

    var points = model.parameters.curveEditorPoints
    points[1].output = 0.38
    model.scheduleCurvePreview(points: points)

    try await waitForCurvePreviewCall(from: service)

    let snapshot = await service.snapshot()
    #expect(snapshot.makeCurvePreviewCalls == 1)
    #expect(snapshot.curvePreviewWidth == 1200)
}

@Test @MainActor func curveProcessingWidthFallsBackToAssetMetadata() throws {
    let input = URL(fileURLWithPath: "/tmp/photonstack-raw-metadata-width.nef")
    var project = PhotonStackProject(name: "Curve Metadata Width")
    project.addAssets(from: [input])
    let assetID = try #require(project.assets.first?.id)
    project.updateMetadata(
        for: assetID,
        metadata: AssetMetadata(
            width: 6048,
            height: 4032,
            channels: 3,
            bitsPerChannel: 16,
            formatDescription: "RAW"
        )
    )

    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: RecordingProcessingService(requiresExistingInputFiles: false)
    )

    #expect(model.resolvedPixelWidthForCurveProcessing(at: input) == 6048)
}

@Test @MainActor func curveApplySurfacesExecutionSourceInJobMessage() async throws {
    let service = RecordingProcessingService()
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackCurveApplyMessage-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    let input = directory.appendingPathComponent("input.png")
    try writeSolidPNG(to: input, red: 0.25, green: 0.35, blue: 0.45, width: 12, height: 8)
    var project = PhotonStackProject(name: "Curve Apply Message")
    project.addAssets(from: [input])
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: service,
        previewDirectory: directory.appendingPathComponent("previews", isDirectory: true)
    )

    var updatedParameters = model.parameters
    var points = updatedParameters.curveEditorPoints
    points[1].output = 0.42
    updatedParameters.replaceCurvePoints(points)
    model.setProcessingParameters(updatedParameters)

    await model.applyCurvePreview()

    #expect(model.latestJob?.status == .succeeded)
    #expect(model.latestJob?.message.contains("\"executionSource\" : \"processing-service\"") == true)
    #expect(model.latestJob?.message.contains("[curve apply source: processing-service]") == true)
    #expect(model.commandOutput.contains("\"executionSource\" : \"processing-service\""))
    #expect(model.commandOutput.contains("[curve apply source: processing-service]"))
}

@Test @MainActor func resettingAfterRepeatedMultiPointCurveAppliesRestoresOriginalState() async throws {
    let service = RecordingProcessingService(requiresExistingInputFiles: false)
    let input = URL(fileURLWithPath: "/tmp/photonstack-repeated-curve-reset.png")
    var project = PhotonStackProject(name: "Repeated Curve Reset")
    project.addAssets(from: [input])
    let model = PhotonStackWorkspaceModel(project: project, processingService: service)
    let originalLayers = model.project.layers
    let originalGraph = model.project.editGraph

    for (firstOutput, secondOutput) in [(0.30, 0.72), (0.18, 0.84), (0.42, 0.66)] {
        var updated = model.parameters
        updated.replaceCurvePoints([
            CurveEditorPoint(input: 0.0, output: 0.0),
            CurveEditorPoint(input: 0.24, output: firstOutput),
            CurveEditorPoint(input: 0.58, output: secondOutput),
            CurveEditorPoint(input: 1.0, output: 1.0),
        ])
        model.setProcessingParameters(updated)
        await model.applyCurvePreview()
        #expect(model.latestJob?.status == .succeeded)
    }

    #expect(model.project.editGraph.operations.map(\.kind) == [.curves, .curves, .curves])
    #expect(model.previewURL != input)

    model.resetCurveEditor()

    #expect(model.previewURL == input)
    #expect(model.project.layers == originalLayers)
    #expect(model.project.editGraph == originalGraph)
    #expect(model.parameters.curveChannel == .rgb)
    #expect(model.parameters.curveEditorPoints.map(\.input) == [0.0, 0.5, 1.0])
    #expect(model.parameters.curveEditorPoints.map(\.output) == [0.0, 0.5, 1.0])
}

#if os(macOS)
@Test @MainActor func nativeCurvePreviewCreatesMissingManagedCacheDirectory() async throws {
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackNativeCurvePreviewDirectory-\(UUID().uuidString)", isDirectory: true)
    let executable = directory.appendingPathComponent("photonstack")
    let input = directory.appendingPathComponent("input.png")
    let missingPreviewDirectory = directory
        .appendingPathComponent("not-created", isDirectory: true)
        .appendingPathComponent("previews", isDirectory: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    try "#!/usr/bin/env bash\nexit 99\n".write(to: executable, atomically: true, encoding: .utf8)
    try FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: executable.path)
    try writeSolidPNG(to: input, red: 0.25, green: 0.35, blue: 0.45, width: 24, height: 16)

    var project = PhotonStackProject(name: "Native Curve Preview Directory")
    project.addAssets(from: [input])
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: CLIProcessingService(executableURL: executable),
        previewDirectory: missingPreviewDirectory
    )
    var updated = model.parameters
    var points = updated.curveEditorPoints
    points[1].output = 0.35
    updated.replaceCurvePoints(points)
    model.setProcessingParameters(updated)

    model.startCurvePreview()
    try await waitForCurvePreviewCompletion(in: model)

    #expect(FileManager.default.fileExists(atPath: missingPreviewDirectory.path))
    #expect(model.previewURL.map { FileManager.default.fileExists(atPath: $0.path) } == true)
    #expect(model.errorMessage == nil)
    #expect(model.project.editGraph.operations.isEmpty)
}

@Test @MainActor func curveApplyCommitsDisplayProxyAndRecordsFullResolutionReplay() async throws {
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackCurveProxyCommit-\(UUID().uuidString)", isDirectory: true)
    let executable = directory.appendingPathComponent("photonstack")
    let input = directory.appendingPathComponent("input.png")
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    try "#!/usr/bin/env bash\nexit 99\n".write(to: executable, atomically: true, encoding: .utf8)
    try FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: executable.path)
    try writeSolidPNG(to: input, red: 0.25, green: 0.35, blue: 0.45, width: 2_000, height: 16)

    var project = PhotonStackProject(name: "Curve Proxy Commit")
    project.addAssets(from: [input])
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: CLIProcessingService(executableURL: executable),
        previewDirectory: directory.appendingPathComponent("previews", isDirectory: true)
    )
    var updatedParameters = model.parameters
    updatedParameters.previewWidth = 1_600
    var points = updatedParameters.curveEditorPoints
    points[1].output = 0.42
    updatedParameters.replaceCurvePoints(points)
    model.setProcessingParameters(updatedParameters)

    await model.applyCurvePreview()

    let output = try #require(model.previewURL)
    let source = try #require(CGImageSourceCreateWithURL(output as CFURL, nil))
    let image = try #require(CGImageSourceCreateImageAtIndex(source, 0, nil))
    #expect(image.width == 800)
    #expect(model.latestJob?.status == .succeeded)
    #expect(model.commandOutput.contains("engine-bridge-proxy"))
    let operation = try #require(model.project.editGraph.operations.last)
    #expect(operation.kind == .curves)
    #expect(operation.parameters["previewOnly"] == nil)
    #expect(operation.parameters["points"] != nil)
}
#endif

@Test @MainActor func curveApplyDefersHistogramRefreshUntilAfterPreviewCommit() async throws {
    let service = RecordingProcessingService(histogramDelayNanoseconds: 50_000_000)
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackCurveDeferredHistogram-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    let input = directory.appendingPathComponent("input.png")
    try writeSolidPNG(to: input, red: 0.25, green: 0.35, blue: 0.45, width: 12, height: 8)
    var project = PhotonStackProject(name: "Curve Deferred Histogram")
    project.addAssets(from: [input])
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: service,
        previewDirectory: directory.appendingPathComponent("previews", isDirectory: true)
    )

    await model.refreshHistogram()
    let histogramCallsBeforeApply = await service.histogramCalls()
    #expect(histogramCallsBeforeApply == 1)

    var updatedParameters = model.parameters
    var points = updatedParameters.curveEditorPoints
    points[1].output = 0.42
    updatedParameters.replaceCurvePoints(points)
    model.setProcessingParameters(updatedParameters)

    await model.applyCurvePreview()

    #expect(model.latestJob?.status == .succeeded)
    #expect(await service.histogramCalls() == histogramCallsBeforeApply)

    try await waitForHistogramCall(from: service, minimum: histogramCallsBeforeApply + 1)
    #expect(await service.histogramCalls() == histogramCallsBeforeApply + 1)
}

@Test @MainActor func cancellingDuringAutomaticHistogramRefreshDoesNotCommitTheOperation() async throws {
    let service = RecordingProcessingService(
        histogramDelayNanoseconds: 300_000_000,
        ignoreHistogramCancellation: true
    )
    let input = URL(fileURLWithPath: "/tmp/photonstack-histogram-cancel.png")
    var project = PhotonStackProject(name: "Histogram Cancellation")
    project.addAssets(from: [input])
    let model = PhotonStackWorkspaceModel(project: project, processingService: service)
    let initialPreview = model.previewURL
    let initialLayers = model.project.layers
    let initialArtifacts = model.project.artifacts

    model.startAutoStretchPreview()
    try await waitForHistogramCall(from: service)
    model.cancelCurrentTask()
    try await waitForProcessingCompletion(in: model)

    #expect(model.latestJob?.status == .cancelled)
    #expect(model.previewURL == initialPreview)
    #expect(model.project.layers == initialLayers)
    #expect(model.project.artifacts == initialArtifacts)
    #expect(model.project.editGraph.operations.isEmpty)
    #expect(model.histogram == nil)
    #expect(model.commandOutput == model.localized(.cancelledTask))
}

@Test @MainActor func exportPreservesScientificFITSUntilDisplayStretch() async throws {
    let service = RecordingProcessingService()
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackFullResolutionExportTest-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    let rawURL = directory.appendingPathComponent("DSC_6699.NEF")
    try Data([0]).write(to: rawURL)
    let rawPreviewURL = directory.appendingPathComponent("raw-preview.png")
    let stretchedPreviewURL = directory.appendingPathComponent("stretched-preview.png")
    var project = PhotonStackProject(name: "Full Resolution Export Test")
    project.addAssets(from: [rawURL])
    project.editGraph = EditGraph(operations: [
        EditOperation(
            kind: .rawDecode,
            parameters: ["width": "1200", "output": rawPreviewURL.path]
        ),
        EditOperation(
            kind: .background,
            parameters: ["model": "grid", "mode": "subtract", "strength": "0.35"]
        ),
        EditOperation(
            kind: .stretch,
            parameters: ["targetBackground": "0.16", "output": stretchedPreviewURL.path]
        ),
    ])
    let previewArtifact = ProcessingArtifact(
        kind: .editedImage,
        name: "Preview",
        outputURLs: [stretchedPreviewURL]
    )
    project.appendArtifact(previewArtifact)
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: service,
        previewDirectory: directory.appendingPathComponent("previews", isDirectory: true)
    )
    model.previewArtifact(previewArtifact)

    await model.exportCurrentImage(to: directory.appendingPathComponent("final.tiff"))

    let snapshot = await service.snapshot()
    #expect(snapshot.convertInputs.count == 2)
    #expect(snapshot.convertInputs.first == rawURL)
    let firstConvertOutput = try #require(snapshot.convertOutputs.first)
    #expect(firstConvertOutput.pathExtension == "fits")
    #expect(snapshot.convertOptions.first?.fitsValueMode == .scientific)
    #expect(snapshot.backgroundPreserveBrightness == false)
    #expect(snapshot.backgroundProtectBrightTargets == false)
    #expect(snapshot.autoStretchInputs.count == 1)
    #expect(snapshot.autoStretchInputs.first?.pathExtension == "fits")
    #expect(snapshot.autoStretchInputs.first != firstConvertOutput)
    #expect(snapshot.autoStretchOutputs.count == 1)
    let stretchedOutput = try #require(snapshot.autoStretchOutputs.first)
    #expect(stretchedOutput.pathExtension == "tiff")
    #expect(snapshot.convertInputs.last == stretchedOutput)
}

@Test @MainActor func exportKeepsLaunchOptionsWhileSettingsLoadDuringReplay() async throws {
    let service = RecordingProcessingService(autoStretchDelayNanoseconds: 300_000_000)
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackExportOptionSnapshot-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    let rawURL = directory.appendingPathComponent("input.nef")
    try Data([0]).write(to: rawURL)
    let rawPreviewURL = directory.appendingPathComponent("raw-preview.png")
    let stretchedPreviewURL = directory.appendingPathComponent("stretched-preview.png")
    var project = PhotonStackProject(name: "Export Option Snapshot")
    project.addAssets(from: [rawURL])
    project.editGraph = EditGraph(operations: [
        EditOperation(
            kind: .rawDecode,
            parameters: ["width": "1200", "output": rawPreviewURL.path]
        ),
        EditOperation(
            kind: .stretch,
            parameters: ["targetBackground": "0.16", "output": stretchedPreviewURL.path]
        ),
    ])
    let previewArtifact = ProcessingArtifact(
        kind: .editedImage,
        name: "Preview",
        outputURLs: [stretchedPreviewURL]
    )
    project.appendArtifact(previewArtifact)

    let settingsRepository = AppSettingsRepository(
        settingsURL: directory.appendingPathComponent("settings.json")
    )
    let launchOptions = ExportOptions(bitDepth: .automatic, colorSpace: .srgb, jpegQuality: 0.92)
    let loadedOptions = ExportOptions(
        bitDepth: .sixteen,
        colorSpace: .linearSRGB,
        fitsValueMode: .scientific,
        jpegQuality: 0.55
    )
    try await settingsRepository.save(AppSettings(exportOptions: loadedOptions))
    var parameters = ProcessingParameters()
    parameters.rawProcessingOptions = RawProcessingOptions(
        whiteBalanceMode: .daylight,
        exposureBias: 0.75,
        blackLevelMode: .auto,
        demosaicQuality: .high,
        linearOutput: false
    )
    let model = PhotonStackWorkspaceModel(
        project: project,
        parameters: parameters,
        exportOptions: launchOptions,
        processingService: service,
        settingsRepository: settingsRepository,
        previewDirectory: directory.appendingPathComponent("previews", isDirectory: true)
    )
    model.previewArtifact(previewArtifact)

    let output = directory.appendingPathComponent("final.tiff")
    model.startExportCurrentImage(to: output)
    try await waitForAutoStretchCall(from: service)
    await model.loadUserSettings()
    #expect(model.exportOptions == loadedOptions)
    try await waitForStartedProcessingCompletion(in: model)

    let snapshot = await service.snapshot()
    let effectiveLaunchOptions = ExportOptions(
        bitDepth: .sixteen,
        colorSpace: launchOptions.colorSpace,
        jpegQuality: launchOptions.jpegQuality
    )
    #expect(snapshot.convertOptions.last == effectiveLaunchOptions)
    #expect(snapshot.convertRawOptions.last == parameters.rawProcessingOptions)

    let sidecarURL = directory.appendingPathComponent("final.tiff.photonstack.json")
    let sidecar = try #require(
        try JSONSerialization.jsonObject(with: Data(contentsOf: sidecarURL)) as? [String: Any]
    )
    let sidecarExport = try #require(sidecar["exportOptions"] as? [String: Any])
    #expect(sidecarExport["bitDepth"] as? String == effectiveLaunchOptions.bitDepth.rawValue)
    #expect(sidecarExport["requestedBitDepth"] as? String == launchOptions.bitDepth.rawValue)
    #expect(sidecarExport["colorSpace"] as? String == launchOptions.colorSpace.rawValue)
    #expect(sidecarExport["fitsValueMode"] as? String == launchOptions.fitsValueMode.rawValue)
    #expect(sidecarExport["nonFiniteHandling"] as? String == "display-zero")
    #expect(sidecarExport["dynamicRangeHandling"] as? String == "display-normalized-quantized")
    #expect(sidecarExport["jpegQuality"] as? Double == launchOptions.jpegQuality)
    let sidecarRaw = try #require(sidecar["rawOptions"] as? [String: Any])
    #expect(sidecarRaw["whiteBalanceMode"] as? String == parameters.rawProcessingOptions.whiteBalanceMode.rawValue)
    #expect(sidecarRaw["exposureBias"] as? Double == parameters.rawProcessingOptions.exposureBias)
}

@Test @MainActor func scientificFITSExportRecordsPreservedFloatDynamicRange() async throws {
    let service = RecordingProcessingService()
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackScientificFITSSidecar-\(UUID().uuidString)", isDirectory: true)
    defer { try? FileManager.default.removeItem(at: directory) }
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    let input = directory.appendingPathComponent("input.png")
    try writeSolidPNG(to: input, red: 0.2, green: 0.3, blue: 0.4)
    var project = PhotonStackProject(name: "Scientific FITS Sidecar")
    project.addAssets(from: [input])
    let model = PhotonStackWorkspaceModel(
        project: project,
        exportOptions: ExportOptions(fitsValueMode: .scientific),
        processingService: service,
        previewDirectory: directory.appendingPathComponent("previews", isDirectory: true)
    )
    let output = directory.appendingPathComponent("final.fits")

    await model.exportCurrentImage(to: output)

    let sidecar = try #require(
        try JSONSerialization.jsonObject(
            with: Data(contentsOf: directory.appendingPathComponent("final.fits.photonstack.json"))
        ) as? [String: Any]
    )
    let sidecarExport = try #require(sidecar["exportOptions"] as? [String: Any])
    #expect(sidecarExport["dynamicRangeHandling"] as? String == "float32-preserved")
    #expect(sidecarExport["nonFiniteHandling"] as? String == "preserve")
}

@Test @MainActor func jpegExportRecordsAndUsesItsActualEightBitDepth() async throws {
    let service = RecordingProcessingService()
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackJPEGEffectiveDepth-\(UUID().uuidString)", isDirectory: true)
    defer { try? FileManager.default.removeItem(at: directory) }
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)

    let input = directory.appendingPathComponent("input.tiff")
    try Data("image".utf8).write(to: input)
    var project = PhotonStackProject(name: "JPEG Effective Depth")
    project.addAssets(from: [input])
    let requested = ExportOptions(bitDepth: .sixteen, colorSpace: .srgb, jpegQuality: 0.81)
    let model = PhotonStackWorkspaceModel(
        project: project,
        exportOptions: requested,
        processingService: service,
        previewDirectory: directory.appendingPathComponent("previews", isDirectory: true)
    )
    let output = directory.appendingPathComponent("final.jpg")

    await model.exportCurrentImage(to: output)

    let snapshot = await service.snapshot()
    #expect(snapshot.convertOptions.last == ExportOptions(
        bitDepth: .eight,
        colorSpace: requested.colorSpace,
        jpegQuality: requested.jpegQuality
    ))
    #expect(model.exportOptions == requested)
    let sidecar = try #require(
        try JSONSerialization.jsonObject(
            with: Data(contentsOf: directory.appendingPathComponent("final.jpg.photonstack.json"))
        ) as? [String: Any]
    )
    let sidecarExport = try #require(sidecar["exportOptions"] as? [String: Any])
    #expect(sidecarExport["bitDepth"] as? String == ExportBitDepth.eight.rawValue)
    #expect(sidecarExport["requestedBitDepth"] as? String == ExportBitDepth.sixteen.rawValue)
    #expect(sidecarExport["alphaHandling"] as? String == "flatten-black")
}

@Test @MainActor func exportSucceedsWithWarningWhenSidecarCannotBeSavedAndRecoversOnRetry() async throws {
    let service = RecordingProcessingService()
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackExportSidecarFailure-\(UUID().uuidString)", isDirectory: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)

    let input = directory.appendingPathComponent("input.tiff")
    let sourceData = Data("exported image".utf8)
    try sourceData.write(to: input)
    var project = PhotonStackProject(name: "Export Sidecar Failure Test")
    project.addAssets(from: [input])
    let model = PhotonStackWorkspaceModel(project: project, processingService: service)
    let output = directory.appendingPathComponent("final.tiff")
    let sidecar = directory.appendingPathComponent("final.tiff.photonstack.json", isDirectory: true)
    try FileManager.default.createDirectory(at: sidecar, withIntermediateDirectories: true)

    await model.exportCurrentImage(to: output)

    #expect(try Data(contentsOf: output) == sourceData)
    #expect(model.latestJob?.status == .succeeded)
    #expect(model.errorMessage?.hasPrefix(model.localized(.exportSidecarFailed)) == true)
    var isDirectory = ObjCBool(false)
    #expect(FileManager.default.fileExists(atPath: sidecar.path, isDirectory: &isDirectory))
    #expect(isDirectory.boolValue)

    try FileManager.default.removeItem(at: sidecar)
    await model.exportCurrentImage(to: output)

    #expect(model.latestJob?.status == .succeeded)
    #expect(model.errorMessage == nil)
    #expect(try JSONSerialization.jsonObject(with: Data(contentsOf: sidecar)) is [String: Any])
}

@Test func maskBrushDecodingNormalizesPersistedValues() throws {
    let stroke = try JSONDecoder().decode(
        MaskBrushStroke.self,
        from: Data(
            #"{"mode":"hide","points":[{"x":-2,"y":3}],"diameterFraction":4,"opacity":-1}"#.utf8
        )
    )

    #expect(stroke.points == [MaskBrushPoint(x: 0, y: 1)])
    #expect(stroke.diameterFraction == 1)
    #expect(stroke.opacity == 0)

    let legacyStroke = try JSONDecoder().decode(
        MaskBrushStroke.self,
        from: Data(#"{"mode":"reveal","points":[],"diameterFraction":0.2}"#.utf8)
    )
    #expect(legacyStroke.opacity == 1)
}

@Test @MainActor func customCurvePresetExportRoundTripsThroughWorkspaceImport() async throws {
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackCurvePresetRoundTrip-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    let preset = CustomCurvePreset(
        name: "Night Lift",
        parameters: CurvePresetParameters(black: 0.02, midInput: 0.41, midOutput: 0.67, white: 0.98),
        createdAt: Date(timeIntervalSince1970: 1_700_000_000)
    )
    let exportURL = directory.appendingPathComponent("curves.photonstack-curves.json")
    let exporter = PhotonStackWorkspaceModel(
        customCurvePresets: [preset],
        processingService: RecordingProcessingService(),
        settingsRepository: AppSettingsRepository(
            settingsURL: directory.appendingPathComponent("exporter-settings.json")
        )
    )
    exporter.exportCustomCurvePresets(to: exportURL)

    let importer = PhotonStackWorkspaceModel(
        customCurvePresets: [],
        processingService: RecordingProcessingService(),
        settingsRepository: AppSettingsRepository(
            settingsURL: directory.appendingPathComponent("importer-settings.json")
        )
    )
    importer.importCustomCurvePresets(from: exportURL)
    await importer.waitForPendingUserSettingsPersistence()

    #expect(exporter.errorMessage == nil)
    #expect(importer.errorMessage == nil)
    #expect(importer.customCurvePresets.count == 1)
    #expect(importer.customCurvePresets.first?.name == preset.name)
    #expect(importer.customCurvePresets.first?.parameters == preset.parameters)
    #expect(importer.customCurvePresets.first?.id != preset.id)
}

@Test @MainActor func customCurvePresetImportRejectsOversizedFilesAndExcessiveCountsWithoutMutation() async throws {
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackCurvePresetLimits-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    let existingPreset = CustomCurvePreset(
        name: "Existing",
        parameters: CurvePresetParameters(black: 0.02, midInput: 0.4, midOutput: 0.7, white: 0.98)
    )
    let model = PhotonStackWorkspaceModel(
        language: .simplifiedChinese,
        customCurvePresets: [existingPreset],
        processingService: RecordingProcessingService(),
        settingsRepository: AppSettingsRepository(
            settingsURL: directory.appendingPathComponent("settings.json")
        )
    )

    let oversizedURL = directory.appendingPathComponent("oversized.photonstack-curves.json")
    #expect(FileManager.default.createFile(atPath: oversizedURL.path, contents: nil))
    let oversizedHandle = try FileHandle(forWritingTo: oversizedURL)
    try oversizedHandle.truncate(atOffset: PhotonStackWorkspaceModel.maximumCurvePresetImportBytes + 1)
    try oversizedHandle.close()

    model.importCustomCurvePresets(from: oversizedURL)

    #expect(model.customCurvePresets == [existingPreset])
    #expect(model.errorMessage == model.localized(.curvePresetImportTooLarge))

    let excessiveURL = directory.appendingPathComponent("excessive.photonstack-curves.json")
    let excessiveLibrary = CustomCurvePresetLibrary(
        presets: (0...PhotonStackWorkspaceModel.maximumImportedCurvePresetCount).map { index in
            CustomCurvePreset(
                name: "Preset \(index)",
                parameters: CurvePresetParameters(black: 0, midInput: 0.5, midOutput: 0.5, white: 1)
            )
        }
    )
    let encoder = JSONEncoder()
    encoder.dateEncodingStrategy = .iso8601
    try encoder.encode(excessiveLibrary).write(to: excessiveURL, options: .atomic)

    model.importCustomCurvePresets(from: excessiveURL)

    #expect(model.customCurvePresets == [existingPreset])
    #expect(model.errorMessage == model.localized(.curvePresetImportTooMany))

    let validURL = directory.appendingPathComponent("valid.photonstack-curves.json")
    try encoder.encode(CustomCurvePresetLibrary(presets: [
        CustomCurvePreset(
            name: "Recovered",
            parameters: CurvePresetParameters(black: 0, midInput: 0.5, midOutput: 0.5, white: 1)
        )
    ])).write(to: validURL, options: .atomic)

    model.importCustomCurvePresets(from: validURL)
    await model.waitForPendingUserSettingsPersistence()

    #expect(model.errorMessage == nil)
    #expect(model.customCurvePresets.map(\.name) == ["Recovered", "Existing"])
}

@Test @MainActor func oversizedProjectAndSettingsLoadsPreserveCurrentWorkspaceState() async throws {
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackWorkspaceDocumentLimits-\(UUID().uuidString)", isDirectory: true)
    let projectDirectory = directory.appendingPathComponent("oversized-project", isDirectory: true)
    let settingsURL = directory.appendingPathComponent("oversized-settings.json")
    try FileManager.default.createDirectory(at: projectDirectory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }
    try Data(repeating: 0x20, count: 129).write(
        to: projectDirectory.appendingPathComponent(ProjectRepository.projectFileName)
    )
    try Data(repeating: 0x20, count: 65).write(to: settingsURL)

    var project = PhotonStackProject(name: "Current Workspace")
    project.addAssets(from: [URL(fileURLWithPath: "/tmp/current-workspace.nef")])
    let originalProject = project
    let originalOptions = ExportOptions(bitDepth: .sixteen, colorSpace: .linearSRGB, jpegQuality: 0.73)
    let model = PhotonStackWorkspaceModel(
        project: project,
        language: .simplifiedChinese,
        selectedTemplate: .mosaic,
        autosaveEnabled: false,
        exportOptions: originalOptions,
        processingService: RecordingProcessingService(),
        projectRepository: ProjectRepository(maximumDocumentBytes: 128),
        settingsRepository: AppSettingsRepository(
            settingsURL: settingsURL,
            maximumDocumentBytes: 64
        )
    )

    await model.loadProject(from: projectDirectory)

    #expect(model.project == originalProject)
    #expect(model.latestJob?.status == .failed)
    #expect(model.currentProjectDirectory == nil)

    await model.loadUserSettings()

    #expect(model.project == originalProject)
    #expect(model.language == .simplifiedChinese)
    #expect(model.selectedTemplate == .mosaic)
    #expect(model.autosaveEnabled == false)
    #expect(model.exportOptions == originalOptions)
    #expect(model.errorMessage?.contains("64 bytes") == true)
}

@Test @MainActor func settingsLoadMergesUserChangesMadeWhileStorageIsWaiting() async throws {
    let loadedPreset = CustomCurvePreset(
        name: "Loaded Curve",
        parameters: CurvePresetParameters(parameters: ProcessingParameters())
    )
    let loadedSettings = AppSettings(
        recentProjects: [
            RecentProject(
                name: "Loaded Project",
                directory: URL(fileURLWithPath: "/tmp/photonstack-loaded-project")
            ),
        ],
        autosaveEnabled: false,
        selectedTemplate: .mosaic,
        language: .simplifiedChinese,
        customCurvePresets: [loadedPreset],
        exportOptions: ExportOptions(
            bitDepth: .sixteen,
            colorSpace: .linearSRGB,
            jpegQuality: 0.55
        )
    )
    let settingsStore = ControllableAppSettingsStore(
        settings: loadedSettings,
        holdLoad: true
    )
    let model = PhotonStackWorkspaceModel(
        language: .systemDefault,
        processingService: RecordingProcessingService(),
        settingsRepository: settingsStore
    )

    let loadTask = Task { @MainActor in
        await model.loadUserSettings()
    }
    try await waitForSettingsLoadStart(in: settingsStore)

    model.language = .english
    model.setExportJPEGQuality(0.73)
    model.persistUserSettings()
    await settingsStore.releaseLoad()
    await loadTask.value
    await model.waitForPendingUserSettingsPersistence()

    #expect(model.language == .english)
    #expect(model.autosaveEnabled == false)
    #expect(model.selectedTemplate == .mosaic)
    #expect(model.customCurvePresets == [loadedPreset])
    #expect(model.recentProjects.map(\.directory) == loadedSettings.recentProjects.map(\.directory))
    #expect(model.exportOptions.bitDepth == .sixteen)
    #expect(model.exportOptions.colorSpace == .linearSRGB)
    #expect(model.exportOptions.jpegQuality == 0.73)

    let persisted = await settingsStore.currentSettings()
    #expect(persisted.language == .english)
    #expect(persisted.autosaveEnabled == false)
    #expect(persisted.selectedTemplate == .mosaic)
    #expect(persisted.customCurvePresets == [loadedPreset])
    #expect(persisted.exportOptions == model.exportOptions)
}

@Test @MainActor func userSettingsWritesCommitInMutationOrder() async throws {
    let settingsStore = ControllableAppSettingsStore(
        settings: AppSettings(),
        delayFirstSaveNanoseconds: 250_000_000
    )
    let model = PhotonStackWorkspaceModel(
        processingService: RecordingProcessingService(),
        settingsRepository: settingsStore
    )

    model.setExportBitDepth(.sixteen)
    #expect(model.hasPendingUserSettingsPersistence)
    try await waitForSettingsSaveCount(1, in: settingsStore)
    model.setExportColorSpace(.linearSRGB)
    #expect(model.hasPendingUserSettingsPersistence)
    await model.waitForPendingUserSettingsPersistence()

    let persisted = await settingsStore.currentSettings()
    #expect(model.hasPendingUserSettingsPersistence == false)
    #expect(persisted.exportOptions.bitDepth == .sixteen)
    #expect(persisted.exportOptions.colorSpace == .linearSRGB)
    #expect(await settingsStore.saveCount() == 2)
}

@Test @MainActor func userSettingsPersistenceReportsLatestFailureAndClearsAfterRetry() async {
    let settingsStore = ControllableAppSettingsStore(
        settings: AppSettings(),
        failingSaveIndices: [1]
    )
    let model = PhotonStackWorkspaceModel(
        processingService: RecordingProcessingService(),
        settingsRepository: settingsStore
    )

    model.setExportBitDepth(.sixteen)
    await model.waitForPendingUserSettingsPersistence()
    #expect(model.hasPendingUserSettingsPersistence == false)
    #expect(model.errorMessage?.hasPrefix(model.localized(.settingsSaveFailed)) == true)
    #expect(await settingsStore.currentSettings().exportOptions.bitDepth == .automatic)

    model.setExportColorSpace(.linearSRGB)
    await model.waitForPendingUserSettingsPersistence()
    #expect(model.errorMessage == nil)
    let persisted = await settingsStore.currentSettings()
    #expect(persisted.exportOptions.bitDepth == .sixteen)
    #expect(persisted.exportOptions.colorSpace == .linearSRGB)
}

@Test @MainActor func exportUsesCurrentArtifactWhenPreviewHasNoReplayProvenance() async throws {
    let service = RecordingProcessingService()
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackOriginalExportTest-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    let rawURL = directory.appendingPathComponent("DSC_6699.NEF")
    try Data([0]).write(to: rawURL)
    let previewURL = directory.appendingPathComponent("raw-preview.png")
    var project = PhotonStackProject(name: "Original Export Test")
    project.addAssets(from: [rawURL])
    let previewArtifact = ProcessingArtifact(kind: .editedImage, name: "Preview", outputURLs: [previewURL])
    project.appendArtifact(previewArtifact)
    let model = PhotonStackWorkspaceModel(project: project, processingService: service)
    model.previewArtifact(previewArtifact)

    await model.exportCurrentImage(to: directory.appendingPathComponent("final.tiff"))

    #expect(await service.snapshot().convertInputs == [previewURL])
}

@Test @MainActor func processingLayersPersistOperationAndInputProvenance() async throws {
    let service = RecordingProcessingService()
    let input = URL(fileURLWithPath: "/tmp/photonstack-layer-provenance.tiff")
    var project = PhotonStackProject(name: "Layer Provenance Test")
    project.addAssets(from: [input])
    let model = PhotonStackWorkspaceModel(project: project, processingService: service)

    await model.autoStretchPreview()

    let operation = try #require(model.project.editGraph.operations.last)
    let layer = try #require(model.project.layers.last)
    #expect(operation.parameters["input"] == input.path)
    #expect(layer.parameters["operationID"] == operation.id.uuidString)
    #expect(layer.parameters["sourceLayerID"] == model.project.layers.first?.id.uuidString)
}

@Test @MainActor func sourcePreviewEditCreatesDedicatedParentForDuplicateInputLayers() async throws {
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackDuplicateEditParents-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    let input = directory.appendingPathComponent("source.png")
    try writeSolidPNG(to: input, red: 0.2, green: 0.4, blue: 0.8, width: 16, height: 8)
    var project = PhotonStackProject(name: "Duplicate Edit Parents")
    project.addAssets(from: [input])
    let asset = try #require(project.assets.first)
    let firstLayer = ProcessingLayer(name: "First copy", kind: .baseImage, inputURL: input)
    let secondLayer = ProcessingLayer(name: "Second copy", kind: .baseImage, inputURL: input)
    project.appendLayer(firstLayer)
    project.appendLayer(secondLayer)
    project.workspaceState = ProjectWorkspaceState(
        selectedAssetID: asset.id,
        previewURL: input,
        canvasMode: .sourcePreview
    )
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: RecordingProcessingService(),
        previewDirectory: directory.appendingPathComponent("previews", isDirectory: true)
    )

    await model.autoStretchPreview()

    let operation = try #require(model.project.editGraph.operations.last { $0.kind == .stretch })
    let resultLayer = try #require(model.project.layers.first {
        $0.parameters["operationID"] == operation.id.uuidString
    })
    let parentID = try #require(resultLayer.parameters["sourceLayerID"].flatMap(UUID.init(uuidString:)))
    #expect(parentID != firstLayer.id)
    #expect(parentID != secondLayer.id)
    let parent = try #require(model.project.layers.first { $0.id == parentID })
    #expect(parent.inputURL == input)
    #expect(parent.parameters["source"] == "automatic")
}

@Test @MainActor func sourcePreviewStarMaskCreatesDedicatedTargetForDuplicateInputLayers() async throws {
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackDuplicateMaskTargets-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    let input = directory.appendingPathComponent("source.png")
    try writeSolidPNG(to: input, red: 0.8, green: 0.8, blue: 0.8, width: 32, height: 16)
    var project = PhotonStackProject(name: "Duplicate Mask Targets")
    project.addAssets(from: [input])
    let asset = try #require(project.assets.first)
    let firstLayer = ProcessingLayer(name: "First copy", kind: .baseImage, inputURL: input)
    let secondLayer = ProcessingLayer(name: "Second copy", kind: .baseImage, inputURL: input)
    project.appendLayer(firstLayer)
    project.appendLayer(secondLayer)
    project.workspaceState = ProjectWorkspaceState(
        selectedAssetID: asset.id,
        previewURL: input,
        canvasMode: .sourcePreview
    )
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: RecordingProcessingService(),
        previewDirectory: directory.appendingPathComponent("previews", isDirectory: true)
    )

    await model.createStarMask(
        radius: 2,
        largeRadius: 4,
        layered: true,
        sigmaThreshold: 3,
        minPeak: 0.02
    )

    let maskedLayers = model.project.layers.filter { $0.kind != .mask && $0.maskArtifactID != nil }
    let maskedLayer = try #require(maskedLayers.first)
    #expect(maskedLayers.count == 1)
    #expect(maskedLayer.id != firstLayer.id)
    #expect(maskedLayer.id != secondLayer.id)
    #expect(maskedLayer.inputURL == input)
    #expect(model.project.layers.first(where: { $0.id == firstLayer.id })?.maskArtifactID == nil)
    #expect(model.project.layers.first(where: { $0.id == secondLayer.id })?.maskArtifactID == nil)
}

@Test @MainActor func previewArtifactDoesNotGuessBetweenOperationsSharingItsOutput() async throws {
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackAmbiguousArtifactPreview-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    let input = directory.appendingPathComponent("input.png")
    let sharedOutput = directory.appendingPathComponent("shared.png")
    try writeSolidPNG(to: input, red: 0.2, green: 0.3, blue: 0.4, width: 16, height: 8)
    try writeSolidPNG(to: sharedOutput, red: 0.5, green: 0.5, blue: 0.5, width: 16, height: 8)
    let artifact = ProcessingArtifact(kind: .editedImage, name: "Shared output", outputURLs: [sharedOutput])
    var project = PhotonStackProject(name: "Ambiguous Artifact Preview")
    project.addAssets(from: [input])
    project.appendArtifact(artifact)
    project.appendOperation(EditOperation(
        kind: .stretch,
        parameters: ["input": input.path, "output": sharedOutput.path]
    ))
    project.appendOperation(EditOperation(
        kind: .denoise,
        parameters: ["input": input.path, "output": sharedOutput.path]
    ))
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: RecordingProcessingService()
    )

    model.previewArtifact(artifact)

    #expect(model.previewURL == sharedOutput)
    #expect(model.previewOperationID == nil)
    #expect(model.canExportCurrentImage == false)
}

@Test @MainActor func rawAssetLayerUsesPhotonStackDecodeBeforeExport() async throws {
    let service = RecordingProcessingService()
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackRawLayerExportTest-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    let rawURL = directory.appendingPathComponent("DSC_6699.NEF")
    try Data([0]).write(to: rawURL)
    var project = PhotonStackProject(name: "RAW Layer Export Test")
    project.addAssets(from: [rawURL])
    let asset = try #require(project.assets.first)
    project.appendLayer(
        ProcessingLayer(
            name: asset.displayName,
            kind: .baseImage,
            inputURL: rawURL,
            parameters: ["source": "asset", "assetID": asset.id.uuidString]
        )
    )
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: service,
        previewDirectory: directory.appendingPathComponent("previews", isDirectory: true)
    )

    await model.exportCurrentImage(to: directory.appendingPathComponent("final.tiff"))

    let snapshot = await service.snapshot()
    #expect(snapshot.convertInputs.count == 2)
    #expect(snapshot.convertInputs.first == rawURL)
    let decodedOutput = try #require(snapshot.convertOutputs.first)
    #expect(decodedOutput.pathExtension == "fits")
    #expect(snapshot.convertOptions.first?.fitsValueMode == .scientific)
    #expect(snapshot.convertInputs.last == decodedOutput)
}

@Test @MainActor func rawLayerCompositeCreatesOneExplicitDisplayBridge() async throws {
    let service = RecordingProcessingService()
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackRawLayerCompositeTest-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer { try? FileManager.default.removeItem(at: directory) }

    let rawURL = directory.appendingPathComponent("DSC_6699.NEF")
    try writeSolidPNG(to: rawURL, red: 0.2, green: 0.3, blue: 0.4, width: 16, height: 8)
    var project = PhotonStackProject(name: "RAW Layer Composite Test")
    project.addAssets(from: [rawURL])
    let asset = try #require(project.assets.first)
    let parameters = ["source": "asset", "assetID": asset.id.uuidString]
    project.appendLayer(ProcessingLayer(name: "RAW Base", kind: .baseImage, inputURL: rawURL, parameters: parameters))
    project.appendLayer(ProcessingLayer(name: "RAW Copy", kind: .baseImage, inputURL: rawURL, opacity: 0.5, parameters: parameters))
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: service,
        previewDirectory: directory.appendingPathComponent("previews", isDirectory: true)
    )

    await model.exportCurrentImage(to: directory.appendingPathComponent("final.tiff"))

    let snapshot = await service.snapshot()
    #expect(snapshot.convertInputs.count == 3)
    let scientific = try #require(snapshot.convertOutputs.first)
    let compositingInput = try #require(snapshot.convertOutputs.dropFirst().first)
    #expect(scientific.pathExtension == "fits")
    #expect(compositingInput.pathExtension == "tiff")
    #expect(snapshot.convertInputs[1] == scientific)
    #expect(snapshot.convertOptions[0].fitsValueMode == .scientific)
    #expect(snapshot.convertOptions[1].fitsValueMode == .display)
    #expect(snapshot.convertOptions[1].colorSpace == .linearSRGB)
}

@Test @MainActor func customLayerCompositeReplaysLegacyLayerAtFullResolution() async throws {
    let service = RecordingProcessingService()
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackFullResolutionLayerTest-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    let originalURL = directory.appendingPathComponent("original.png")
    let proxyURL = directory.appendingPathComponent("proxy.png")
    try writeSolidPNG(to: originalURL, red: 1, green: 0, blue: 0, width: 16, height: 8)
    try writeSolidPNG(to: proxyURL, red: 0, green: 0, blue: 1, width: 4, height: 2)
    var project = PhotonStackProject(name: "Full Resolution Layer Test")
    project.addAssets(from: [originalURL])
    let asset = try #require(project.assets.first)
    let baseLayer = ProcessingLayer(
        name: "Original",
        kind: .baseImage,
        inputURL: originalURL,
        parameters: ["source": "asset", "assetID": asset.id.uuidString]
    )
    let operation = EditOperation(
        kind: .stretch,
        parameters: ["input": originalURL.path, "output": proxyURL.path, "targetBackground": "0.16"]
    )
    let adjustmentLayer = ProcessingLayer(
        name: "Legacy Stretch",
        kind: .adjustment,
        inputURL: proxyURL,
        opacity: 0.5,
        parameters: [
            "source": "operation",
            "sourceLayerID": baseLayer.id.uuidString,
            "operation": ProcessingOperationKind.stretch.rawValue,
        ]
    )
    project.appendOperation(operation)
    project.appendLayer(baseLayer)
    project.appendLayer(adjustmentLayer)
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: service,
        previewDirectory: directory.appendingPathComponent("previews", isDirectory: true)
    )

    await model.exportCurrentImage(to: directory.appendingPathComponent("final.tiff"))

    let snapshot = await service.snapshot()
    #expect(snapshot.autoStretchInputs == [originalURL])
    #expect(snapshot.autoStretchOutputs.first?.pathExtension == "tiff")
    let compositeURL = try #require(snapshot.convertInput)
    let leftPixel = try rgbaPixel(in: compositeURL, x: 1, y: 1)
    let farPixel = try rgbaPixel(in: compositeURL, x: 12, y: 6)
    #expect(leftPixel.red > 240 && leftPixel.blue < 15)
    #expect(farPixel.red > 240 && farPixel.blue < 15)
}

@Test @MainActor func customLayerExportRegeneratesStarMaskFromFullResolutionLayer() async throws {
    let service = RecordingProcessingService()
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackFullResolutionMaskTest-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    let originalURL = directory.appendingPathComponent("stars.png")
    try writeSolidPNG(to: originalURL, red: 1, green: 1, blue: 1, width: 120, height: 60)
    var project = PhotonStackProject(name: "Full Resolution Mask Test")
    project.addAssets(from: [originalURL])
    let asset = try #require(project.assets.first)
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: service,
        previewDirectory: directory.appendingPathComponent("previews", isDirectory: true)
    )
    model.createLayer(from: asset)
    await model.createStarMask(radius: 2, largeRadius: 5, layered: true, sigmaThreshold: 3, minPeak: 0.02)

    let sourceLayer = try #require(model.project.layers.first(where: { $0.kind != .mask }))
    #expect(model.applyMaskBrushEdits(
        [MaskBrushStroke(
            mode: .hide,
            points: [MaskBrushPoint(x: 0.5, y: 0.25)],
            diameterFraction: 0.2
        )],
        to: sourceLayer.id
    ))

    await model.exportCurrentImage(to: directory.appendingPathComponent("final.tiff"))

    let snapshot = await service.snapshot()
    #expect(snapshot.starMaskInputs == [originalURL, originalURL])
    #expect(snapshot.starMaskOutputs.count == 2)
    #expect(snapshot.starMaskOutputs.last?.pathExtension == "tiff")
    let compositeURL = try #require(snapshot.convertInput)
    let composite = try #require(CIImage(contentsOf: compositeURL))
    #expect(composite.extent.width == 120)
    #expect(composite.extent.height == 60)
    #expect(try rgbaPixel(in: compositeURL, x: 60, y: 45).alpha < 15)
    #expect(try rgbaPixel(in: compositeURL, x: 60, y: 15).alpha > 240)
}

@Test @MainActor func fullResolutionMaskClampsExtremePersistedRadiiBeforeIntegerConversion() async throws {
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackExtremeMaskRadius-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    let originalURL = directory.appendingPathComponent("original.png")
    let previewMaskURL = directory.appendingPathComponent("preview-mask.png")
    try writeSolidPNG(to: originalURL, red: 0.8, green: 0.8, blue: 0.8, width: 120, height: 60)
    try writeSolidPNG(to: previewMaskURL, red: 1, green: 1, blue: 1, width: 16, height: 8)
    var project = PhotonStackProject(name: "Extreme Mask Radius")
    project.addAssets(from: [originalURL])
    let asset = try #require(project.assets.first)
    let mask = ProcessingArtifact(
        kind: .mask,
        name: "Extreme Star Mask",
        operationKind: .starMask,
        outputURLs: [previewMaskURL],
        parameters: [
            "radius": String(Int.max),
            "largeRadius": String(Int.max),
            "sigmaThreshold": "3",
            "minPeak": "0.02",
        ]
    )
    project.appendArtifact(mask)
    project.appendLayer(
        ProcessingLayer(
            name: "Masked Source",
            kind: .baseImage,
            inputURL: originalURL,
            maskArtifactID: mask.id,
            parameters: [
                "source": "asset",
                "assetID": asset.id.uuidString,
            ]
        )
    )
    let service = RecordingProcessingService()
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: service,
        previewDirectory: directory.appendingPathComponent("previews", isDirectory: true)
    )

    await model.exportCurrentImage(to: directory.appendingPathComponent("final.tiff"))

    let snapshot = await service.snapshot()
    #expect(snapshot.starMaskInputs == [originalURL])
    #expect(snapshot.starMaskRadius == 32)
    #expect(snapshot.convertInput != nil)
    #expect(model.latestJob?.status == .succeeded)
}

@Test @MainActor func sharedMaskIsRegeneratedPerFullResolutionLayer() async throws {
    let service = RecordingProcessingService()
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackSharedMaskExportTest-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    let whiteURL = directory.appendingPathComponent("white.png")
    let blackURL = directory.appendingPathComponent("black.png")
    let previewMaskURL = directory.appendingPathComponent("preview-mask.png")
    try writeSolidPNG(to: whiteURL, red: 1, green: 1, blue: 1, width: 10, height: 6)
    try writeSolidPNG(to: blackURL, red: 0, green: 0, blue: 0, width: 10, height: 6)
    try writeSolidPNG(to: previewMaskURL, red: 1, green: 1, blue: 1, width: 2, height: 2)
    let maskArtifact = ProcessingArtifact(
        kind: .mask,
        name: "Shared Mask",
        operationKind: .starMask,
        outputURLs: [previewMaskURL],
        parameters: ["radius": "2", "largeRadius": "4", "layered": "true"]
    )
    let whiteLayer = ProcessingLayer(
        name: "White",
        kind: .baseImage,
        inputURL: whiteURL,
        maskArtifactID: maskArtifact.id
    )
    let blackLayer = ProcessingLayer(
        name: "Black",
        kind: .baseImage,
        inputURL: blackURL,
        maskArtifactID: maskArtifact.id
    )
    var project = PhotonStackProject(name: "Shared Mask Export Test")
    project.addAssets(from: [whiteURL, blackURL])
    project.appendArtifact(maskArtifact)
    project.appendLayer(whiteLayer)
    project.appendLayer(blackLayer)
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: service,
        previewDirectory: directory.appendingPathComponent("previews", isDirectory: true)
    )

    await model.exportCurrentImage(to: directory.appendingPathComponent("final.tiff"))

    let snapshot = await service.snapshot()
    #expect(snapshot.starMaskInputs == [whiteURL, blackURL])
    #expect(snapshot.starMaskRadius == 10)
    #expect(snapshot.largeStarMaskRadius == 20)
    let compositeURL = try #require(snapshot.convertInput)
    let pixel = try rgbaPixel(in: compositeURL, x: 5, y: 3)
    #expect(pixel.red > 240 && pixel.green > 240 && pixel.blue > 240 && pixel.alpha > 240)
}

@Test @MainActor func previewStepBackRestoresProcessingSourceLayersAndEditGraph() async throws {
    let service = RecordingProcessingService()
    let input = URL(fileURLWithPath: "/tmp/photonstack-preview-history.tiff")
    var project = PhotonStackProject(name: "Preview History Test")
    project.addAssets(from: [input])
    let model = PhotonStackWorkspaceModel(project: project, processingService: service)

    await model.autoStretchPreview()
    let stretchedLayer = try #require(model.activeLayer)
    #expect(stretchedLayer.kind == .adjustment)
    #expect(model.project.editGraph.operations.map(\.kind) == [.stretch])
    #expect(model.currentInputURL == stretchedLayer.inputURL)

    model.stepBackPreview()
    #expect(model.previewURL == input)
    #expect(model.currentInputURL == input)
    #expect(model.activeLayerID == nil)
    #expect(model.project.layers.isEmpty)
    #expect(model.project.editGraph.operations.isEmpty)

    await model.denoisePreview()
    #expect(await service.snapshot().denoiseInput == input)
}

@Test @MainActor func previewStepBackFallsBackWhenHistoricalPreviewWasDeleted() async throws {
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackMissingPreviewHistory-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    let input = directory.appendingPathComponent("input.png")
    try writeSolidPNG(to: input, red: 0.2, green: 0.6, blue: 0.8, width: 16, height: 8)
    var project = PhotonStackProject(name: "Missing Preview History")
    project.addAssets(from: [input])
    let service = RecordingProcessingService(requiresExistingInputFiles: true)
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: service,
        previewDirectory: directory.appendingPathComponent("previews", isDirectory: true)
    )

    await model.autoStretchPreview()
    let stretchedPreview = try #require(model.previewURL)
    #expect(FileManager.default.fileExists(atPath: stretchedPreview.path))
    await model.denoisePreview()
    #expect(model.previewURL != stretchedPreview)

    try FileManager.default.removeItem(at: stretchedPreview)
    model.stepBackPreview()

    #expect(model.previewURL == input)
    #expect(model.activeLayerID == nil)
    #expect(model.project.editGraph.operations.map(\.kind) == [.stretch])
    #expect(model.editGraphNeedsReplay)
    #expect(model.errorMessage?.contains(model.localized(.missingWorkspaceSourceError)) == true)
}

@Test @MainActor func resetToOriginalCanRestoreProcessedLayerState() async throws {
    let service = RecordingProcessingService()
    let input = URL(fileURLWithPath: "/tmp/photonstack-reset-history.tiff")
    var project = PhotonStackProject(name: "Reset History Test")
    project.addAssets(from: [input])
    let model = PhotonStackWorkspaceModel(project: project, processingService: service)

    await model.autoStretchPreview()
    let processedPreview = try #require(model.previewURL)
    let processedLayerID = try #require(model.activeLayerID)

    model.resetPreviewToOriginal()
    #expect(model.previewURL == input)
    #expect(model.currentInputURL == input)
    #expect(model.activeLayer?.inputURL == input)
    #expect(model.project.layers.first(where: { $0.id == processedLayerID })?.isVisible == false)
    #expect(model.canStepBack)

    model.stepBackPreview()
    #expect(model.previewURL == processedPreview)
    #expect(model.activeLayerID == processedLayerID)
    #expect(model.project.layers.first(where: { $0.id == processedLayerID })?.isVisible == true)
}

@Test @MainActor func rawParameterChangePersistsRawDecodeOperation() async throws {
    let service = RecordingProcessingService()
    let raw = URL(fileURLWithPath: "/tmp/photonstack-light.nef")
    var project = PhotonStackProject(name: "RAW Test")
    project.addAssets(from: [raw])

    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: service
    )

    model.setRawExposureBias(0.75)
    model.setRawWhiteBalanceMode(.auto)

    let operation = try #require(model.project.editGraph.operations.first { $0.kind == .rawDecode })
    #expect(operation.parameters["rawExposureBias"] == "0.75")
    #expect(operation.parameters["rawWhiteBalance"] == "auto")
    #expect(operation.parameters["rawLinear"] == "true")

    model.cancelCurrentTask()
}

@Test @MainActor func rawAutoPreviewDoesNotApplyLinearOutputAsDisplayEnhancement() async throws {
    let service = RecordingProcessingService()
    let raw = URL(fileURLWithPath: "/tmp/photonstack-visual-raw.nef")
    var project = PhotonStackProject(name: "RAW Visual Preview Test")
    project.addAssets(from: [raw])
    let previewDirectory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackRawVisualPreviewTest-\(UUID().uuidString)", isDirectory: true)
    defer {
        try? FileManager.default.removeItem(at: previewDirectory)
    }

    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: service,
        previewDirectory: previewDirectory
    )

    model.setRawExposureBias(1.25)
    let options = try await waitForRawAdjustOptions(from: service)
    #expect(options.exposureBias == 1.25)
    #expect(options.whiteBalanceMode == .camera)
    #expect(options.blackLevelMode == .camera)
    #expect(options.linearOutput == false)

    let operation = try #require(model.project.editGraph.operations.first { $0.kind == .rawDecode })
    #expect(operation.parameters["rawLinear"] == "true")
}

@Test @MainActor func rawPreviewSurfacesAndRestoresDecoderFallbackFromCache() async throws {
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackRawDecoderStatus-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer { try? FileManager.default.removeItem(at: directory) }

    let png = directory.appendingPathComponent("source.png")
    let raw = directory.appendingPathComponent("source.nef")
    try writeSolidPNG(to: png, red: 0.2, green: 0.3, blue: 0.4)
    try Data(contentsOf: png).write(to: raw)
    let previewReport = #"{"type":"complete","command":"preview","decodeBackend":"imageio","decodeFallback":true,"decodeFallbackCode":"RawDecodeFailed","decodeFallbackMessage":"Core Image unavailable"}"#
    let service = RecordingProcessingService(previewOutputOverride: previewReport)
    var project = PhotonStackProject(name: "RAW Decoder Status")
    project.addAssets(from: [raw])
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: service,
        previewDirectory: directory.appendingPathComponent("previews", isDirectory: true)
    )

    model.setRawExposureBias(0.5)
    try await waitForRawPreviewCompletion(in: model)
    let expected = RawDecodeStatus(
        backend: .imageIO,
        usedFallback: true,
        fallbackErrorCode: "RawDecodeFailed",
        fallbackMessage: "Core Image unavailable"
    )
    #expect(model.rawDecodeStatus == expected)
    #expect(await service.snapshot().makePreviewCalls == 1)

    model.cancelCurrentTask()
    #expect(model.rawDecodeStatus == nil)
    model.setRawExposureBias(1.0)
    try await waitForRawPreviewCompletion(in: model)
    #expect(model.rawDecodeStatus == expected)
    #expect(await service.snapshot().makePreviewCalls == 1)
}

@Test @MainActor func rawPreviewRejectsContradictoryDecoderFallbackReport() async throws {
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackRawDecoderInvalid-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer { try? FileManager.default.removeItem(at: directory) }

    let png = directory.appendingPathComponent("source.png")
    let raw = directory.appendingPathComponent("source.nef")
    try writeSolidPNG(to: png, red: 0.2, green: 0.3, blue: 0.4)
    try Data(contentsOf: png).write(to: raw)
    let invalidReport = #"{"type":"complete","command":"preview","decodeBackend":"apple-raw","decodeFallback":true,"decodeFallbackCode":"RawDecodeFailed","decodeFallbackMessage":"contradiction"}"#
    let service = RecordingProcessingService(previewOutputOverride: invalidReport)
    var project = PhotonStackProject(name: "RAW Decoder Invalid")
    project.addAssets(from: [raw])
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: service,
        previewDirectory: directory.appendingPathComponent("previews", isDirectory: true)
    )

    model.setRawExposureBias(0.5)
    try await waitForRawPreviewCompletion(in: model)
    #expect(model.rawDecodeStatus == nil)
    #expect(model.errorMessage?.contains("preview") == true)
}

@Test @MainActor func rawLayerExportUsesPersistedDecodeOptionsAfterWorkspaceRestoration() async throws {
    let service = RecordingProcessingService()
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackPersistedRawExport-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    let rawURL = directory.appendingPathComponent("persisted.nef")
    try Data([0]).write(to: rawURL)
    var project = PhotonStackProject(name: "Persisted RAW Export")
    project.addAssets(from: [rawURL])
    let asset = try #require(project.assets.first)
    project.appendOperation(
        EditOperation(
            kind: .rawDecode,
            parameters: [
                "assetID": asset.id.uuidString,
                "rawWhiteBalance": RawWhiteBalanceMode.auto.rawValue,
                "rawExposureBias": "1.25",
                "rawBlackLevel": RawBlackLevelMode.auto.rawValue,
                "rawDemosaic": RawDemosaicQuality.high.rawValue,
                "rawLinear": "false",
            ]
        )
    )
    project.appendLayer(
        ProcessingLayer(
            name: asset.displayName,
            kind: .baseImage,
            inputURL: rawURL,
            parameters: ["source": "asset", "assetID": asset.id.uuidString]
        )
    )
    let repository = ProjectRepository()
    let projectDirectory = directory.appendingPathComponent("project", isDirectory: true)
    try await repository.save(project, to: projectDirectory)

    let model = PhotonStackWorkspaceModel(
        processingService: service,
        projectRepository: repository,
        previewDirectory: directory.appendingPathComponent("previews", isDirectory: true)
    )
    await model.loadProject(from: projectDirectory)

    #expect(model.parameters.rawProcessingOptions.whiteBalanceMode == .auto)
    #expect(model.parameters.rawProcessingOptions.exposureBias == 1.25)
    #expect(model.parameters.rawProcessingOptions.blackLevelMode == .auto)
    #expect(model.parameters.rawProcessingOptions.demosaicQuality == .high)
    #expect(model.parameters.rawProcessingOptions.linearOutput == false)
    await model.exportCurrentImage(to: directory.appendingPathComponent("final.tiff"))

    let decodeOptions = try #require(await service.snapshot().convertRawOptions.first)
    #expect(decodeOptions.whiteBalanceMode == .auto)
    #expect(decodeOptions.exposureBias == 1.25)
    #expect(decodeOptions.blackLevelMode == .auto)
    #expect(decodeOptions.demosaicQuality == .high)
    #expect(decodeOptions.linearOutput == false)
}

@Test @MainActor func rawPreviewCannotCompleteIntoAnotherSelectedAsset() async throws {
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackRawSelectionIsolation-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    let firstURL = directory.appendingPathComponent("first.nef")
    let secondURL = directory.appendingPathComponent("second.nef")
    try Data([1]).write(to: firstURL)
    try Data([2]).write(to: secondURL)
    var project = PhotonStackProject(name: "RAW Selection Isolation")
    project.addAssets(from: [firstURL, secondURL])
    let firstAsset = try #require(project.assets.first { $0.originalURL == firstURL })
    let secondAsset = try #require(project.assets.first { $0.originalURL == secondURL })
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: RecordingProcessingService(),
        previewDirectory: directory.appendingPathComponent("previews", isDirectory: true)
    )

    model.select(firstAsset)
    model.setRawExposureBias(0.75)
    model.select(secondAsset)
    try await Task.sleep(nanoseconds: 500_000_000)

    #expect(model.selectedAssetID == secondAsset.id)
    #expect(model.previewURL == secondURL)
    let firstOperation = try #require(model.project.editGraph.operations.first {
        $0.kind == .rawDecode && $0.parameters["assetID"] == firstAsset.id.uuidString
    })
    #expect(firstOperation.parameters["rawExposureBias"] == "0.75")
    #expect(model.project.editGraph.operations.contains {
        $0.kind == .rawDecode && $0.parameters["assetID"] == secondAsset.id.uuidString
    } == false)

    let projectDirectory = directory.appendingPathComponent("project", isDirectory: true)
    await model.saveProject(to: projectDirectory)
    let restoredModel = PhotonStackWorkspaceModel(
        processingService: RecordingProcessingService(),
        previewDirectory: directory.appendingPathComponent("restored-previews", isDirectory: true)
    )
    await restoredModel.loadProject(from: projectDirectory)
    #expect(restoredModel.selectedAssetID == secondAsset.id)
    #expect(restoredModel.previewURL == secondURL)
    #expect(restoredModel.parameters.rawProcessingOptions.exposureBias == 0.75)

    model.setRawExposureBias(1.25)
    model.cancelCurrentTask()
    let secondOperation = try #require(model.project.editGraph.operations.first {
        $0.kind == .rawDecode && $0.parameters["assetID"] == secondAsset.id.uuidString
    })
    #expect(secondOperation.parameters["rawExposureBias"] == "1.25")
    #expect(model.project.editGraph.operations.filter { $0.kind == .rawDecode }.count == 2)
    #expect(model.project.editGraph.operations.filter { $0.kind == .rawDecode }.allSatisfy {
        $0.parameters["rawExposureBias"] == "1.25"
    })

    model.select(firstAsset)
    #expect(model.parameters.rawProcessingOptions.exposureBias == 1.25)
}

@Test @MainActor func relinkingRawAssetRejectsLatePreviewFromOldPath() async throws {
    let service = RecordingProcessingService(
        rawAdjustDelayNanoseconds: 300_000_000,
        ignoreRawAdjustCancellation: true
    )
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackRawRelinkIsolation-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    let originalURL = directory.appendingPathComponent("original.nef")
    let replacementURL = directory.appendingPathComponent("replacement.nef")
    try Data([1]).write(to: originalURL)
    try Data([2]).write(to: replacementURL)
    var project = PhotonStackProject(name: "RAW Relink Isolation")
    project.addAssets(from: [originalURL])
    let asset = try #require(project.assets.first)
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: service,
        previewDirectory: directory.appendingPathComponent("previews", isDirectory: true)
    )

    model.setRawExposureBias(0.75)
    _ = try await waitForRawAdjustOptions(from: service)
    #expect(model.isRawPreviewUpdating)
    #expect(model.relinkAsset(asset, to: replacementURL))
    #expect(model.isRawPreviewUpdating == false)
    #expect(model.previewURL == replacementURL)

    try await Task.sleep(nanoseconds: 350_000_000)
    #expect(model.selectedAsset?.originalURL == replacementURL)
    #expect(model.previewURL == replacementURL)
    #expect(model.project.editGraph.operations.first {
        $0.kind == .rawDecode && $0.parameters["assetID"] == asset.id.uuidString
    }?.parameters["output"] == nil)
}

@Test @MainActor func rawPreviewFreezesWorkspaceProductsButKeepsRawControlsResponsive() async throws {
    let service = RecordingProcessingService(
        rawAdjustDelayNanoseconds: 300_000_000,
        ignoreRawAdjustCancellation: true
    )
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackRawWorkspaceGate-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    let rawURL = directory.appendingPathComponent("source.nef")
    let bottomURL = directory.appendingPathComponent("bottom.png")
    let topURL = directory.appendingPathComponent("top.png")
    let artifactURL = directory.appendingPathComponent("object.png")
    try Data([1]).write(to: rawURL)
    try writeSolidPNG(to: bottomURL, red: 0.2, green: 0.3, blue: 0.4, width: 8, height: 6)
    try writeSolidPNG(to: topURL, red: 0.6, green: 0.5, blue: 0.4, width: 8, height: 6)
    try writeSolidPNG(to: artifactURL, red: 0.3, green: 0.6, blue: 0.2, width: 8, height: 6)

    var project = PhotonStackProject(name: "RAW Workspace Gate")
    project.addAssets(from: [rawURL])
    let artifact = ProcessingArtifact(kind: .editedImage, name: "Object", outputURLs: [artifactURL])
    let bottomLayer = ProcessingLayer(name: "Bottom", kind: .baseImage, inputURL: bottomURL)
    let topLayer = ProcessingLayer(name: "Top", kind: .baseImage, inputURL: topURL)
    let operation = EditOperation(
        kind: .stretch,
        parameters: ["input": rawURL.path, "output": artifactURL.path]
    )
    project.appendArtifact(artifact)
    project.appendLayer(bottomLayer)
    project.appendLayer(topLayer)
    project.appendOperation(operation)
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: service,
        previewDirectory: directory.appendingPathComponent("previews", isDirectory: true)
    )

    model.setRawExposureBias(0.5)
    try await waitForRawAdjustCall(1, from: service)
    #expect(model.canModifyProcessingConfiguration)
    #expect(model.canModifyWorkspaceProducts == false)

    let projectSnapshot = model.project
    let previewSnapshot = model.previewURL
    let activeLayerSnapshot = model.activeLayerID
    model.previewArtifact(artifact)
    model.createLayer(from: artifact)
    model.selectLayer(topLayer)
    model.setLayerVisibility(topLayer.id, isVisible: false)
    model.setLayerOpacity(topLayer.id, opacity: 0.25)
    model.setLayerBlendMode(topLayer.id, blendMode: .screen)
    model.moveProcessingLayer(topLayer.id, direction: .down)
    model.deleteProcessingLayer(topLayer.id)
    model.requestArtifactRemoval(artifact)
    model.confirmArtifactRemoval(artifact)
    model.toggleEditOperation(operation.id)
    model.updateEditOperationParameter(operation.id, key: "targetBackground", value: "0.4")
    model.deleteEditOperation(operation.id)
    model.resetPreviewToOriginal()
    model.stepBackPreview()

    #expect(model.project == projectSnapshot)
    #expect(model.previewURL == previewSnapshot)
    #expect(model.activeLayerID == activeLayerSnapshot)
    #expect(model.pendingArtifactRemovalID == nil)

    model.setRawExposureBias(1.0)
    #expect(model.parameters.rawProcessingOptions.exposureBias == 1.0)
    try await waitForRawAdjustCall(2, from: service)
    #expect(await service.snapshot().rawAdjustOptionHistory.map(\.exposureBias) == [0.5, 1.0])
    try await waitForRawAdjustCompletion(2, from: service)
    try await waitForRawPreviewCompletion(in: model)
    #expect(model.canModifyWorkspaceProducts)
    #expect(model.project.editGraph.operations.first {
        $0.kind == .rawDecode
    }?.parameters["rawExposureBias"] == "1.0")
}

@Test @MainActor func importedAssetMetadataRefreshesAutomatically() async throws {
    let service = RecordingProcessingService()
    let raw = URL(fileURLWithPath: "/tmp/photonstack-auto-metadata.nef")
    let model = PhotonStackWorkspaceModel(
        project: PhotonStackProject(name: "Auto Metadata"),
        processingService: service
    )

    model.importAssets(from: [raw])

    let metadata = try await waitForSelectedMetadata(in: model)
    #expect(metadata.cameraModel == "Z6_3")
    #expect(metadata.lensModel == "Viltrox AF 16/1.8 Z")
    #expect(metadata.exposureTimeSeconds == 25)
    #expect(metadata.fNumber == 1.8)
    #expect(metadata.iso == 1600)
    #expect(metadata.whiteBalance == "manual")
    #expect(metadata.exposureBias == "-0.67 EV")
}

@Test @MainActor func manualInspectRejectsMalformedSuccessReport() async throws {
    let service = RecordingProcessingService(
        inspectOutputOverride: #"{"width":8}"#
    )
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackMalformedInspect-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    let input = directory.appendingPathComponent("input.png")
    try writeSolidPNG(to: input, red: 0.1, green: 0.2, blue: 0.3, width: 8, height: 4)
    var project = PhotonStackProject(name: "Malformed Inspect")
    project.addAssets(from: [input])
    let model = PhotonStackWorkspaceModel(project: project, processingService: service)

    await model.inspectSelectedAsset()

    #expect(model.latestJob?.status == .failed)
    #expect(model.selectedAsset?.metadata == nil)
    #expect(model.errorMessage?.contains("inspect") == true)
}

@Test @MainActor func manualInspectRejectsInvalidOptionalMetadataNumbers() async throws {
    let reports = [
        #"{"width":8,"height":4,"channels":4,"bitsPerChannel":16,"format":"RAW","metadata":{"exposureTimeSeconds":-1}}"#,
        #"{"width":8,"height":4,"channels":4,"bitsPerChannel":16,"format":"RAW","metadata":{"iso":true}}"#,
        #"{"width":8,"height":4,"channels":4,"bitsPerChannel":16,"format":"RAW","metadata":{"orientation":9}}"#,
    ]
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackInvalidInspectMetadata-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }
    let input = directory.appendingPathComponent("input.png")
    try writeSolidPNG(to: input, red: 0.1, green: 0.2, blue: 0.3, width: 8, height: 4)

    for report in reports {
        let service = RecordingProcessingService(inspectOutputOverride: report)
        var project = PhotonStackProject(name: "Invalid Inspect Metadata")
        project.addAssets(from: [input])
        let model = PhotonStackWorkspaceModel(project: project, processingService: service)

        await model.inspectSelectedAsset()

        #expect(model.latestJob?.status == .failed)
        #expect(model.selectedAsset?.metadata == nil)
        #expect(model.errorMessage?.contains("inspect") == true)
    }
}

@Test @MainActor func lateMetadataRefreshCannotDirtyReplacementProject() async throws {
    let service = RecordingProcessingService(
        inspectDelayNanoseconds: 300_000_000,
        ignoreInspectCancellation: true
    )
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackMetadataProjectIsolation-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    let oldURL = directory.appendingPathComponent("old.nef")
    let newURL = directory.appendingPathComponent("new.nef")
    try Data([1]).write(to: oldURL)
    try Data([2]).write(to: newURL)
    var oldProject = PhotonStackProject(name: "Old Metadata Project")
    oldProject.addAssets(from: [oldURL])
    let oldAsset = try #require(oldProject.assets.first)
    var newProject = PhotonStackProject(name: "Replacement Metadata Project")
    newProject.addAssets(from: [newURL])
    let projectDirectory = directory.appendingPathComponent("replacement-project", isDirectory: true)
    try await ProjectRepository().save(newProject, to: projectDirectory)

    let model = PhotonStackWorkspaceModel(
        project: oldProject,
        autosaveEnabled: false,
        processingService: service
    )
    model.select(oldAsset)
    try await waitForInspectCall(from: service)
    await model.loadProject(from: projectDirectory)
    #expect(model.project.id == newProject.id)
    #expect(model.hasUnsavedChanges == false)

    try await Task.sleep(nanoseconds: 350_000_000)
    #expect(model.project.id == newProject.id)
    #expect(model.project.assets.first?.metadata == nil)
    #expect(model.hasUnsavedChanges == false)
}

@Test @MainActor func createProjectUsesTemplateAndCustomName() async throws {
    let service = RecordingProcessingService()
    let image = URL(fileURLWithPath: "/tmp/photonstack-existing.tiff")
    var project = PhotonStackProject(name: "Existing")
    project.addAssets(from: [image])

    let model = PhotonStackWorkspaceModel(
        project: project,
        selectedTemplate: .deepSky,
        processingService: service
    )

    await model.detectArtifactTrails()
    #expect(model.artifactTrailReport?.trails == 2)

    model.createProject(template: .mosaic, name: "  Summer Milky Way  ")

    #expect(model.project.name == "Summer Milky Way")
    #expect(model.project.template == .mosaic)
    #expect(model.selectedTemplate == .mosaic)
    #expect(model.project.assets.isEmpty)
    #expect(model.selectedAsset == nil)
    #expect(model.project.editGraph.operations.map(\.kind) == [.mosaic, .background, .stretch])
    #expect(model.artifactTrailReport == nil)
    #expect(model.artifactMarkedPreviewURL == nil)
    #expect(model.cloudRegionReport == nil)
    #expect(model.detectedStarCount == nil)
}

@Test @MainActor func unsavedProjectReplacementRequiresExplicitDecision() throws {
    let asset = PhotonStackAsset(
        originalURL: URL(fileURLWithPath: "/tmp/unsaved-project-light.tiff"),
        kind: .tiff
    )
    let model = PhotonStackWorkspaceModel(
        project: PhotonStackProject(name: "Unsaved Original", assets: [asset]),
        processingService: RecordingProcessingService()
    )

    #expect(model.hasUnsavedChanges == false)
    model.setRole(.dark, for: asset)
    #expect(model.hasUnsavedChanges)

    let createdImmediately = model.requestCreateProject(
        template: .mosaic,
        name: "Replacement",
        importAfterCreate: true
    )
    #expect(createdImmediately == false)
    #expect(model.project.name == "Unsaved Original")
    #expect(model.pendingProjectTransition == .create(
        template: .mosaic,
        name: "Replacement",
        importAfterCreate: true
    ))

    model.cancelPendingProjectTransition()
    #expect(model.pendingProjectTransition == nil)
    #expect(model.project.name == "Unsaved Original")
    #expect(model.hasUnsavedChanges)

    model.requestOpenProject(from: URL(fileURLWithPath: "/tmp/another-photonstack-project"))
    #expect(model.pendingProjectTransition == .open(
        directory: URL(fileURLWithPath: "/tmp/another-photonstack-project")
    ))
    model.cancelPendingProjectTransition()

    model.requestApplicationTermination()
    #expect(model.pendingProjectTransition == .terminateApplication)
    model.cancelPendingProjectTransition()

    model.requestWindowClose()
    #expect(model.pendingProjectTransition == .closeWindow)
    #expect(model.project.name == "Unsaved Original")
    model.cancelPendingProjectTransition()

    _ = model.requestCreateProject(
        template: .mosaic,
        name: "Replacement",
        importAfterCreate: true
    )
    let transition = try #require(model.discardChangesAndPerformPendingProjectTransition())
    #expect(transition == .create(
        template: .mosaic,
        name: "Replacement",
        importAfterCreate: true
    ))
    #expect(model.project.name == "Replacement")
    #expect(model.project.template == .mosaic)
    #expect(model.hasUnsavedChanges)
}

@Test @MainActor func savingBeforeProjectReplacementPreservesOldProjectSnapshot() async throws {
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackSaveBeforeReplace-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }
    let asset = PhotonStackAsset(
        originalURL: directory.appendingPathComponent("light.tiff"),
        kind: .tiff
    )
    let repository = ProjectRepository()
    let model = PhotonStackWorkspaceModel(
        project: PhotonStackProject(name: "Saved Original", assets: [asset]),
        autosaveEnabled: false,
        processingService: RecordingProcessingService(),
        projectRepository: repository,
        settingsRepository: AppSettingsRepository(
            settingsURL: directory.appendingPathComponent("settings.json")
        )
    )
    model.setRole(.flat, for: asset)
    #expect(model.hasUnsavedChanges)
    _ = model.requestCreateProject(template: .singleFrame, name: "New Project")

    let savedDirectory = directory.appendingPathComponent("saved-original", isDirectory: true)
    let pendingTransition = try #require(model.beginSavingPendingProjectTransition())
    #expect(model.pendingProjectTransition == nil)
    let transition = try #require(
        await model.saveChangesAndPerformProjectTransition(
            pendingTransition,
            to: savedDirectory
        )
    )

    #expect(transition == .create(
        template: .singleFrame,
        name: "New Project",
        importAfterCreate: false
    ))
    let saved = try await repository.load(from: savedDirectory)
    #expect(saved.name == "Saved Original")
    #expect(saved.assets.first?.role == .flat)
    #expect(model.project.name == "New Project")
    #expect(model.currentProjectDirectory == nil)
    #expect(model.hasUnsavedChanges)
}

@Test @MainActor func closingOrQuittingDuringProcessingRequiresExplicitInterruption() async throws {
    let service = RecordingProcessingService(autoStretchDelayNanoseconds: 5_000_000_000)
    let input = URL(fileURLWithPath: "/tmp/photonstack-processing-close-guard.tiff")
    var project = PhotonStackProject(name: "Processing Close Guard")
    project.addAssets(from: [input])
    let model = PhotonStackWorkspaceModel(project: project, processingService: service)

    model.startAutoStretchPreview()
    try await waitForAutoStretchCall(from: service)
    #expect(model.isProcessing)

    #expect(model.requestWindowClose() == false)
    #expect(model.pendingProcessingInterruption == .closeWindow)
    #expect(model.pendingProjectTransition == nil)
    model.cancelPendingProcessingInterruption()
    #expect(model.pendingProcessingInterruption == nil)
    #expect(model.isProcessing)

    #expect(model.requestApplicationTermination() == false)
    #expect(model.pendingProcessingInterruption == .terminateApplication)
    let transition = model.confirmPendingProcessingInterruption(.terminateApplication)
    #expect(transition == .terminateApplication)
    #expect(model.pendingProcessingInterruption == nil)
    #expect(model.pendingProjectTransition == nil)
    #expect(model.isProcessing == false)
}

@Test @MainActor func stoppingProcessingBeforeQuitStillRequiresUnsavedProjectDecision() async throws {
    let service = RecordingProcessingService(autoStretchDelayNanoseconds: 5_000_000_000)
    let input = URL(fileURLWithPath: "/tmp/photonstack-processing-unsaved-quit.tiff")
    var project = PhotonStackProject(name: "Processing Unsaved Quit")
    project.addAssets(from: [input])
    let asset = try #require(project.assets.first)
    let model = PhotonStackWorkspaceModel(
        project: project,
        autosaveEnabled: false,
        processingService: service
    )
    model.setRole(.dark, for: asset)
    #expect(model.hasUnsavedChanges)

    model.startAutoStretchPreview()
    try await waitForAutoStretchCall(from: service)
    #expect(model.requestApplicationTermination() == false)
    #expect(model.pendingProcessingInterruption == .terminateApplication)

    let immediateTransition = model.confirmPendingProcessingInterruption(.terminateApplication)
    #expect(immediateTransition == nil)
    #expect(model.isProcessing == false)
    #expect(model.pendingProjectTransition == .terminateApplication)
    #expect(model.hasUnsavedChanges)

    let finalTransition = model.discardChangesAndPerformPendingProjectTransition()
    #expect(finalTransition == .terminateApplication)
}

@Test @MainActor func mosaicTemplateImportsPanelsWithoutApplyingDeepSkyDefaults() async {
    let model = PhotonStackWorkspaceModel(
        selectedTemplate: .deepSky,
        processingService: RecordingProcessingService()
    )
    model.createProject(template: .mosaic, name: "Three Panel Mosaic")

    model.importAssets(from: [
        URL(fileURLWithPath: "/tmp/mosaic-panel-c.nef"),
        URL(fileURLWithPath: "/tmp/mosaic-panel-a.nef"),
        URL(fileURLWithPath: "/tmp/mosaic-panel-b.nef"),
    ])

    #expect(model.selectedTemplate == .mosaic)
    #expect(model.project.template == .mosaic)
    #expect(model.project.assets.map(\.role) == [.mosaic, .mosaic, .mosaic])
    #expect(model.project.mosaicPanelOrder == model.project.assets.map(\.id))
    #expect(model.mosaicAssets.map(\.id) == model.project.assets.map(\.id))
    #expect(model.parameters.rawProcessingOptions.blackLevelMode == .camera)
}

@Test @MainActor func mosaicAssetsIgnoreDuplicatePanelOrderEntries() {
    let firstID = UUID()
    let secondID = UUID()
    let project = PhotonStackProject(
        name: "Duplicate Mosaic Order",
        template: .mosaic,
        assets: [
            PhotonStackAsset(
                id: firstID,
                originalURL: URL(fileURLWithPath: "/tmp/mosaic-order-a.tiff"),
                kind: .tiff,
                role: .mosaic
            ),
            PhotonStackAsset(
                id: secondID,
                originalURL: URL(fileURLWithPath: "/tmp/mosaic-order-b.tiff"),
                kind: .tiff,
                role: .mosaic
            ),
        ],
        mosaicPanelOrder: [secondID, secondID, firstID]
    )
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: RecordingProcessingService()
    )

    #expect(model.mosaicAssets.map(\.id) == [secondID, firstID])
}

@Test @MainActor func pendingAutosaveKeepsOriginalProjectSnapshotWhenCreatingNewProject() async throws {
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackAutosaveSnapshot-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    let image = directory.appendingPathComponent("light.tiff")
    var project = PhotonStackProject(name: "Original Project")
    project.addAssets(from: [image])
    let asset = try #require(project.assets.first)
    let repository = ProjectRepository()
    let model = PhotonStackWorkspaceModel(
        project: project,
        autosaveEnabled: true,
        processingService: RecordingProcessingService(),
        projectRepository: repository,
        settingsRepository: AppSettingsRepository(
            settingsURL: directory.appendingPathComponent("settings.json")
        )
    )
    let projectDirectory = directory.appendingPathComponent("saved-project", isDirectory: true)
    await model.saveProject(to: projectDirectory)

    model.setRole(.dark, for: asset)
    model.setRole(.flat, for: asset)
    model.createProject(template: .mosaic, name: "Replacement Project")

    let saved = try await waitForSavedProject(
        in: projectDirectory,
        repository: repository,
        expectedRole: .flat
    )
    #expect(saved.name == "Original Project")
    #expect(saved.assets.first?.id == asset.id)
    #expect(saved.assets.first?.role == .flat)
    #expect(model.project.name == "Replacement Project")
    #expect(model.commandOutput.isEmpty)

    await model.saveProject(to: projectDirectory)
    let replacement = try await repository.load(from: projectDirectory)
    #expect(replacement.name == "Replacement Project")
    #expect(replacement.assets.isEmpty)
}

@Test @MainActor func autosaveClearsDirtyStateOnlyAfterLatestProjectVersionIsSaved() async throws {
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackDirtyAutosave-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }
    let asset = PhotonStackAsset(
        originalURL: directory.appendingPathComponent("light.tiff"),
        kind: .tiff
    )
    let repository = ProjectRepository()
    let model = PhotonStackWorkspaceModel(
        project: PhotonStackProject(name: "Dirty Autosave", assets: [asset]),
        autosaveEnabled: true,
        processingService: RecordingProcessingService(),
        projectRepository: repository,
        settingsRepository: AppSettingsRepository(
            settingsURL: directory.appendingPathComponent("settings.json")
        )
    )
    let projectDirectory = directory.appendingPathComponent("project", isDirectory: true)
    await model.saveProject(to: projectDirectory)
    #expect(model.hasUnsavedChanges == false)

    model.setRole(.dark, for: asset)
    model.setRole(.flat, for: asset)
    #expect(model.hasUnsavedChanges)
    _ = try await waitForSavedProject(
        in: projectDirectory,
        repository: repository,
        expectedRole: .flat
    )
    for _ in 0..<100 where model.hasUnsavedChanges {
        try await Task.sleep(nanoseconds: 10_000_000)
    }
    #expect(model.hasUnsavedChanges == false)

    model.setAutosaveEnabled(false)
    model.setRole(.bias, for: asset)
    try await Task.sleep(nanoseconds: 50_000_000)
    #expect(model.hasUnsavedChanges)
    let stillSaved = try await repository.load(from: projectDirectory)
    #expect(stillSaved.assets.first?.role == .flat)

    model.setAutosaveEnabled(true)
    _ = try await waitForSavedProject(
        in: projectDirectory,
        repository: repository,
        expectedRole: .bias
    )
    for _ in 0..<100 where model.hasUnsavedChanges {
        try await Task.sleep(nanoseconds: 10_000_000)
    }
    #expect(model.hasUnsavedChanges == false)
}

@Test @MainActor func backgroundStrengthIsPassedAndPersisted() async throws {
    let service = RecordingProcessingService()
    let image = URL(fileURLWithPath: "/tmp/photonstack-background.tiff")
    var project = PhotonStackProject(name: "Background Test")
    project.addAssets(from: [image])

    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: service
    )

    await model.backgroundPreview(
        model: "grid",
        mode: "divide",
        strength: 0.42,
        preserveBrightness: false,
        protectBrightTargets: false
    )

    let snapshot = await service.snapshot()
    #expect(snapshot.backgroundStrength == 0.42)
    #expect(snapshot.backgroundPreserveBrightness == false)
    #expect(snapshot.backgroundProtectBrightTargets == false)
    let operation = try #require(model.project.editGraph.operations.first { $0.kind == .background })
    #expect(operation.parameters["model"] == "grid")
    #expect(operation.parameters["mode"] == "divide")
    #expect(operation.parameters["strength"] == "0.42")
    #expect(operation.parameters["preserveBrightness"] == "false")
    #expect(operation.parameters["protectBrightTargets"] == "false")
}

@Test @MainActor func advancedProcessingInputsNormalizeBeforeExecutionAndRecording() async throws {
    let service = RecordingProcessingService()
    let image = URL(fileURLWithPath: "/tmp/photonstack-normalized-advanced-\(UUID().uuidString).tiff")
    var project = PhotonStackProject(name: "Normalized Advanced Parameters")
    project.addAssets(from: [image])
    let previewDirectory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackNormalizedAdvanced-\(UUID().uuidString)", isDirectory: true)
    defer {
        try? FileManager.default.removeItem(at: previewDirectory)
    }
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: service,
        previewDirectory: previewDirectory
    )

    await model.detectStars(sigmaThreshold: .nan, minPeak: .infinity, maxStars: Int.min)
    await model.createStarMask(
        radius: Int.min,
        largeRadius: Int.max,
        layered: true,
        sigmaThreshold: .nan,
        minPeak: -.infinity
    )
    await model.backgroundPreview(model: "invalid", mode: "invalid", strength: .nan)
    await model.removeClouds(strength: .nan)
    await model.normalizePreview(targetBackground: .nan, targetScale: .infinity)
    await model.colorNeutralizePreview(strength: -.infinity)
    await model.colorSaturatePreview(amount: .nan)
    await model.deconvolvePreview(iterations: Int.min, radius: Int.max, sigma: .nan)
    await model.runDrizzle(scale: Int.max, pixfrac: .nan, alignment: .distortion)

    let snapshot = await service.snapshot()
    #expect(snapshot.starDetectionThreshold == 3)
    #expect(snapshot.starMinimumPeak == 0.05)
    #expect(snapshot.starMaximumCount == 1)
    #expect(snapshot.starMaskRadius == 1)
    #expect(snapshot.largeStarMaskRadius == 64)
    #expect(snapshot.backgroundStrength == 0.35)
    #expect(snapshot.backgroundPreserveBrightness == true)
    #expect(snapshot.backgroundProtectBrightTargets == true)
    #expect(snapshot.cloudRemovalStrength == 0.65)
    #expect(snapshot.drizzleScale == 4)
    #expect(snapshot.drizzlePixfrac == 1)

    let operations = model.project.editGraph.operations
    let background = try #require(operations.last { $0.kind == .background })
    #expect(background.parameters["model"] == "grid")
    #expect(background.parameters["mode"] == "subtract")
    #expect(background.parameters["strength"] == "0.35")
    #expect(background.parameters["protectBrightTargets"] == "true")
    let normalize = try #require(operations.last { $0.kind == .normalize })
    #expect(normalize.parameters["targetBackground"] == "0.25")
    #expect(normalize.parameters["targetScale"] == "1.0")
    let neutralize = try #require(operations.last { $0.kind == .colorNeutralize })
    #expect(neutralize.parameters["strength"] == "1.0")
    let saturation = try #require(operations.last { $0.kind == .colorSaturate })
    #expect(saturation.parameters["amount"] == "0.2")
    let deconvolve = try #require(operations.last { $0.kind == .deconvolve })
    #expect(deconvolve.parameters["iterations"] == "1")
    #expect(deconvolve.parameters["radius"] == "8")
    #expect(deconvolve.parameters["sigma"] == "1.2")
    let drizzle = try #require(operations.last { $0.kind == .drizzle })
    #expect(drizzle.parameters["scale"] == "4")
    #expect(drizzle.parameters["pixfrac"] == "1.0")
}

@Test @MainActor func workspaceProcessingParametersNormalizeAtInitializationAndBindingUpdates() {
    let duplicateCurvePointID = UUID()
    let initialParameters = ProcessingParameters(
        previewWidth: Int.min,
        stretchTargetBackground: .nan,
        starReductionAmount: .infinity,
        comaReductionAmount: -.infinity,
        comaReductionRadius: Int.max,
        comaReductionEccentricity: .nan,
        localContrastAmount: .nan,
        localContrastRadius: Int.min,
        denoiseAmount: .nan,
        denoiseChromaAmount: .infinity,
        denoiseRadius: Int.max,
        sharpenAmount: -.infinity,
        sharpenRadius: Int.min,
        cloudRemovalStrength: .nan,
        curvePoints: [
            CurveEditorPoint(input: .nan, output: 0.5),
            CurveEditorPoint(id: duplicateCurvePointID, input: 0, output: 0),
            CurveEditorPoint(id: duplicateCurvePointID, input: 1, output: 1),
        ],
        mosaicOverlapPixels: Int.min,
        mosaicColumns: Int.max,
        mosaicPreviewWidth: Int.min,
        rawProcessingOptions: RawProcessingOptions(
            manualWhiteBalanceTemperature: .nan,
            manualWhiteBalanceTint: .infinity,
            exposureBias: .nan,
            manualBlackLevel: -.infinity
        )
    )
    let model = PhotonStackWorkspaceModel(
        parameters: initialParameters,
        processingService: RecordingProcessingService()
    )

    #expect(model.parameters.previewWidth == 320)
    #expect(model.parameters.stretchTargetBackground == 0.25)
    #expect(model.parameters.starReductionAmount == 0.35)
    #expect(model.parameters.comaReductionAmount == 0.8)
    #expect(model.parameters.comaReductionRadius == 16)
    #expect(model.parameters.comaReductionEccentricity == 0.22)
    #expect(model.parameters.localContrastAmount == 0.25)
    #expect(model.parameters.localContrastRadius == 1)
    #expect(model.parameters.denoiseAmount == 0.4)
    #expect(model.parameters.denoiseChromaAmount == 0.55)
    #expect(model.parameters.denoiseRadius == 8)
    #expect(model.parameters.sharpenAmount == 0.35)
    #expect(model.parameters.sharpenRadius == 1)
    #expect(model.parameters.cloudRemovalStrength == 0.65)
    #expect(model.parameters.mosaicOverlapPixels == 0)
    #expect(model.parameters.mosaicColumns == 12)
    #expect(model.parameters.mosaicPreviewWidth == 320)
    #expect(model.parameters.rawProcessingOptions.manualWhiteBalanceTemperature == 6500)
    #expect(model.parameters.rawProcessingOptions.manualWhiteBalanceTint == 0)
    #expect(model.parameters.rawProcessingOptions.exposureBias == 0)
    #expect(model.parameters.rawProcessingOptions.manualBlackLevel == 0)
    #expect(model.parameters.curveEditorPoints.count == 2)
    #expect(Set(model.parameters.curveEditorPoints.map(\.id)).count == 2)
    #expect(model.parameters.curveEditorPoints.allSatisfy { $0.input.isFinite && $0.output.isFinite })

    var updatedParameters = model.parameters
    updatedParameters.previewWidth = Int.max
    updatedParameters.stretchTargetBackground = 1
    updatedParameters.starReductionAmount = -1
    updatedParameters.comaReductionAmount = 2
    updatedParameters.comaReductionRadius = Int.min
    updatedParameters.comaReductionEccentricity = 2
    updatedParameters.localContrastAmount = -1
    updatedParameters.localContrastRadius = Int.max
    updatedParameters.denoiseAmount = 2
    updatedParameters.denoiseChromaAmount = -1
    updatedParameters.denoiseRadius = Int.min
    updatedParameters.sharpenAmount = 2
    updatedParameters.sharpenRadius = Int.max
    updatedParameters.cloudRemovalStrength = -1
    updatedParameters.mosaicOverlapPixels = Int.max
    updatedParameters.mosaicColumns = Int.min
    updatedParameters.mosaicPreviewWidth = Int.max
    updatedParameters.rawProcessingOptions.manualWhiteBalanceTemperature = 100000
    updatedParameters.rawProcessingOptions.manualWhiteBalanceTint = -1000
    updatedParameters.rawProcessingOptions.exposureBias = 5
    updatedParameters.rawProcessingOptions.manualBlackLevel = 2
    model.setProcessingParameters(updatedParameters)

    #expect(model.parameters.previewWidth == 4096)
    #expect(model.parameters.stretchTargetBackground == 0.65)
    #expect(model.parameters.starReductionAmount == 0)
    #expect(model.parameters.comaReductionAmount == 1)
    #expect(model.parameters.comaReductionRadius == 2)
    #expect(model.parameters.comaReductionEccentricity == 0.9)
    #expect(model.parameters.localContrastAmount == 0)
    #expect(model.parameters.localContrastRadius == 64)
    #expect(model.parameters.denoiseAmount == 1)
    #expect(model.parameters.denoiseChromaAmount == 0)
    #expect(model.parameters.denoiseRadius == 1)
    #expect(model.parameters.sharpenAmount == 1)
    #expect(model.parameters.sharpenRadius == 8)
    #expect(model.parameters.cloudRemovalStrength == 0)
    #expect(model.parameters.mosaicOverlapPixels == 4096)
    #expect(model.parameters.mosaicColumns == 1)
    #expect(model.parameters.mosaicPreviewWidth == 4096)
    #expect(model.parameters.rawProcessingOptions.manualWhiteBalanceTemperature == 50000)
    #expect(model.parameters.rawProcessingOptions.manualWhiteBalanceTint == -150)
    #expect(model.parameters.rawProcessingOptions.exposureBias == 2)
    #expect(model.parameters.rawProcessingOptions.manualBlackLevel == 1)
}

@Test @MainActor func consoleCommandRunsBundledCLIArguments() async throws {
    let service = RecordingProcessingService()
    let model = PhotonStackWorkspaceModel(
        project: PhotonStackProject(name: "Console Test"),
        processingService: service
    )

    await model.runConsoleCommand("photonstack clouds detect --input '/tmp/with space.tiff'")

    let snapshot = await service.snapshot()
    #expect(snapshot.cliArguments == ["clouds", "detect", "--input", "/tmp/with space.tiff"])
    #expect(model.commandOutput.contains("manual cli"))
}

@Test @MainActor func cloudDetectionAndSelectedRemovalAreRecorded() async throws {
    let service = RecordingProcessingService()
    let directory = FileManager.default.temporaryDirectory.appendingPathComponent(
        "PhotonStackCloudSelection-\(UUID().uuidString)",
        isDirectory: true
    )
    let image = directory.appendingPathComponent("input.png")
    defer {
        try? FileManager.default.removeItem(at: directory)
    }
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    try writeSolidPNG(to: image, red: 0.08, green: 0.10, blue: 0.18, width: 240, height: 140)
    var project = PhotonStackProject(name: "Cloud Test")
    project.addAssets(from: [image])

    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: service,
        previewDirectory: directory.appendingPathComponent("previews", isDirectory: true)
    )

    await model.detectClouds()

    let report = try #require(model.cloudRegionReport)
    #expect(report.clouds == 2)
    #expect(report.topItems.count == 2)
    #expect(report.items[0].displayLabel == "#1")
    #expect(report.items[1].displayLabel == "#2")
    #expect(report.items[0].maskRects.count == 2)
    #expect(report.items[0].maskRects[1].x == 90)
    #expect(model.latestJob?.kind == .cloudDetect)

    await model.removeClouds(selectedIndices: [1], strength: 0.4)

    let snapshot = await service.snapshot()
    #expect(snapshot.cloudDetectCalls == 1)
    #expect(snapshot.cloudSelectedIndices == [1])
    #expect(snapshot.cloudRemovalStrength == 0.4)
    let operation = try #require(model.project.editGraph.operations.first { $0.kind == .cloudRemove })
    #expect(operation.parameters["selectedIndices"] == "1")
    #expect(operation.parameters["strength"] == "0.4")
    #expect(model.project.layers.map(\.kind) == [.baseImage, .adjustment])
}

@Test @MainActor func cloudDetectionRejectsAReportWhoseCountDoesNotMatchItsItems() async {
    let service = RecordingProcessingService(
        cloudDetectOutputOverride: #"{"type":"complete","command":"clouds detect","clouds":2,"removedClouds":0,"items":[]}"#
    )
    let image = URL(fileURLWithPath: "/tmp/photonstack-invalid-cloud-report.tiff")
    var project = PhotonStackProject(name: "Invalid Cloud Report")
    project.addAssets(from: [image])
    let model = PhotonStackWorkspaceModel(project: project, processingService: service)

    await model.detectClouds()

    #expect(model.latestJob?.status == .failed)
    #expect(model.cloudRegionReport == nil)
    #expect(model.artifactMarkedPreviewURL == nil)
}

@Test @MainActor func cloudRemovalRejectsCandidateOutsideCurrentReport() async {
    let service = RecordingProcessingService()
    let image = URL(fileURLWithPath: "/tmp/photonstack-invalid-cloud-selection.tiff")
    var project = PhotonStackProject(name: "Invalid Cloud Selection")
    project.addAssets(from: [image])
    let model = PhotonStackWorkspaceModel(project: project, processingService: service)

    await model.detectClouds()
    await model.removeClouds(selectedIndices: [2], strength: 0.4)

    #expect(model.latestJob?.status == .failed)
    #expect(model.errorMessage?.contains("selected candidate") == true)
    #expect(await service.snapshot().cloudSelectedIndices == nil)
    #expect(model.cloudRegionReport != nil)
    #expect(model.project.editGraph.operations.isEmpty)
}

@Test @MainActor func explicitCloudRemovalRequiresUniqueCurrentSelection() async {
    let service = RecordingProcessingService()
    let image = URL(fileURLWithPath: "/tmp/photonstack-strict-cloud-selection.tiff")
    var project = PhotonStackProject(name: "Strict Cloud Selection")
    project.addAssets(from: [image])
    let model = PhotonStackWorkspaceModel(project: project, processingService: service)

    await model.removeClouds(selectedIndices: [0], strength: 0.4)
    #expect(model.latestJob?.status == .failed)

    await model.detectClouds()
    let report = model.cloudRegionReport
    await model.removeClouds(selectedIndices: [], strength: 0.4)
    #expect(model.latestJob?.status == .failed)
    #expect(model.cloudRegionReport == report)

    await model.removeClouds(selectedIndices: [1, 1], strength: 0.4)
    #expect(model.latestJob?.status == .failed)
    #expect(model.cloudRegionReport == report)
    #expect(await service.snapshot().cloudSelectedIndices == nil)
    #expect(model.project.editGraph.operations.isEmpty)
}

@Test @MainActor func failedCloudRemovalPreservesDetectionStateAndStrength() async throws {
    let service = RecordingProcessingService(failCloudRemoval: true)
    let directory = FileManager.default.temporaryDirectory.appendingPathComponent(
        "PhotonStackFailedCloudRemoval-\(UUID().uuidString)",
        isDirectory: true
    )
    let image = directory.appendingPathComponent("input.png")
    defer {
        try? FileManager.default.removeItem(at: directory)
    }
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    try writeSolidPNG(to: image, red: 0.05, green: 0.06, blue: 0.12, width: 240, height: 140)
    var project = PhotonStackProject(name: "Failed Cloud Removal")
    project.addAssets(from: [image])
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: service,
        previewDirectory: directory.appendingPathComponent("previews", isDirectory: true)
    )
    let originalStrength = model.parameters.cloudRemovalStrength

    await model.detectClouds()
    let report = model.cloudRegionReport
    let markedPreview = model.artifactMarkedPreviewURL
    await model.removeClouds(selectedIndices: [1], strength: 0.4)

    #expect(model.latestJob?.status == .failed)
    #expect(model.cloudRegionReport == report)
    #expect(model.artifactMarkedPreviewURL == markedPreview)
    #expect(model.parameters.cloudRemovalStrength == originalStrength)
    #expect(model.project.editGraph.operations.isEmpty)
}

@Test @MainActor func cloudReplayMatchesRecordedGeometryAcrossScaleAndCandidateOrder() async throws {
    let directory = FileManager.default.temporaryDirectory.appendingPathComponent(
        "PhotonStackCloudGeometryReplay-\(UUID().uuidString)",
        isDirectory: true
    )
    let previewInput = directory.appendingPathComponent("preview.png")
    let fullInput = directory.appendingPathComponent("full.png")
    defer {
        try? FileManager.default.removeItem(at: directory)
    }
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    try writeSolidPNG(to: previewInput, red: 0.08, green: 0.09, blue: 0.14, width: 100, height: 50)
    try writeSolidPNG(to: fullInput, red: 0.08, green: 0.09, blue: 0.14, width: 200, height: 100)

    let previewReport = #"{"type":"complete","command":"clouds detect","width":100,"height":50,"clouds":2,"removedClouds":0,"items":[{"confidence":0.9,"x":5,"y":5,"width":30,"height":20,"coverage":0.1,"meanLuminance":0.3,"backgroundLuminance":0.1,"mask":[]},{"confidence":0.8,"x":60,"y":10,"width":30,"height":30,"coverage":0.2,"meanLuminance":0.28,"backgroundLuminance":0.1,"mask":[]}]}"#
    let primingService = RecordingProcessingService(cloudDetectOutputOverride: previewReport)
    var previewProject = PhotonStackProject(name: "Cloud Preview Selection")
    previewProject.addAssets(from: [previewInput])
    let primingModel = PhotonStackWorkspaceModel(
        project: previewProject,
        processingService: primingService,
        previewDirectory: directory.appendingPathComponent("preview-cache", isDirectory: true)
    )
    await primingModel.detectClouds()
    await primingModel.removeClouds(selectedIndices: [1], strength: 0.4)
    let recordedOperation = try #require(
        primingModel.project.editGraph.operations.last { $0.kind == .cloudRemove }
    )
    #expect(recordedOperation.parameters["selectedCloudRegionsV1"]?.isEmpty == false)

    let fullReport = #"{"type":"complete","command":"clouds detect","width":200,"height":100,"clouds":2,"removedClouds":0,"items":[{"confidence":0.8,"x":120,"y":20,"width":60,"height":60,"coverage":0.2,"meanLuminance":0.28,"backgroundLuminance":0.1,"mask":[]},{"confidence":0.9,"x":10,"y":10,"width":60,"height":40,"coverage":0.1,"meanLuminance":0.3,"backgroundLuminance":0.1,"mask":[]}]}"#
    let replayService = RecordingProcessingService(cloudDetectOutputOverride: fullReport)
    var replayParameters = recordedOperation.parameters
    replayParameters.removeValue(forKey: "input")
    replayParameters.removeValue(forKey: "output")
    var replayProject = PhotonStackProject(name: "Cloud Full Resolution Replay")
    replayProject.addAssets(from: [fullInput])
    replayProject.editGraph = EditGraph(operations: [
        EditOperation(kind: .cloudRemove, parameters: replayParameters),
    ])
    let replayModel = PhotonStackWorkspaceModel(
        project: replayProject,
        processingService: replayService,
        previewDirectory: directory.appendingPathComponent("full-cache", isDirectory: true)
    )

    replayModel.startReplayEditGraph()
    try await waitForStartedProcessingCompletion(in: replayModel)

    let replaySnapshot = await replayService.snapshot()
    #expect(replaySnapshot.cloudDetectCalls == 1)
    #expect(replaySnapshot.cloudSelectedIndices == [0])
    #expect(replayModel.latestJob?.status == .succeeded)
}

@Test @MainActor func cloudReplayUsesGlobalCandidateAssignment() async throws {
    let directory = FileManager.default.temporaryDirectory.appendingPathComponent(
        "PhotonStackCloudGlobalAssignment-\(UUID().uuidString)",
        isDirectory: true
    )
    let previewInput = directory.appendingPathComponent("preview.png")
    let replayInput = directory.appendingPathComponent("replay.png")
    defer {
        try? FileManager.default.removeItem(at: directory)
    }
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    try writeSolidPNG(to: previewInput, red: 0.08, green: 0.09, blue: 0.14, width: 100, height: 100)
    try writeSolidPNG(to: replayInput, red: 0.08, green: 0.09, blue: 0.14, width: 100, height: 100)

    let previewReport = #"{"type":"complete","command":"clouds detect","width":100,"height":100,"clouds":2,"removedClouds":0,"items":[{"confidence":0.9,"x":6,"y":20,"width":10,"height":10,"coverage":0.1,"meanLuminance":0.3,"backgroundLuminance":0.1,"mask":[]},{"confidence":0.8,"x":0,"y":20,"width":10,"height":10,"coverage":0.1,"meanLuminance":0.28,"backgroundLuminance":0.1,"mask":[]}]}"#
    let primingService = RecordingProcessingService(cloudDetectOutputOverride: previewReport)
    var previewProject = PhotonStackProject(name: "Cloud Global Assignment Preview")
    previewProject.addAssets(from: [previewInput])
    let primingModel = PhotonStackWorkspaceModel(
        project: previewProject,
        processingService: primingService,
        previewDirectory: directory.appendingPathComponent("preview-cache", isDirectory: true)
    )
    await primingModel.detectClouds()
    await primingModel.removeClouds(selectedIndices: [0, 1], strength: 0.4)
    let operation = try #require(primingModel.project.editGraph.operations.last { $0.kind == .cloudRemove })

    let replayReport = #"{"type":"complete","command":"clouds detect","width":100,"height":100,"clouds":2,"removedClouds":0,"items":[{"confidence":0.8,"x":4,"y":20,"width":10,"height":10,"coverage":0.1,"meanLuminance":0.28,"backgroundLuminance":0.1,"mask":[]},{"confidence":0.9,"x":8,"y":20,"width":10,"height":10,"coverage":0.1,"meanLuminance":0.3,"backgroundLuminance":0.1,"mask":[]}]}"#
    let replayService = RecordingProcessingService(cloudDetectOutputOverride: replayReport)
    var replayParameters = operation.parameters
    replayParameters.removeValue(forKey: "input")
    replayParameters.removeValue(forKey: "output")
    var replayProject = PhotonStackProject(name: "Cloud Global Assignment Replay")
    replayProject.addAssets(from: [replayInput])
    replayProject.editGraph = EditGraph(operations: [
        EditOperation(kind: .cloudRemove, parameters: replayParameters),
    ])
    let replayModel = PhotonStackWorkspaceModel(
        project: replayProject,
        processingService: replayService,
        previewDirectory: directory.appendingPathComponent("replay-cache", isDirectory: true)
    )

    replayModel.startReplayEditGraph()
    try await waitForStartedProcessingCompletion(in: replayModel)

    #expect(replayModel.latestJob?.status == .succeeded)
    #expect(await replayService.snapshot().cloudSelectedIndices == [0, 1])
}

@Test @MainActor func cloudMarkedPreviewRendersComponentMaskInsteadOfBoundingBox() async throws {
    let service = RecordingProcessingService()
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackCloudMaskPreview-\(UUID().uuidString)", isDirectory: true)
    let input = directory.appendingPathComponent("input.png")
    defer {
        try? FileManager.default.removeItem(at: directory)
    }
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    try writeSolidPNG(to: input, red: 0.02, green: 0.02, blue: 0.02, width: 240, height: 140)
    var project = PhotonStackProject(name: "Cloud Mask Preview")
    project.addAssets(from: [input])
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: service,
        previewDirectory: directory
    )

    await model.detectClouds()

    let preview = try #require(model.artifactMarkedPreviewURL)
    let insideMask = try rgbaPixel(in: preview, x: 30, y: 100)
    let insideBoundsOutsideMask = try rgbaPixel(in: preview, x: 70, y: 100)
    #expect(Int(insideMask.blue) > Int(insideBoundsOutsideMask.blue) + 5)
    #expect(Int(insideMask.green) > Int(insideBoundsOutsideMask.green) + 5)
}

@Test @MainActor func markedPreviewRenderingFailurePreservesDetectionReportsAndShowsError() async throws {
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackMarkedPreviewFailure-\(UUID().uuidString)", isDirectory: true)
    let input = directory.appendingPathComponent("damaged.png")
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    try Data([0x00, 0x01, 0x02]).write(to: input)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    var project = PhotonStackProject(name: "Marked Preview Failure")
    project.addAssets(from: [input])
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: RecordingProcessingService(),
        previewDirectory: directory.appendingPathComponent("previews", isDirectory: true)
    )

    await model.detectArtifactTrails()
    #expect(model.artifactTrailReport?.items.isEmpty == false)
    #expect(model.artifactMarkedPreviewURL == nil)
    #expect(model.errorMessage == model.localized(.markedPreviewRenderFailed))
    #expect(model.latestJob?.status == .succeeded)

    try writeSolidPNG(to: input, red: 0.02, green: 0.02, blue: 0.02, width: 240, height: 140)
    model.refreshArtifactMarkedPreview(selectedIndices: [])
    #expect(model.artifactMarkedPreviewURL != nil)
    #expect(model.errorMessage == nil)

    try Data([0x00, 0x01, 0x02]).write(to: input, options: .atomic)
    await model.detectClouds()
    #expect(model.cloudRegionReport?.items.isEmpty == false)
    #expect(model.artifactMarkedPreviewURL == nil)
    #expect(model.errorMessage == model.localized(.markedPreviewRenderFailed))
    #expect(model.latestJob?.status == .succeeded)

    try writeSolidPNG(to: input, red: 0.02, green: 0.02, blue: 0.02, width: 240, height: 140)
    model.refreshCloudMarkedPreview(selectedIndices: [])
    #expect(model.artifactMarkedPreviewURL != nil)
    #expect(model.errorMessage == nil)
}

@Test @MainActor func artifactAndCloudMarkedPreviewsApplyExifOrientation() async throws {
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackOrientedMarkers-\(UUID().uuidString)", isDirectory: true)
    let input = directory.appendingPathComponent("oriented.jpg")
    defer {
        try? FileManager.default.removeItem(at: directory)
    }
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    try writeOrientedJPEG(to: input, width: 80, height: 40, orientation: 6)

    let artifactReport = #"{"type":"complete","command":"artifacts detect","width":40,"height":80,"trails":1,"removedTrails":0,"protectedMeteors":0,"items":[{"kind":"airplane","confidence":0.9,"x1":5,"y1":10,"x2":35,"y2":20,"length":31.6,"width":2,"meanBrightness":0.05}]}"#
    let cloudReport = #"{"type":"complete","command":"clouds detect","width":40,"height":80,"clouds":1,"removedClouds":0,"items":[{"confidence":0.8,"x":5,"y":10,"width":20,"height":30,"coverage":0.2,"meanLuminance":0.3,"backgroundLuminance":0.1,"mask":[]}]}"#
    let service = RecordingProcessingService(
        artifactDetectOutputOverride: artifactReport,
        cloudDetectOutputOverride: cloudReport
    )
    var project = PhotonStackProject(name: "Oriented Marker Test")
    project.addAssets(from: [input])
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: service,
        previewDirectory: directory.appendingPathComponent("previews", isDirectory: true)
    )

    await model.detectArtifactTrails()
    let artifactPreview = try #require(model.artifactMarkedPreviewURL)
    let artifactSource = try #require(CGImageSourceCreateWithURL(artifactPreview as CFURL, nil))
    let artifactImage = try #require(CGImageSourceCreateImageAtIndex(artifactSource, 0, nil))
    #expect(artifactImage.width == 40)
    #expect(artifactImage.height == 80)

    await model.detectClouds()
    #expect(FileManager.default.fileExists(atPath: artifactPreview.path) == false)
    let cloudPreview = try #require(model.artifactMarkedPreviewURL)
    let cloudSource = try #require(CGImageSourceCreateWithURL(cloudPreview as CFURL, nil))
    let cloudImage = try #require(CGImageSourceCreateImageAtIndex(cloudSource, 0, nil))
    #expect(cloudImage.width == 40)
    #expect(cloudImage.height == 80)
}

@Test @MainActor func artifactTrailDetectionAndSelectedRemovalAreRecorded() async throws {
    let service = RecordingProcessingService()
    let directory = FileManager.default.temporaryDirectory.appendingPathComponent(
        "PhotonStackArtifactSelection-\(UUID().uuidString)",
        isDirectory: true
    )
    let image = directory.appendingPathComponent("input.png")
    defer {
        try? FileManager.default.removeItem(at: directory)
    }
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    try writeSolidPNG(to: image, red: 0.08, green: 0.10, blue: 0.18, width: 240, height: 140)
    var project = PhotonStackProject(name: "Artifact Test")
    project.addAssets(from: [image])

    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: service,
        previewDirectory: directory.appendingPathComponent("previews", isDirectory: true)
    )

    await model.detectArtifactTrails()

    let report = try #require(model.artifactTrailReport)
    #expect(report.trails == 2)
    #expect(report.protectedMeteors == 0)
    #expect(report.topItems.map(\.kind) == ["airplane", "drone"])
    #expect(report.topItems.first?.weight == 0.80)
    #expect(model.latestJob?.kind == .artifactDetect)

    await model.removeArtifactTrails(selectedIndices: [1], removeMeteors: false)

    let snapshot = await service.snapshot()
    #expect(snapshot.artifactDetectCalls == 1)
    #expect(snapshot.artifactSelectedIndices == [1])
    #expect(snapshot.artifactRemoveSatellites == true)
    #expect(snapshot.artifactRemoveMeteors == false)
    let operation = try #require(model.project.editGraph.operations.first { $0.kind == .artifactRemove })
    #expect(operation.parameters["algorithmRevision"] == "satellite-trails-v6-path-shape")
    #expect(operation.parameters["selectedIndices"] == "1")
    #expect(operation.parameters["removeSatellites"] == "true")
    #expect(operation.parameters["removeMeteors"] == "false")
    #expect(model.project.layers.map(\.kind) == [.baseImage, .adjustment])
}

@Test @MainActor func artifactRemovalRejectsCandidateOutsideCurrentReport() async {
    let service = RecordingProcessingService()
    let image = URL(fileURLWithPath: "/tmp/photonstack-invalid-artifact-selection.tiff")
    var project = PhotonStackProject(name: "Invalid Artifact Selection")
    project.addAssets(from: [image])
    let model = PhotonStackWorkspaceModel(project: project, processingService: service)

    await model.detectArtifactTrails()
    await model.removeArtifactTrails(selectedIndices: [2], removeMeteors: false)

    #expect(model.latestJob?.status == .failed)
    #expect(model.errorMessage?.contains("selected candidate") == true)
    #expect(await service.snapshot().artifactSelectedIndices == nil)
    #expect(model.artifactTrailReport != nil)
    #expect(model.project.editGraph.operations.isEmpty)
}

@Test @MainActor func artifactReplayMatchesRecordedGeometryAcrossScaleAndCandidateOrder() async throws {
    let directory = FileManager.default.temporaryDirectory.appendingPathComponent(
        "PhotonStackArtifactGeometryReplay-\(UUID().uuidString)",
        isDirectory: true
    )
    let previewInput = directory.appendingPathComponent("preview.png")
    let fullInput = directory.appendingPathComponent("full.png")
    defer {
        try? FileManager.default.removeItem(at: directory)
    }
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    try writeSolidPNG(to: previewInput, red: 0.05, green: 0.07, blue: 0.12, width: 100, height: 50)
    try writeSolidPNG(to: fullInput, red: 0.05, green: 0.07, blue: 0.12, width: 200, height: 100)

    let previewReport = #"{"type":"complete","command":"artifacts detect","width":100,"height":50,"trails":2,"removedTrails":0,"protectedMeteors":0,"items":[{"kind":"airplane","confidence":0.9,"x1":10,"y1":10,"x2":70,"y2":10,"length":60,"width":2,"meanBrightness":0.05},{"kind":"drone","confidence":0.8,"x1":20,"y1":40,"x2":80,"y2":35,"length":60.2,"width":1.5,"meanBrightness":0.04}]}"#
    let primingService = RecordingProcessingService(artifactDetectOutputOverride: previewReport)
    var previewProject = PhotonStackProject(name: "Artifact Preview Selection")
    previewProject.addAssets(from: [previewInput])
    let primingModel = PhotonStackWorkspaceModel(
        project: previewProject,
        processingService: primingService,
        previewDirectory: directory.appendingPathComponent("preview-cache", isDirectory: true)
    )
    await primingModel.detectArtifactTrails()
    await primingModel.removeArtifactTrails(selectedIndices: [1], removeMeteors: false)
    let recordedOperation = try #require(
        primingModel.project.editGraph.operations.last { $0.kind == .artifactRemove }
    )
    #expect(recordedOperation.parameters["selectedTrailsV1"]?.isEmpty == false)

    let fullReport = #"{"type":"complete","command":"artifacts detect","width":200,"height":100,"trails":2,"removedTrails":0,"protectedMeteors":0,"items":[{"kind":"drone","confidence":0.8,"x1":40,"y1":80,"x2":160,"y2":70,"length":120.4,"width":3,"meanBrightness":0.04},{"kind":"airplane","confidence":0.9,"x1":20,"y1":20,"x2":140,"y2":20,"length":120,"width":4,"meanBrightness":0.05}]}"#
    let replayService = RecordingProcessingService(artifactDetectOutputOverride: fullReport)
    var replayParameters = recordedOperation.parameters
    replayParameters.removeValue(forKey: "input")
    replayParameters.removeValue(forKey: "output")
    var replayProject = PhotonStackProject(name: "Artifact Full Resolution Replay")
    replayProject.addAssets(from: [fullInput])
    replayProject.editGraph = EditGraph(operations: [
        EditOperation(kind: .artifactRemove, parameters: replayParameters),
    ])
    let replayModel = PhotonStackWorkspaceModel(
        project: replayProject,
        processingService: replayService,
        previewDirectory: directory.appendingPathComponent("full-cache", isDirectory: true)
    )

    replayModel.startReplayEditGraph()
    try await waitForStartedProcessingCompletion(in: replayModel)

    let replaySnapshot = await replayService.snapshot()
    #expect(replaySnapshot.artifactDetectCalls == 1)
    #expect(replaySnapshot.artifactSelectedIndices == [0])
    #expect(replayModel.latestJob?.status == .succeeded)
}

@Test @MainActor func artifactReplayUsesGlobalCandidateAssignment() async throws {
    let directory = FileManager.default.temporaryDirectory.appendingPathComponent(
        "PhotonStackArtifactGlobalAssignment-\(UUID().uuidString)",
        isDirectory: true
    )
    let previewInput = directory.appendingPathComponent("preview.png")
    let replayInput = directory.appendingPathComponent("replay.png")
    defer {
        try? FileManager.default.removeItem(at: directory)
    }
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    try writeSolidPNG(to: previewInput, red: 0.05, green: 0.07, blue: 0.12, width: 100, height: 100)
    try writeSolidPNG(to: replayInput, red: 0.05, green: 0.07, blue: 0.12, width: 100, height: 100)

    let previewReport = #"{"type":"complete","command":"artifacts detect","width":100,"height":100,"trails":2,"removedTrails":0,"protectedMeteors":0,"items":[{"kind":"drone","confidence":0.9,"x1":10,"y1":4,"x2":90,"y2":4,"length":80,"width":2,"meanBrightness":0.05},{"kind":"drone","confidence":0.8,"x1":10,"y1":0,"x2":90,"y2":0,"length":80,"width":2,"meanBrightness":0.04}]}"#
    let primingService = RecordingProcessingService(artifactDetectOutputOverride: previewReport)
    var previewProject = PhotonStackProject(name: "Artifact Global Assignment Preview")
    previewProject.addAssets(from: [previewInput])
    let primingModel = PhotonStackWorkspaceModel(
        project: previewProject,
        processingService: primingService,
        previewDirectory: directory.appendingPathComponent("preview-cache", isDirectory: true)
    )
    await primingModel.detectArtifactTrails()
    await primingModel.removeArtifactTrails(selectedIndices: [0, 1], removeMeteors: false)
    let operation = try #require(primingModel.project.editGraph.operations.last { $0.kind == .artifactRemove })

    let replayReport = #"{"type":"complete","command":"artifacts detect","width":100,"height":100,"trails":2,"removedTrails":0,"protectedMeteors":0,"items":[{"kind":"drone","confidence":0.8,"x1":10,"y1":3,"x2":90,"y2":3,"length":80,"width":2,"meanBrightness":0.04},{"kind":"drone","confidence":0.9,"x1":10,"y1":5,"x2":90,"y2":5,"length":80,"width":2,"meanBrightness":0.05}]}"#
    let replayService = RecordingProcessingService(artifactDetectOutputOverride: replayReport)
    var replayParameters = operation.parameters
    replayParameters.removeValue(forKey: "input")
    replayParameters.removeValue(forKey: "output")
    var replayProject = PhotonStackProject(name: "Artifact Global Assignment Replay")
    replayProject.addAssets(from: [replayInput])
    replayProject.editGraph = EditGraph(operations: [
        EditOperation(kind: .artifactRemove, parameters: replayParameters),
    ])
    let replayModel = PhotonStackWorkspaceModel(
        project: replayProject,
        processingService: replayService,
        previewDirectory: directory.appendingPathComponent("replay-cache", isDirectory: true)
    )

    replayModel.startReplayEditGraph()
    try await waitForStartedProcessingCompletion(in: replayModel)

    #expect(replayModel.latestJob?.status == .succeeded)
    #expect(await replayService.snapshot().artifactSelectedIndices == [0, 1])
}

@Test @MainActor func artifactReplayMatchesCurvedPathShapeWithSharedEndpoints() async throws {
    let directory = FileManager.default.temporaryDirectory.appendingPathComponent(
        "PhotonStackArtifactPathReplay-\(UUID().uuidString)",
        isDirectory: true
    )
    let previewInput = directory.appendingPathComponent("preview.png")
    let replayInput = directory.appendingPathComponent("replay.png")
    defer {
        try? FileManager.default.removeItem(at: directory)
    }
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    try writeSolidPNG(to: previewInput, red: 0.05, green: 0.07, blue: 0.12, width: 100, height: 100)
    try writeSolidPNG(to: replayInput, red: 0.05, green: 0.07, blue: 0.12, width: 100, height: 100)

    let previewReport = #"{"type":"complete","command":"artifacts detect","width":100,"height":100,"trails":1,"removedTrails":0,"protectedMeteors":0,"items":[{"kind":"drone","confidence":0.9,"x1":10,"y1":50,"x2":90,"y2":50,"length":85.4,"width":2,"meanBrightness":0.05,"path":[{"x":10,"y":50},{"x":50,"y":35},{"x":90,"y":50}]}]}"#
    let primingService = RecordingProcessingService(artifactDetectOutputOverride: previewReport)
    var previewProject = PhotonStackProject(name: "Artifact Curved Path Preview")
    previewProject.addAssets(from: [previewInput])
    let primingModel = PhotonStackWorkspaceModel(
        project: previewProject,
        processingService: primingService,
        previewDirectory: directory.appendingPathComponent("preview-cache", isDirectory: true)
    )
    await primingModel.detectArtifactTrails()
    await primingModel.removeArtifactTrails(selectedIndices: [0], removeMeteors: false)
    let operation = try #require(primingModel.project.editGraph.operations.last { $0.kind == .artifactRemove })
    #expect(operation.parameters["selectedTrailsV1"]?.contains("\"path\"") == true)

    let replayReport = #"{"type":"complete","command":"artifacts detect","width":100,"height":100,"trails":2,"removedTrails":0,"protectedMeteors":0,"items":[{"kind":"drone","confidence":0.8,"x1":10,"y1":50,"x2":90,"y2":50,"length":85.4,"width":2,"meanBrightness":0.04,"path":[{"x":10,"y":50},{"x":50,"y":65},{"x":90,"y":50}]},{"kind":"drone","confidence":0.9,"x1":10,"y1":50,"x2":90,"y2":50,"length":85.4,"width":2,"meanBrightness":0.05,"path":[{"x":10,"y":50},{"x":50,"y":35},{"x":90,"y":50}]}]}"#
    let replayService = RecordingProcessingService(artifactDetectOutputOverride: replayReport)
    var replayParameters = operation.parameters
    replayParameters.removeValue(forKey: "input")
    replayParameters.removeValue(forKey: "output")
    var replayProject = PhotonStackProject(name: "Artifact Curved Path Replay")
    replayProject.addAssets(from: [replayInput])
    replayProject.editGraph = EditGraph(operations: [
        EditOperation(kind: .artifactRemove, parameters: replayParameters),
    ])
    let replayModel = PhotonStackWorkspaceModel(
        project: replayProject,
        processingService: replayService,
        previewDirectory: directory.appendingPathComponent("replay-cache", isDirectory: true)
    )

    replayModel.startReplayEditGraph()
    try await waitForStartedProcessingCompletion(in: replayModel)

    #expect(replayModel.latestJob?.status == .succeeded)
    #expect(await replayService.snapshot().artifactSelectedIndices == [1])
}

@Test @MainActor func artifactDetectionRejectsACompleteReportForTheWrongCommand() async {
    let service = RecordingProcessingService(
        artifactDetectOutputOverride: #"{"type":"complete","command":"artifacts remove","trails":0,"removedTrails":0,"protectedMeteors":0,"items":[]}"#
    )
    let image = URL(fileURLWithPath: "/tmp/photonstack-invalid-artifact-report.tiff")
    var project = PhotonStackProject(name: "Invalid Artifact Report")
    project.addAssets(from: [image])
    let model = PhotonStackWorkspaceModel(project: project, processingService: service)

    await model.detectArtifactTrails()

    #expect(model.latestJob?.status == .failed)
    #expect(model.artifactTrailReport == nil)
    #expect(model.artifactMarkedPreviewURL == nil)
}

@Test @MainActor func detectionReportsRejectPartialDimensionsAndUnknownTrailKinds() async {
    let artifactService = RecordingProcessingService(
        artifactDetectOutputOverride: #"{"type":"complete","command":"artifacts detect","width":100,"height":100,"trails":1,"removedTrails":0,"protectedMeteors":0,"items":[{"kind":"unknown","confidence":0.9,"x1":10,"y1":10,"x2":90,"y2":10,"length":80,"width":2,"meanBrightness":0.05}]}"#
    )
    let artifactInput = URL(fileURLWithPath: "/tmp/photonstack-unknown-artifact-kind.tiff")
    var artifactProject = PhotonStackProject(name: "Unknown Artifact Kind")
    artifactProject.addAssets(from: [artifactInput])
    let artifactModel = PhotonStackWorkspaceModel(
        project: artifactProject,
        processingService: artifactService
    )

    await artifactModel.detectArtifactTrails()

    #expect(artifactModel.latestJob?.status == .failed)
    #expect(artifactModel.artifactTrailReport == nil)

    let cloudService = RecordingProcessingService(
        cloudDetectOutputOverride: #"{"type":"complete","command":"clouds detect","width":100,"clouds":0,"removedClouds":0,"items":[]}"#
    )
    let cloudInput = URL(fileURLWithPath: "/tmp/photonstack-partial-cloud-dimensions.tiff")
    var cloudProject = PhotonStackProject(name: "Partial Cloud Dimensions")
    cloudProject.addAssets(from: [cloudInput])
    let cloudModel = PhotonStackWorkspaceModel(
        project: cloudProject,
        processingService: cloudService
    )

    await cloudModel.detectClouds()

    #expect(cloudModel.latestJob?.status == .failed)
    #expect(cloudModel.cloudRegionReport == nil)
}

@Test @MainActor func repeatedArtifactDetectionKeepsCurrentReportAndMarkerWhileProcessing() async throws {
    let service = RecordingProcessingService(artifactDetectDelayNanoseconds: 50_000_000)
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackArtifactRefresh-\(UUID().uuidString)", isDirectory: true)
    let image = directory.appendingPathComponent("input.png")
    defer {
        try? FileManager.default.removeItem(at: directory)
    }
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    try writeSolidPNG(to: image, red: 0.02, green: 0.02, blue: 0.02, width: 64, height: 64)
    var project = PhotonStackProject(name: "Artifact Refresh Test")
    project.addAssets(from: [image])
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: service,
        previewDirectory: directory
    )

    await model.detectArtifactTrails()
    #expect(model.artifactTrailReport != nil)
    let firstMarkedPreview = try #require(model.artifactMarkedPreviewURL)

    let refresh = Task { @MainActor in
        await model.detectArtifactTrails()
    }
    await waitForArtifactDetectCall(from: service, minimum: 2)
    #expect(model.artifactTrailReport != nil)
    #expect(model.artifactMarkedPreviewURL != nil)

    await refresh.value
    #expect(model.artifactTrailReport != nil)
    let refreshedMarkedPreview = try #require(model.artifactMarkedPreviewURL)
    #expect(refreshedMarkedPreview != firstMarkedPreview)
    #expect(FileManager.default.fileExists(atPath: firstMarkedPreview.path) == false)
    #expect(FileManager.default.fileExists(atPath: refreshedMarkedPreview.path))
}

@Test @MainActor func explicitArtifactRemovalRequiresUniqueCurrentSelection() async {
    let service = RecordingProcessingService()
    let image = URL(fileURLWithPath: "/tmp/photonstack-strict-artifact-selection.tiff")
    var project = PhotonStackProject(name: "Strict Artifact Selection")
    project.addAssets(from: [image])
    let model = PhotonStackWorkspaceModel(project: project, processingService: service)

    await model.removeArtifactTrails(selectedIndices: [0], removeMeteors: false)
    #expect(model.latestJob?.status == .failed)

    await model.detectArtifactTrails()
    let report = model.artifactTrailReport
    await model.removeArtifactTrails(selectedIndices: [], removeMeteors: false)
    #expect(model.latestJob?.status == .failed)
    #expect(model.artifactTrailReport == report)

    await model.removeArtifactTrails(selectedIndices: [1, 1], removeMeteors: false)
    #expect(model.latestJob?.status == .failed)
    #expect(model.artifactTrailReport == report)
    #expect(await service.snapshot().artifactSelectedIndices == nil)
    #expect(model.project.editGraph.operations.isEmpty)
}

@Test @MainActor func failedArtifactRemovalPreservesDetectionState() async throws {
    let service = RecordingProcessingService(failArtifactRemoval: true)
    let directory = FileManager.default.temporaryDirectory.appendingPathComponent(
        "PhotonStackFailedArtifactRemoval-\(UUID().uuidString)",
        isDirectory: true
    )
    let image = directory.appendingPathComponent("input.png")
    defer {
        try? FileManager.default.removeItem(at: directory)
    }
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    try writeSolidPNG(to: image, red: 0.05, green: 0.06, blue: 0.12, width: 240, height: 140)
    var project = PhotonStackProject(name: "Failed Artifact Removal")
    project.addAssets(from: [image])
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: service,
        previewDirectory: directory.appendingPathComponent("previews", isDirectory: true)
    )

    await model.detectArtifactTrails()
    let report = model.artifactTrailReport
    let markedPreview = model.artifactMarkedPreviewURL
    await model.removeArtifactTrails(selectedIndices: [1], removeMeteors: false)

    #expect(model.latestJob?.status == .failed)
    #expect(model.artifactTrailReport == report)
    #expect(model.artifactMarkedPreviewURL == markedPreview)
    #expect(model.project.editGraph.operations.isEmpty)
}

@Test @MainActor func cachedArtifactRemovalClearsMarkedPreviewAndRecordsOperation() async throws {
    let input = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackCachedArtifactInput-\(UUID().uuidString).png")
    let previewDirectory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackCachedArtifactTest-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: previewDirectory, withIntermediateDirectories: true)
    try writeSolidPNG(to: input, red: 0.1, green: 0.1, blue: 0.2, width: 240, height: 140)
    defer {
        try? FileManager.default.removeItem(at: input)
        try? FileManager.default.removeItem(at: previewDirectory)
    }

    var project = PhotonStackProject(name: "Cached Artifact Test")
    project.addAssets(from: [input])

    let primingModel = PhotonStackWorkspaceModel(
        project: project,
        processingService: RecordingProcessingService(),
        previewDirectory: previewDirectory
    )
    await primingModel.detectArtifactTrails()
    await primingModel.removeArtifactTrails(selectedIndices: [0, 1], removeMeteors: false)
    let cachedOutput = try #require(primingModel.previewURL)

    let cachedService = RecordingProcessingService()
    let cachedModel = PhotonStackWorkspaceModel(
        project: project,
        processingService: cachedService,
        previewDirectory: previewDirectory
    )
    await cachedModel.detectArtifactTrails()
    let markedPreview = try #require(cachedModel.artifactMarkedPreviewURL)

    await cachedModel.removeArtifactTrails(selectedIndices: [0, 1], removeMeteors: false)

    #expect(cachedModel.artifactTrailReport == nil)
    #expect(cachedModel.artifactMarkedPreviewURL == nil)
    #expect(FileManager.default.fileExists(atPath: markedPreview.path) == false)
    #expect(cachedModel.previewURL == cachedOutput)
    #expect(cachedModel.commandOutput.contains("Using cached preview"))
    let snapshot = await cachedService.snapshot()
    #expect(snapshot.artifactSelectedIndices == nil)
    let operation = try #require(cachedModel.project.editGraph.operations.first { $0.kind == .artifactRemove })
    #expect(operation.parameters["algorithmRevision"] == "satellite-trails-v6-path-shape")
    #expect(operation.parameters["selectedIndices"] == "0,1")
}

@Test @MainActor func cachedCloudRemovalClearsMarkedPreviewAndRecordsOperation() async throws {
    let input = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackCachedCloudInput-\(UUID().uuidString).png")
    let previewDirectory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackCachedCloudTest-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: previewDirectory, withIntermediateDirectories: true)
    try writeSolidPNG(to: input, red: 0.1, green: 0.1, blue: 0.2, width: 240, height: 140)
    defer {
        try? FileManager.default.removeItem(at: input)
        try? FileManager.default.removeItem(at: previewDirectory)
    }

    var project = PhotonStackProject(name: "Cached Cloud Test")
    project.addAssets(from: [input])

    let primingModel = PhotonStackWorkspaceModel(
        project: project,
        processingService: RecordingProcessingService(),
        previewDirectory: previewDirectory
    )
    await primingModel.detectClouds()
    await primingModel.removeClouds(selectedIndices: [1], strength: 0.4)
    let cachedOutput = try #require(primingModel.previewURL)

    let cachedService = RecordingProcessingService()
    let cachedModel = PhotonStackWorkspaceModel(
        project: project,
        processingService: cachedService,
        previewDirectory: previewDirectory
    )
    await cachedModel.detectClouds()
    #expect(cachedModel.cloudRegionReport != nil)
    let markedPreview = try #require(cachedModel.artifactMarkedPreviewURL)

    await cachedModel.removeClouds(selectedIndices: [1], strength: 0.4)

    #expect(cachedModel.cloudRegionReport == nil)
    #expect(cachedModel.artifactMarkedPreviewURL == nil)
    #expect(FileManager.default.fileExists(atPath: markedPreview.path) == false)
    #expect(cachedModel.previewURL == cachedOutput)
    #expect(cachedModel.commandOutput.contains("Using cached preview"))
    let snapshot = await cachedService.snapshot()
    #expect(snapshot.cloudSelectedIndices == nil)
    let operation = try #require(cachedModel.project.editGraph.operations.first { $0.kind == .cloudRemove })
    #expect(operation.parameters["selectedIndices"] == "1")
    #expect(operation.parameters["strength"] == "0.4")
}

@Test @MainActor func previewCacheChangesAcrossBuilds() async throws {
    let input = URL(fileURLWithPath: "/tmp/photonstack-build-cache.tiff")
    let previewDirectory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackBuildCacheTest-\(UUID().uuidString)", isDirectory: true)
    defer {
        try? FileManager.default.removeItem(at: previewDirectory)
    }

    var project = PhotonStackProject(name: "Build Cache Test")
    project.addAssets(from: [input])
    let firstModel = PhotonStackWorkspaceModel(
        project: project,
        processingService: RecordingProcessingService(),
        previewDirectory: previewDirectory,
        cacheBuildIdentity: "build-a"
    )
    await firstModel.autoStretchPreview()
    let firstOutput = try #require(firstModel.previewURL)

    let secondModel = PhotonStackWorkspaceModel(
        project: project,
        processingService: RecordingProcessingService(),
        previewDirectory: previewDirectory,
        cacheBuildIdentity: "build-b"
    )
    await secondModel.autoStretchPreview()
    let secondOutput = try #require(secondModel.previewURL)

    #expect(firstOutput != secondOutput)
}

@Test @MainActor func clearingPreviewCachePreservesReachableFilesAndRemovesOnlyOrphans() async throws {
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackClearCacheTest-\(UUID().uuidString)", isDirectory: true)
    let previewDirectory = directory.appendingPathComponent("previews", isDirectory: true)
    let input = directory.appendingPathComponent("input.tiff")
    defer {
        try? FileManager.default.removeItem(at: directory)
    }
    try FileManager.default.createDirectory(at: previewDirectory, withIntermediateDirectories: true)
    try writeSolidPNG(to: input, red: 0.1, green: 0.2, blue: 0.3, width: 16, height: 12)

    var project = PhotonStackProject(name: "Clear Preview Cache")
    project.addAssets(from: [input])
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: RecordingProcessingService(),
        previewDirectory: previewDirectory,
        cacheBuildIdentity: "clear-cache-test"
    )
    await model.autoStretchPreview()
    let currentPreview = try #require(model.previewURL)
    let currentReceipt = currentPreview.appendingPathExtension("photonstack-cache-receipt")
    let orphan = previewDirectory.appendingPathComponent("cache-orphan.tiff")
    let unrelated = previewDirectory.appendingPathComponent("manual-reference.tiff")
    let disguisedDirectory = previewDirectory.appendingPathComponent("cache-not-a-file.tiff", isDirectory: true)
    try Data("orphan".utf8).write(to: orphan)
    try Data("manual".utf8).write(to: unrelated)
    try FileManager.default.createDirectory(at: disguisedDirectory, withIntermediateDirectories: true)
    try Data("nested".utf8).write(to: disguisedDirectory.appendingPathComponent("keep.txt"))
    #expect(FileManager.default.fileExists(atPath: currentPreview.path))
    #expect(FileManager.default.fileExists(atPath: currentReceipt.path))

    model.requestPreviewCacheCleanup()
    let plan = try #require(model.pendingPreviewCacheCleanupPlan)

    #expect(plan.fileCount == 1)
    #expect(plan.totalBytes == 6)
    #expect(FileManager.default.fileExists(atPath: orphan.path))

    model.cancelPreviewCacheCleanup()
    #expect(model.pendingPreviewCacheCleanupPlan == nil)
    #expect(FileManager.default.fileExists(atPath: orphan.path))

    model.requestPreviewCacheCleanup()
    model.confirmPreviewCacheCleanup(try #require(model.pendingPreviewCacheCleanupPlan))

    #expect(FileManager.default.fileExists(atPath: currentPreview.path))
    #expect(FileManager.default.fileExists(atPath: currentReceipt.path))
    #expect(FileManager.default.fileExists(atPath: orphan.path) == false)
    #expect(FileManager.default.fileExists(atPath: unrelated.path))
    #expect(FileManager.default.fileExists(atPath: disguisedDirectory.path))
    #expect(model.previewURL == currentPreview)
    #expect(model.currentInputIsMissing == false)
    #expect(model.errorMessage == nil)
}

@Test @MainActor func clearingMissingPreviewCacheIsSuccessfulNoOp() {
    let previewDirectory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackMissingCacheTest-\(UUID().uuidString)", isDirectory: true)
    let model = PhotonStackWorkspaceModel(
        processingService: RecordingProcessingService(),
        previewDirectory: previewDirectory
    )

    model.clearPreviewCache()

    #expect(model.errorMessage == nil)
    #expect(model.commandOutput == model.localized(.previewCacheCleared))
}

@Test @MainActor func previewCacheCleanupProtectsFilesReferencedThroughSymbolicLinks() throws {
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackAliasedCacheProtection-\(UUID().uuidString)", isDirectory: true)
    let previewDirectory = directory.appendingPathComponent("previews", isDirectory: true)
    try FileManager.default.createDirectory(at: previewDirectory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    let protectedOutput = previewDirectory.appendingPathComponent("cache-protected.png")
    let protectedReceipt = protectedOutput.appendingPathExtension("photonstack-cache-receipt")
    let protectedAlias = directory.appendingPathComponent("protected-alias.png")
    let orphan = previewDirectory.appendingPathComponent("cache-orphan.png")
    try Data("protected".utf8).write(to: protectedOutput)
    try Data("receipt".utf8).write(to: protectedReceipt)
    try Data("orphan".utf8).write(to: orphan)
    try FileManager.default.createSymbolicLink(at: protectedAlias, withDestinationURL: protectedOutput)

    var project = PhotonStackProject(name: "Aliased Cache Protection")
    project.appendLayer(ProcessingLayer(name: "Aliased", kind: .baseImage, inputURL: protectedAlias))
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: RecordingProcessingService(),
        previewDirectory: previewDirectory
    )

    let plan = try #require(model.makePreviewCacheCleanupPlan())
    #expect(plan.fileCount == 1)
    #expect(plan.totalBytes == 6)

    model.clearPreviewCache(plan)

    #expect(FileManager.default.fileExists(atPath: protectedOutput.path))
    #expect(FileManager.default.fileExists(atPath: protectedReceipt.path))
    #expect(FileManager.default.fileExists(atPath: protectedAlias.path))
    #expect(FileManager.default.fileExists(atPath: orphan.path) == false)
}

@Test @MainActor func previewCacheCleanupProtectsLayerInputsInEditGraphHistory() async throws {
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackEditGraphHistoryCache-\(UUID().uuidString)", isDirectory: true)
    let previewDirectory = directory.appendingPathComponent("previews", isDirectory: true)
    try FileManager.default.createDirectory(at: previewDirectory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    let input = directory.appendingPathComponent("input.png")
    try writeSolidPNG(to: input, red: 0.1, green: 0.3, blue: 0.7, width: 16, height: 8)
    var project = PhotonStackProject(name: "Edit Graph History Cache")
    project.addAssets(from: [input])
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: RecordingProcessingService(),
        previewDirectory: previewDirectory,
        cacheBuildIdentity: "edit-graph-history-cache"
    )

    await model.autoStretchPreview()
    let operation = try #require(model.project.editGraph.operations.last)
    #expect(model.updateEditOperationParameter(
        operation.id,
        key: "targetBackground",
        value: "0.35"
    ))
    model.startReplayEditGraph()
    try await waitForStartedProcessingCompletion(in: model, minimumJobCount: 2)
    let historyOnlyLayerInput = try #require(model.project.layers.first {
        $0.parameters["operationID"] == operation.id.uuidString
    }?.inputURL)

    #expect(model.updateEditOperationParameter(
        operation.id,
        key: "targetBackground",
        value: "0.45"
    ))
    model.startReplayEditGraph()
    try await waitForStartedProcessingCompletion(in: model, minimumJobCount: 3)
    let currentLayerInput = try #require(model.project.layers.first {
        $0.parameters["operationID"] == operation.id.uuidString
    }?.inputURL)
    #expect(currentLayerInput != historyOnlyLayerInput)
    #expect(FileManager.default.fileExists(atPath: historyOnlyLayerInput.path))

    let orphan = previewDirectory.appendingPathComponent("replay-orphan.png")
    try Data("orphan".utf8).write(to: orphan)
    let plan = try #require(model.makePreviewCacheCleanupPlan())
    #expect(plan.fileCount == 1)
    model.clearPreviewCache(plan)

    #expect(FileManager.default.fileExists(atPath: orphan.path) == false)
    #expect(FileManager.default.fileExists(atPath: historyOnlyLayerInput.path))
    model.undoEditGraphChange()
    #expect(model.project.layers.first(where: {
        $0.parameters["operationID"] == operation.id.uuidString
    })?.inputURL == historyOnlyLayerInput)
    #expect(FileManager.default.fileExists(atPath: historyOnlyLayerInput.path))

    let redoOrphan = previewDirectory.appendingPathComponent("replay-redo-orphan.png")
    try Data("redo-orphan".utf8).write(to: redoOrphan)
    let redoPlan = try #require(model.makePreviewCacheCleanupPlan())
    #expect(redoPlan.fileCount == 1)
    model.clearPreviewCache(redoPlan)

    #expect(FileManager.default.fileExists(atPath: redoOrphan.path) == false)
    #expect(FileManager.default.fileExists(atPath: currentLayerInput.path))
    model.redoEditGraphChange()
    #expect(model.project.layers.first(where: {
        $0.parameters["operationID"] == operation.id.uuidString
    })?.inputURL == currentLayerInput)
}

@Test @MainActor func previewCacheCleanupRecoversUnreferencedManagedWorkflowFiles() throws {
    let previewDirectory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackNestedCacheCleanup-\(UUID().uuidString)", isDirectory: true)
    let staleWorkflow = previewDirectory.appendingPathComponent("workflow-stale", isDirectory: true)
    let currentWorkflow = previewDirectory.appendingPathComponent("workflow-current", isDirectory: true)
    let manualDirectory = previewDirectory.appendingPathComponent("manual-work", isDirectory: true)
    try FileManager.default.createDirectory(at: staleWorkflow, withIntermediateDirectories: true)
    try FileManager.default.createDirectory(at: currentWorkflow, withIntermediateDirectories: true)
    try FileManager.default.createDirectory(at: manualDirectory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: previewDirectory)
    }

    let staleFile = staleWorkflow.appendingPathComponent("stack-temporary.tiff")
    let currentOutput = currentWorkflow.appendingPathComponent("stack-final.tiff")
    let currentIntermediate = currentWorkflow.appendingPathComponent("stack-intermediate.tiff")
    let manualFile = manualDirectory.appendingPathComponent("cache-orphan.tiff")
    try Data("stale".utf8).write(to: staleFile)
    try Data("final".utf8).write(to: currentOutput)
    try Data("intermediate".utf8).write(to: currentIntermediate)
    try Data("manual".utf8).write(to: manualFile)

    var project = PhotonStackProject(name: "Nested Cache Cleanup")
    project.appendArtifact(
        ProcessingArtifact(kind: .stackMaster, name: "Current Stack", outputURLs: [currentOutput])
    )
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: RecordingProcessingService(),
        previewDirectory: previewDirectory
    )

    let plan = try #require(model.makePreviewCacheCleanupPlan())
    #expect(plan.fileCount == 2)
    #expect(plan.totalBytes == 17)

    model.clearPreviewCache(plan)

    #expect(FileManager.default.fileExists(atPath: staleFile.path) == false)
    #expect(FileManager.default.fileExists(atPath: staleWorkflow.path) == false)
    #expect(FileManager.default.fileExists(atPath: currentOutput.path))
    #expect(FileManager.default.fileExists(atPath: currentIntermediate.path) == false)
    #expect(FileManager.default.fileExists(atPath: currentWorkflow.path))
    #expect(FileManager.default.fileExists(atPath: manualFile.path))
}

@Test @MainActor func defaultPreviewCachesAreIsolatedBetweenProjects() async throws {
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackProjectCacheIsolation-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    let input = directory.appendingPathComponent("input.png")
    try writeSolidPNG(to: input, red: 0.2, green: 0.3, blue: 0.4, width: 16, height: 8)
    var firstProject = PhotonStackProject(name: "First Cache Project")
    firstProject.addAssets(from: [input])
    var secondProject = PhotonStackProject(name: "Second Cache Project")
    secondProject.addAssets(from: [input])

    let firstModel = PhotonStackWorkspaceModel(
        project: firstProject,
        processingService: RecordingProcessingService(),
        cacheBuildIdentity: "project-cache-isolation"
    )
    let secondModel = PhotonStackWorkspaceModel(
        project: secondProject,
        processingService: RecordingProcessingService(),
        cacheBuildIdentity: "project-cache-isolation"
    )
    await firstModel.autoStretchPreview()
    await secondModel.autoStretchPreview()
    let firstOutput = try #require(firstModel.previewURL)
    let secondOutput = try #require(secondModel.previewURL)

    #expect(firstOutput.deletingLastPathComponent() != secondOutput.deletingLastPathComponent())
    #expect(firstOutput.deletingLastPathComponent().lastPathComponent == firstProject.id.uuidString)
    #expect(secondOutput.deletingLastPathComponent().lastPathComponent == secondProject.id.uuidString)

    firstModel.clearPreviewCache()

    #expect(FileManager.default.fileExists(atPath: firstOutput.path))
    #expect(FileManager.default.fileExists(atPath: secondOutput.path))
}

@Test @MainActor func creatingProjectCleansOldMarkersBeforeSwitchingManagedCacheDirectory() async throws {
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackManagedCacheTransition-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    let input = directory.appendingPathComponent("input.png")
    try writeSolidPNG(to: input, red: 0.02, green: 0.02, blue: 0.02, width: 32, height: 24)
    var project = PhotonStackProject(name: "Old Managed Cache")
    project.addAssets(from: [input])
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: RecordingProcessingService(),
        cacheBuildIdentity: "managed-cache-transition"
    )

    await model.detectArtifactTrails()
    let oldMarker = try #require(model.artifactMarkedPreviewURL)
    #expect(FileManager.default.fileExists(atPath: oldMarker.path))
    #expect(oldMarker.deletingLastPathComponent().lastPathComponent == project.id.uuidString)

    model.createProject(template: .singleFrame, name: "New Managed Cache")
    let newProjectID = model.project.id
    model.importAssets(from: [input])
    await model.autoStretchPreview()
    let newPreview = try #require(model.previewURL)

    #expect(FileManager.default.fileExists(atPath: oldMarker.path) == false)
    #expect(newPreview.deletingLastPathComponent().lastPathComponent == newProjectID.uuidString)
}

@Test @MainActor func invalidPreviewCacheIsReprocessed() async throws {
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackInvalidCacheTest-\(UUID().uuidString)", isDirectory: true)
    let input = directory.appendingPathComponent("input.png")
    let previewDirectory = directory.appendingPathComponent("previews", isDirectory: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    try writeSolidPNG(to: input, red: 0.2, green: 0.3, blue: 0.4, width: 32, height: 24)

    var project = PhotonStackProject(name: "Invalid Cache Test")
    project.addAssets(from: [input])
    let primingModel = PhotonStackWorkspaceModel(
        project: project,
        processingService: RecordingProcessingService(),
        previewDirectory: previewDirectory,
        cacheBuildIdentity: "build-a"
    )
    await primingModel.autoStretchPreview()
    let cachedOutput = try #require(primingModel.previewURL)
    try Data().write(to: cachedOutput)

    let service = RecordingProcessingService()
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: service,
        previewDirectory: previewDirectory,
        cacheBuildIdentity: "build-a"
    )
    await model.autoStretchPreview()

    #expect(await service.snapshot().autoStretchCalls == 1)
    #expect(model.commandOutput.contains("Using cached preview") == false)
}

@Test @MainActor func headerOnlyPreviewCacheIsReprocessed() async throws {
    let input = URL(fileURLWithPath: "/tmp/photonstack-header-only-cache.tiff")
    let previewDirectory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackHeaderOnlyCacheTest-\(UUID().uuidString)", isDirectory: true)
    defer {
        try? FileManager.default.removeItem(at: previewDirectory)
    }

    var project = PhotonStackProject(name: "Header-Only Cache Test")
    project.addAssets(from: [input])
    let primingModel = PhotonStackWorkspaceModel(
        project: project,
        processingService: RecordingProcessingService(),
        previewDirectory: previewDirectory,
        cacheBuildIdentity: "build-a"
    )
    await primingModel.autoStretchPreview()
    let cachedOutput = try #require(primingModel.previewURL)
    try writeSolidPNG(to: cachedOutput, red: 0.2, green: 0.3, blue: 0.4, width: 64, height: 64)
    let validData = try Data(contentsOf: cachedOutput)
    #expect(validData.count > 33)
    try Data(validData.prefix(33)).write(to: cachedOutput)

    let source = try #require(CGImageSourceCreateWithURL(cachedOutput as CFURL, nil))
    #expect(CGImageSourceCopyPropertiesAtIndex(source, 0, nil) != nil)
    #expect(CGImageSourceCreateImageAtIndex(source, 0, nil) == nil)

    let service = RecordingProcessingService()
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: service,
        previewDirectory: previewDirectory,
        cacheBuildIdentity: "build-a"
    )
    await model.autoStretchPreview()

    #expect(await service.snapshot().autoStretchCalls == 1)
    #expect(model.commandOutput.contains("Using cached preview") == false)
}

@Test func atomicLocalImageWriterPreservesExistingOutputWhenRenderingFails() throws {
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackAtomicLocalRenderFailure-\(UUID().uuidString)", isDirectory: true)
    let output = directory.appendingPathComponent("preview.png")
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    try Data("original image".utf8).write(to: output)

    do {
        try AtomicLocalImageWriter.write(to: output, validate: { _ in true }) { temporaryOutput in
            try Data("partial image".utf8).write(to: temporaryOutput)
            throw CocoaError(.fileWriteUnknown)
        }
        Issue.record("Expected the render writer to fail")
    } catch is CocoaError {
    }

    #expect(try Data(contentsOf: output) == Data("original image".utf8))
    let leftovers = try FileManager.default.contentsOfDirectory(atPath: directory.path)
        .filter { $0.contains("photonstack-render-") }
    #expect(leftovers.isEmpty)
}

@Test func atomicLocalImageWriterPreservesExistingOutputWhenValidationFails() throws {
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackAtomicLocalValidationFailure-\(UUID().uuidString)", isDirectory: true)
    let output = directory.appendingPathComponent("preview.png")
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    try Data("original image".utf8).write(to: output)

    do {
        try AtomicLocalImageWriter.write(to: output, validate: { _ in false }) { temporaryOutput in
            try Data("invalid replacement".utf8).write(to: temporaryOutput)
        }
        Issue.record("Expected image validation to fail")
    } catch AtomicLocalImageWriterError.invalidOutput {
    }

    #expect(try Data(contentsOf: output) == Data("original image".utf8))
    let leftovers = try FileManager.default.contentsOfDirectory(atPath: directory.path)
        .filter { $0.contains("photonstack-render-") }
    #expect(leftovers.isEmpty)
}

@Test func atomicLocalImageWriterReplacesExistingOutputAfterValidation() throws {
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackAtomicLocalRenderSuccess-\(UUID().uuidString)", isDirectory: true)
    let output = directory.appendingPathComponent("preview.png")
    let replacement = Data("replacement image".utf8)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    try Data("original image".utf8).write(to: output)

    try AtomicLocalImageWriter.write(
        to: output,
        validate: { try Data(contentsOf: $0) == replacement }
    ) { temporaryOutput in
        try replacement.write(to: temporaryOutput)
    }

    #expect(try Data(contentsOf: output) == replacement)
    let leftovers = try FileManager.default.contentsOfDirectory(atPath: directory.path)
        .filter { $0.contains("photonstack-render-") }
    #expect(leftovers.isEmpty)
}

@Test @MainActor func batchOutputChangesWithSourceAndBuildIdentity() async throws {
    let input = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackBatchCacheInput-\(UUID().uuidString).tiff")
    let outputDirectory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackBatchCacheOutput-\(UUID().uuidString)", isDirectory: true)
    try Data("first".utf8).write(to: input)
    defer {
        try? FileManager.default.removeItem(at: input)
        try? FileManager.default.removeItem(at: outputDirectory)
    }

    var project = PhotonStackProject(name: "Batch Cache Test")
    project.addAssets(from: [input])
    project.setBatchOutputDirectory(outputDirectory)

    let firstModel = PhotonStackWorkspaceModel(
        project: project,
        processingService: RecordingProcessingService(),
        cacheBuildIdentity: "build-a"
    )
    firstModel.enqueueCurrent(.stretch)
    firstModel.startBatchQueue()
    try await waitForBatchCompletion(in: firstModel)
    let firstOutput = try #require(firstModel.batchQueue.first?.outputURL)

    try Data("second-version".utf8).write(to: input)
    let modifiedSourceModel = PhotonStackWorkspaceModel(
        project: project,
        processingService: RecordingProcessingService(),
        cacheBuildIdentity: "build-a"
    )
    modifiedSourceModel.enqueueCurrent(.stretch)
    modifiedSourceModel.startBatchQueue()
    try await waitForBatchCompletion(in: modifiedSourceModel)
    let modifiedSourceOutput = try #require(modifiedSourceModel.batchQueue.first?.outputURL)

    let newBuildModel = PhotonStackWorkspaceModel(
        project: project,
        processingService: RecordingProcessingService(),
        cacheBuildIdentity: "build-b"
    )
    newBuildModel.enqueueCurrent(.stretch)
    newBuildModel.startBatchQueue()
    try await waitForBatchCompletion(in: newBuildModel)
    let newBuildOutput = try #require(newBuildModel.batchQueue.first?.outputURL)

    #expect(firstOutput != modifiedSourceOutput)
    #expect(modifiedSourceOutput != newBuildOutput)
}

@Test @MainActor func batchQueueHonorsOutputFormatInPreviewCache() async throws {
    let input = URL(fileURLWithPath: "/tmp/photonstack-batch-format-input.tiff")

    for format in BatchOutputFormat.allCases {
        var project = PhotonStackProject(name: "Batch Format \(format.rawValue)")
        project.addAssets(from: [input])
        let previewDirectory = FileManager.default.temporaryDirectory
            .appendingPathComponent("PhotonStackBatchFormat-\(format.rawValue)-\(UUID().uuidString)", isDirectory: true)
        defer {
            try? FileManager.default.removeItem(at: previewDirectory)
        }

        let model = PhotonStackWorkspaceModel(
            project: project,
            processingService: RecordingProcessingService(),
            previewDirectory: previewDirectory
        )
        model.setBatchOutputFormat(format)
        model.enqueueCurrent(.stretch)
        model.startBatchQueue()
        try await waitForBatchCompletion(in: model)

        let item = try #require(model.batchQueue.first)
        #expect(item.status == .succeeded)
        #expect(item.outputURL?.pathExtension == format.fileExtension)
    }
}

@Test @MainActor func batchQueueRunsEveryExposedOperation() async throws {
    let input = URL(fileURLWithPath: "/tmp/photonstack-batch-operations-input.tiff")
    var project = PhotonStackProject(name: "Batch Operations")
    project.addAssets(from: [input])
    let previewDirectory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackBatchOperations-\(UUID().uuidString)", isDirectory: true)
    defer {
        try? FileManager.default.removeItem(at: previewDirectory)
    }

    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: RecordingProcessingService(),
        previewDirectory: previewDirectory
    )
    let operations: [ProcessingOperationKind] = [.stretch, .denoise, .sharpen, .starReduce]
    for operation in operations {
        model.enqueueCurrent(operation)
    }

    model.startBatchQueue()
    try await waitForBatchCompletion(in: model)

    #expect(model.batchQueue.map(\.kind) == operations)
    #expect(model.batchQueue.allSatisfy { $0.status == .succeeded })
    #expect(model.batchQueue.allSatisfy { $0.outputURL?.pathExtension == "png" })
}

@Test @MainActor func batchQueueRejectsUnsupportedOperationsBeforeEnqueueing() {
    let input = URL(fileURLWithPath: "/tmp/photonstack-unsupported-batch-input.tiff")
    var project = PhotonStackProject(name: "Unsupported Batch Operations")
    project.addAssets(from: [input])
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: RecordingProcessingService()
    )

    model.enqueueCurrent(.mosaic)
    #expect(model.batchQueue.isEmpty)
    #expect(model.errorMessage == "Unsupported batch queue operation")

    let allAssetsModel = PhotonStackWorkspaceModel(
        project: project,
        processingService: RecordingProcessingService()
    )
    allAssetsModel.enqueueAllAssets(.export)
    #expect(allAssetsModel.batchQueue.isEmpty)
    #expect(allAssetsModel.errorMessage == "Unsupported batch queue operation")

    let supportedModel = PhotonStackWorkspaceModel(
        project: project,
        processingService: RecordingProcessingService()
    )
    supportedModel.enqueueCurrent(.comaReduce)
    #expect(supportedModel.batchQueue.map(\.kind) == [.comaReduce])
    #expect(supportedModel.errorMessage == nil)
}

@Test @MainActor func batchQueueReprocessesAnUnverifiedExistingOutput() async throws {
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackUnverifiedBatchOutput-\(UUID().uuidString)", isDirectory: true)
    let outputDirectory = directory.appendingPathComponent("outputs", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    let input = directory.appendingPathComponent("input.png")
    try writeSolidPNG(to: input, red: 0.2, green: 0.3, blue: 0.4, width: 16, height: 8)
    var project = PhotonStackProject(name: "Unverified Batch Output")
    project.addAssets(from: [input])
    project.setBatchOutputDirectory(outputDirectory)

    let firstService = RecordingProcessingService()
    let firstModel = PhotonStackWorkspaceModel(project: project, processingService: firstService)
    firstModel.enqueueCurrent(.stretch)
    firstModel.startBatchQueue()
    try await waitForBatchCompletion(in: firstModel)
    let existingOutput = try #require(firstModel.batchQueue.first?.outputURL)
    #expect(FileManager.default.fileExists(atPath: existingOutput.path))
    #expect(await firstService.snapshot().autoStretchCalls == 1)

    let secondService = RecordingProcessingService()
    let secondModel = PhotonStackWorkspaceModel(project: project, processingService: secondService)
    secondModel.enqueueCurrent(.stretch)
    secondModel.startBatchQueue()
    try await waitForBatchCompletion(in: secondModel)

    #expect(secondModel.batchQueue.first?.outputURL == existingOutput)
    #expect(await secondService.snapshot().autoStretchCalls == 1)
    #expect(secondModel.commandOutput.contains("Using cached preview") == false)
}

@Test @MainActor func batchExportReplaysOnlyThePreviewedAssetBranchWithRecordedParameters() async throws {
    let service = RecordingProcessingService()
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackBatchExportBranch-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    let firstInput = directory.appendingPathComponent("first.png")
    let secondInput = directory.appendingPathComponent("second.png")
    try writeSolidPNG(to: firstInput, red: 1, green: 0, blue: 0, width: 16, height: 8)
    try writeSolidPNG(to: secondInput, red: 0, green: 0, blue: 1, width: 16, height: 8)
    var project = PhotonStackProject(name: "Batch Export Branch")
    project.addAssets(from: [firstInput, secondInput])
    project.setBatchMaxConcurrentTasks(1)
    var parameters = ProcessingParameters()
    parameters.stretchTargetBackground = 0.37
    let model = PhotonStackWorkspaceModel(
        project: project,
        parameters: parameters,
        processingService: service,
        previewDirectory: directory.appendingPathComponent("previews", isDirectory: true)
    )

    model.enqueueAllAssets(.stretch)
    model.startBatchQueue()
    try await waitForBatchCompletion(in: model)

    #expect(model.selectedAsset?.originalURL == firstInput)
    #expect(model.previewURL == model.batchQueue.last?.outputURL)
    #expect(model.project.editGraph.operations.map { $0.parameters["input"] } == [firstInput.path, secondInput.path])

    var updatedParameters = model.parameters
    updatedParameters.stretchTargetBackground = 0.11
    model.setProcessingParameters(updatedParameters)
    await model.exportCurrentImage(to: directory.appendingPathComponent("final.tiff"))

    let snapshot = await service.snapshot()
    #expect(snapshot.autoStretchInputs == [firstInput, secondInput, secondInput])
    #expect(snapshot.autoStretchTargets == [0.37, 0.37, 0.37])
    #expect(snapshot.autoStretchOutputs.last?.pathExtension == "tiff")
    #expect(snapshot.convertInput == snapshot.autoStretchOutputs.last)
}

@Test @MainActor func batchEditGraphReplayUsesOnlyThePreviewedAssetBranch() async throws {
    let service = RecordingProcessingService()
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackBatchReplayBranch-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    let firstInput = directory.appendingPathComponent("first.png")
    let secondInput = directory.appendingPathComponent("second.png")
    try writeSolidPNG(to: firstInput, red: 1, green: 0, blue: 0, width: 16, height: 8)
    try writeSolidPNG(to: secondInput, red: 0, green: 0, blue: 1, width: 16, height: 8)
    var project = PhotonStackProject(name: "Batch Replay Branch")
    project.addAssets(from: [firstInput, secondInput])
    var parameters = ProcessingParameters()
    parameters.stretchTargetBackground = 0.37
    let model = PhotonStackWorkspaceModel(
        project: project,
        parameters: parameters,
        processingService: service,
        previewDirectory: directory.appendingPathComponent("previews", isDirectory: true)
    )

    model.enqueueAllAssets(.stretch)
    model.startBatchQueue()
    try await waitForBatchCompletion(in: model)
    #expect(model.selectedAsset?.originalURL == firstInput)
    #expect(model.previewURL == model.batchQueue.last?.outputURL)

    model.startReplayEditGraph()
    try await waitForStartedProcessingCompletion(in: model)

    let snapshot = await service.snapshot()
    #expect(snapshot.autoStretchInputs == [firstInput, secondInput, secondInput])
    #expect(snapshot.autoStretchTargets == [0.37, 0.37, 0.37])
    #expect(model.previewURL == snapshot.autoStretchOutputs.last)

    await model.exportCurrentImage(to: directory.appendingPathComponent("replayed-final.tiff"))
    let exportSnapshot = await service.snapshot()
    #expect(exportSnapshot.autoStretchInputs == [firstInput, secondInput, secondInput, secondInput])
    #expect(exportSnapshot.autoStretchTargets == [0.37, 0.37, 0.37, 0.37])
    #expect(exportSnapshot.convertInput == exportSnapshot.autoStretchOutputs.last)
}

@Test @MainActor func savedProjectRestoresSelectedAssetAndPreviewBranchForExport() async throws {
    let processingService = RecordingProcessingService()
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackSavedPreviewBranch-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    let firstInput = directory.appendingPathComponent("first.png")
    let secondInput = directory.appendingPathComponent("second.png")
    try writeSolidPNG(to: firstInput, red: 1, green: 0, blue: 0, width: 16, height: 8)
    try writeSolidPNG(to: secondInput, red: 0, green: 0, blue: 1, width: 16, height: 8)
    var project = PhotonStackProject(name: "Saved Preview Branch")
    project.addAssets(from: [firstInput, secondInput])
    var parameters = ProcessingParameters()
    parameters.stretchTargetBackground = 0.37
    let model = PhotonStackWorkspaceModel(
        project: project,
        parameters: parameters,
        processingService: processingService,
        previewDirectory: directory.appendingPathComponent("previews", isDirectory: true)
    )

    model.enqueueAllAssets(.stretch)
    model.startBatchQueue()
    try await waitForBatchCompletion(in: model)
    let finalPreview = try #require(model.previewURL)
    #expect(model.selectedAsset?.originalURL == firstInput)
    #expect(finalPreview == model.batchQueue.last?.outputURL)

    let projectDirectory = directory.appendingPathComponent("project", isDirectory: true)
    await model.saveProject(to: projectDirectory)

    let restoredService = RecordingProcessingService()
    let restored = PhotonStackWorkspaceModel(
        processingService: restoredService,
        previewDirectory: directory.appendingPathComponent("restored-previews", isDirectory: true)
    )
    await restored.loadProject(from: projectDirectory)

    #expect(restored.selectedAsset?.originalURL == firstInput)
    #expect(restored.previewURL == finalPreview)

    await restored.exportCurrentImage(to: directory.appendingPathComponent("restored-final.tiff"))
    let snapshot = await restoredService.snapshot()
    #expect(snapshot.autoStretchInputs == [secondInput])
    #expect(snapshot.autoStretchTargets == [0.37])
    #expect(snapshot.convertInput == snapshot.autoStretchOutputs.last)

    try FileManager.default.removeItem(at: finalPreview)
    let missingPreviewService = RecordingProcessingService()
    let missingPreviewModel = PhotonStackWorkspaceModel(
        processingService: missingPreviewService,
        previewDirectory: directory.appendingPathComponent("missing-preview-replay", isDirectory: true)
    )
    await missingPreviewModel.loadProject(from: projectDirectory)
    #expect(missingPreviewModel.previewURL == firstInput)

    missingPreviewModel.startReplayEditGraph()
    try await waitForStartedProcessingCompletion(in: missingPreviewModel, minimumJobCount: 2)
    let missingPreviewSnapshot = await missingPreviewService.snapshot()
    #expect(missingPreviewSnapshot.autoStretchInputs == [secondInput])
    #expect(missingPreviewSnapshot.autoStretchTargets == [0.37])
    #expect(missingPreviewModel.previewURL == missingPreviewSnapshot.autoStretchOutputs.last)

    await missingPreviewModel.exportCurrentImage(to: directory.appendingPathComponent("missing-preview-final.tiff"))
    let missingPreviewExportSnapshot = await missingPreviewService.snapshot()
    #expect(missingPreviewExportSnapshot.autoStretchInputs == [secondInput, secondInput])
    #expect(missingPreviewExportSnapshot.autoStretchTargets == [0.37, 0.37])
    #expect(missingPreviewExportSnapshot.convertInput == missingPreviewExportSnapshot.autoStretchOutputs.last)
}

@Test @MainActor func initializedWorkspaceRestoresPersistedCanvasSelectionAndLayer() throws {
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackInitializedWorkspaceRestore-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    let firstInput = directory.appendingPathComponent("first.png")
    let secondInput = directory.appendingPathComponent("second.png")
    let layerInput = directory.appendingPathComponent("layer.png")
    try writeSolidPNG(to: firstInput, red: 1, green: 0, blue: 0)
    try writeSolidPNG(to: secondInput, red: 0, green: 0, blue: 1)
    try writeSolidPNG(to: layerInput, red: 0, green: 1, blue: 0)

    var project = PhotonStackProject(name: "Initialized Workspace Restore")
    project.addAssets(from: [firstInput, secondInput])
    let selectedID = try #require(project.assets.last?.id)
    let operation = EditOperation(kind: .denoise, parameters: ["output": layerInput.path])
    project.appendOperation(operation)
    let layer = ProcessingLayer(name: "Restored Layer", kind: .adjustment, inputURL: layerInput)
    project.appendLayer(layer)
    project.workspaceState = ProjectWorkspaceState(
        selectedAssetID: selectedID,
        activeLayerID: layer.id,
        previewURL: layerInput,
        previewOperationID: operation.id,
        editGraphNeedsReplay: false,
        canvasMode: .layerComposite
    )

    let model = PhotonStackWorkspaceModel(project: project, processingService: RecordingProcessingService())

    #expect(model.selectedAssetID == selectedID)
    #expect(model.activeLayerID == layer.id)
    #expect(model.previewURL == layerInput)
    #expect(model.previewOperationID == operation.id)
    #expect(model.canvasMode == .layerComposite)
}

@Test @MainActor func initializedWorkspaceRepairsDanglingPersistedSelectionReferences() throws {
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackInitializedWorkspaceRepair-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    let input = directory.appendingPathComponent("source.png")
    try writeSolidPNG(to: input, red: 0.2, green: 0.3, blue: 0.4)
    var project = PhotonStackProject(name: "Initialized Workspace Repair")
    project.addAssets(from: [input])
    project.workspaceState = ProjectWorkspaceState(
        selectedAssetID: UUID(),
        activeLayerID: UUID(),
        previewURL: directory.appendingPathComponent("missing-preview.png"),
        previewOperationID: UUID(),
        editGraphNeedsReplay: false,
        canvasMode: .sourcePreview
    )

    let model = PhotonStackWorkspaceModel(project: project, processingService: RecordingProcessingService())

    #expect(model.selectedAssetID == project.assets.first?.id)
    #expect(model.activeLayerID == nil)
    #expect(model.previewURL == input)
    #expect(model.previewOperationID == nil)
    #expect(model.editGraphNeedsReplay == false)
}

@Test @MainActor func ambiguousLegacyPreviewProvenanceBlocksReplayAndExport() async throws {
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackAmbiguousPreview-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    let source = directory.appendingPathComponent("source.png")
    let sharedPreview = directory.appendingPathComponent("shared-preview.png")
    try writeSolidPNG(to: source, red: 0.2, green: 0.4, blue: 0.6, width: 16, height: 8)
    try writeSolidPNG(to: sharedPreview, red: 0.4, green: 0.6, blue: 0.8, width: 16, height: 8)
    var project = PhotonStackProject(name: "Ambiguous Preview")
    project.addAssets(from: [source])
    let assetID = try #require(project.assets.first?.id)
    project.editGraph = EditGraph(operations: [
        EditOperation(
            kind: .stretch,
            parameters: [
                "input": source.path,
                "output": sharedPreview.path,
                "targetBackground": "0.25",
            ]
        ),
        EditOperation(
            kind: .denoise,
            parameters: [
                "input": source.path,
                "output": sharedPreview.path,
                "amount": "0.4",
                "chroma": "0.5",
                "radius": "1",
            ]
        ),
    ])
    project.workspaceState = ProjectWorkspaceState(
        selectedAssetID: assetID,
        previewURL: sharedPreview,
        canvasMode: .sourcePreview
    )
    let projectDirectory = directory.appendingPathComponent("project", isDirectory: true)
    let repository = ProjectRepository()
    try await repository.save(project, to: projectDirectory)
    let service = RecordingProcessingService()
    let model = PhotonStackWorkspaceModel(
        processingService: service,
        projectRepository: repository,
        previewDirectory: directory.appendingPathComponent("previews", isDirectory: true)
    )

    await model.loadProject(from: projectDirectory)

    #expect(model.previewURL == sharedPreview)
    #expect(model.previewOperationID == nil)
    #expect(model.canExportCurrentImage == false)

    model.enqueueCurrent(.export)
    #expect(model.batchQueue.isEmpty)
    #expect(model.errorMessage == "Unsupported batch queue operation")

    await model.exportCurrentImage(to: directory.appendingPathComponent("blocked-export.tiff"))
    #expect(await service.snapshot().convertInputs.isEmpty)
    #expect(model.errorMessage == model.localized(.editGraphAmbiguousProvenance))

    model.startReplayEditGraph()
    try await waitForStartedProcessingCompletion(in: model, minimumJobCount: 2)
    let snapshot = await service.snapshot()
    #expect(snapshot.autoStretchInputs.isEmpty)
    #expect(snapshot.denoiseInput == nil)
    #expect(model.latestJob?.status == .failed)
    #expect(model.errorMessage == model.localized(.editGraphAmbiguousProvenance))
}

@Test @MainActor func explicitPreviewOperationIdentityDisambiguatesSharedOutputPath() async throws {
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackExplicitPreviewIdentity-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    let source = directory.appendingPathComponent("source.png")
    let sharedPreview = directory.appendingPathComponent("shared-preview.png")
    try writeSolidPNG(to: source, red: 0.15, green: 0.35, blue: 0.55, width: 16, height: 8)
    try writeSolidPNG(to: sharedPreview, red: 0.35, green: 0.55, blue: 0.75, width: 16, height: 8)
    var project = PhotonStackProject(name: "Explicit Preview Identity")
    project.addAssets(from: [source])
    let assetID = try #require(project.assets.first?.id)
    let stretch = EditOperation(
        kind: .stretch,
        parameters: [
            "input": source.path,
            "output": sharedPreview.path,
            "targetBackground": "0.31",
        ]
    )
    let denoise = EditOperation(
        kind: .denoise,
        parameters: [
            "input": source.path,
            "output": sharedPreview.path,
            "amount": "0.4",
            "chroma": "0.5",
            "radius": "1",
        ]
    )
    project.editGraph = EditGraph(operations: [stretch, denoise])
    project.workspaceState = ProjectWorkspaceState(
        selectedAssetID: assetID,
        previewURL: sharedPreview,
        previewOperationID: stretch.id,
        canvasMode: .sourcePreview
    )
    let projectDirectory = directory.appendingPathComponent("project", isDirectory: true)
    let repository = ProjectRepository()
    try await repository.save(project, to: projectDirectory)
    let service = RecordingProcessingService()
    let model = PhotonStackWorkspaceModel(
        processingService: service,
        projectRepository: repository,
        previewDirectory: directory.appendingPathComponent("previews", isDirectory: true)
    )

    await model.loadProject(from: projectDirectory)

    #expect(model.previewOperationID == stretch.id)
    #expect(model.canExportCurrentImage)
    await model.exportCurrentImage(to: directory.appendingPathComponent("explicit-export.tiff"))

    let snapshot = await service.snapshot()
    #expect(snapshot.autoStretchInputs == [source])
    #expect(snapshot.autoStretchTargets == [0.31])
    #expect(snapshot.denoiseInput == nil)
    #expect(snapshot.convertInput == snapshot.autoStretchOutputs.last)
    #expect(model.latestJob?.status == .succeeded)
}

@Test @MainActor func explicitReplayAnchorRejectsAmbiguousPredecessorOutputs() async throws {
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackAmbiguousPredecessor-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    let source = directory.appendingPathComponent("source.png")
    let sharedIntermediate = directory.appendingPathComponent("shared-intermediate.png")
    let finalPreview = directory.appendingPathComponent("final-preview.png")
    try writeSolidPNG(to: source, red: 0.1, green: 0.3, blue: 0.6, width: 16, height: 8)
    try writeSolidPNG(to: sharedIntermediate, red: 0.3, green: 0.4, blue: 0.5, width: 16, height: 8)
    try writeSolidPNG(to: finalPreview, red: 0.5, green: 0.5, blue: 0.5, width: 16, height: 8)
    let firstStretch = EditOperation(
        kind: .stretch,
        parameters: [
            "input": source.path,
            "output": sharedIntermediate.path,
            "targetBackground": "0.21",
        ]
    )
    let secondStretch = EditOperation(
        kind: .stretch,
        parameters: [
            "input": source.path,
            "output": sharedIntermediate.path,
            "targetBackground": "0.31",
        ]
    )
    let denoise = EditOperation(
        kind: .denoise,
        parameters: [
            "input": sharedIntermediate.path,
            "output": finalPreview.path,
            "amount": "0.4",
            "chromaAmount": "0.5",
            "radius": "1",
        ]
    )
    var project = PhotonStackProject(name: "Ambiguous Predecessor")
    project.addAssets(from: [source])
    project.editGraph = EditGraph(operations: [firstStretch, secondStretch, denoise])
    project.workspaceState = ProjectWorkspaceState(
        selectedAssetID: project.assets.first?.id,
        previewURL: finalPreview,
        previewOperationID: denoise.id,
        editGraphNeedsReplay: false,
        canvasMode: .sourcePreview
    )
    let service = RecordingProcessingService()
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: service,
        previewDirectory: directory.appendingPathComponent("previews", isDirectory: true)
    )

    #expect(model.canReplayEditGraph)
    #expect(model.canExportCurrentImage == false)
    model.startReplayEditGraph()
    try await waitForStartedProcessingCompletion(in: model)

    let snapshot = await service.snapshot()
    #expect(snapshot.autoStretchInputs.isEmpty)
    #expect(snapshot.denoiseInput == nil)
    #expect(model.latestJob?.status == .failed)
    #expect(model.errorMessage == model.localized(.editGraphAmbiguousProvenance))
}

@Test @MainActor func unanchoredReplayRejectsOperationsFromDifferentAssetBranches() async throws {
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackUnanchoredBranches-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    let firstInput = directory.appendingPathComponent("first.png")
    let secondInput = directory.appendingPathComponent("second.png")
    let firstOutput = directory.appendingPathComponent("first-stretch.png")
    let secondOutput = directory.appendingPathComponent("second-stretch.png")
    try writeSolidPNG(to: firstInput, red: 0.1, green: 0.2, blue: 0.7, width: 16, height: 8)
    try writeSolidPNG(to: secondInput, red: 0.7, green: 0.2, blue: 0.1, width: 16, height: 8)
    var project = PhotonStackProject(name: "Unanchored Asset Branches")
    project.addAssets(from: [firstInput, secondInput])
    project.editGraph = EditGraph(operations: [
        EditOperation(
            kind: .stretch,
            parameters: [
                "input": firstInput.path,
                "output": firstOutput.path,
                "targetBackground": "0.21",
            ]
        ),
        EditOperation(
            kind: .stretch,
            parameters: [
                "input": secondInput.path,
                "output": secondOutput.path,
                "targetBackground": "0.31",
            ]
        ),
    ])
    project.workspaceState = ProjectWorkspaceState(
        selectedAssetID: project.assets.first?.id,
        previewURL: firstInput,
        canvasMode: .sourcePreview
    )
    let service = RecordingProcessingService()
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: service,
        previewDirectory: directory.appendingPathComponent("previews", isDirectory: true)
    )

    #expect(model.previewOperationID == nil)
    #expect(model.canReplayEditGraph)
    model.startReplayEditGraph()
    try await waitForStartedProcessingCompletion(in: model)

    #expect(await service.snapshot().autoStretchInputs.isEmpty)
    #expect(model.previewURL == firstInput)
    #expect(model.latestJob?.status == .failed)
    #expect(model.errorMessage == model.localized(.editGraphAmbiguousProvenance))
}

@Test @MainActor func missingLegacyPreviewInfersUniqueOperationAndRequiresReplay() async throws {
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackMissingLegacyPreview-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    let source = directory.appendingPathComponent("source.png")
    let missingPreview = directory.appendingPathComponent("missing-preview.png")
    try writeSolidPNG(to: source, red: 0.1, green: 0.3, blue: 0.5, width: 16, height: 8)
    var project = PhotonStackProject(name: "Missing Legacy Preview")
    project.addAssets(from: [source])
    let assetID = try #require(project.assets.first?.id)
    let stretch = EditOperation(
        kind: .stretch,
        parameters: [
            "input": source.path,
            "output": missingPreview.path,
            "targetBackground": "0.29",
        ]
    )
    project.editGraph = EditGraph(operations: [stretch])
    project.workspaceState = ProjectWorkspaceState(
        selectedAssetID: assetID,
        previewURL: missingPreview,
        canvasMode: .sourcePreview
    )
    let projectDirectory = directory.appendingPathComponent("project", isDirectory: true)
    let repository = ProjectRepository()
    try await repository.save(project, to: projectDirectory)
    let service = RecordingProcessingService()
    let model = PhotonStackWorkspaceModel(
        processingService: service,
        projectRepository: repository,
        previewDirectory: directory.appendingPathComponent("previews", isDirectory: true)
    )

    await model.loadProject(from: projectDirectory)

    #expect(model.previewURL == source)
    #expect(model.previewOperationID == stretch.id)
    #expect(model.editGraphNeedsReplay)
    #expect(model.canExportCurrentImage == false)

    model.startReplayEditGraph()
    try await waitForStartedProcessingCompletion(in: model, minimumJobCount: 2)
    let snapshot = await service.snapshot()
    #expect(snapshot.autoStretchInputs == [source])
    #expect(snapshot.autoStretchTargets == [0.29])
    #expect(model.previewOperationID == stretch.id)
    #expect(model.editGraphNeedsReplay == false)
    #expect(model.latestJob?.status == .succeeded)
}

@Test @MainActor func savedSourceCanvasModeKeepsVisibleLayersOutOfRestoredExport() async throws {
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackSavedSourceCanvas-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    let redURL = directory.appendingPathComponent("red.png")
    let blueURL = directory.appendingPathComponent("blue.png")
    try writeSolidPNG(to: redURL, red: 1, green: 0, blue: 0, width: 8, height: 4)
    try writeSolidPNG(to: blueURL, red: 0, green: 0, blue: 1, width: 8, height: 4)
    var project = PhotonStackProject(name: "Saved Source Canvas")
    project.addAssets(from: [redURL, blueURL])
    let redLayer = ProcessingLayer(name: "Red layer", kind: .baseImage, inputURL: redURL)
    project.appendLayer(redLayer)
    let blueAsset = try #require(project.assets.last)
    let repository = ProjectRepository()
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: RecordingProcessingService(),
        projectRepository: repository,
        previewDirectory: directory.appendingPathComponent("previews", isDirectory: true)
    )
    model.selectLayer(redLayer)
    model.select(blueAsset)

    let projectDirectory = directory.appendingPathComponent("project", isDirectory: true)
    await model.saveProject(to: projectDirectory)

    let restoredService = RecordingProcessingService()
    let restored = PhotonStackWorkspaceModel(
        processingService: restoredService,
        projectRepository: repository,
        previewDirectory: directory.appendingPathComponent("restored-previews", isDirectory: true)
    )
    await restored.loadProject(from: projectDirectory)

    #expect(restored.canvasMode == .sourcePreview)
    #expect(restored.activeLayerID == nil)
    #expect(restored.currentInputURL == blueURL)
    #expect(restored.project.layers.first?.isVisible == true)
    await restored.exportCurrentImage(to: directory.appendingPathComponent("restored-blue.tiff"))
    #expect(await restoredService.snapshot().convertInput == blueURL)
}

@Test @MainActor func batchQueueReportsLiveChildProgress() async throws {
    let service = RecordingProcessingService(
        autoStretchDelayNanoseconds: 300_000_000,
        autoStretchProgressToEmit: 0.37
    )
    let input = URL(fileURLWithPath: "/tmp/photonstack-batch-live-progress.tiff")
    var project = PhotonStackProject(name: "Batch Live Progress")
    project.addAssets(from: [input])
    let previewDirectory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackBatchLiveProgress-\(UUID().uuidString)", isDirectory: true)
    defer {
        try? FileManager.default.removeItem(at: previewDirectory)
    }
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: service,
        previewDirectory: previewDirectory
    )
    model.enqueueCurrent(.stretch)

    model.startBatchQueue()
    let deadline = ContinuousClock.now + .seconds(5)
    while ContinuousClock.now < deadline,
          (model.progressFraction != 0.37 || model.progressMessage?.contains("stretch work") != true) {
        try await Task.sleep(nanoseconds: 10_000_000)
    }

    #expect(model.isBatchRunning)
    #expect(model.progressFraction == 0.37)
    #expect(model.progressMessage?.contains("stretch work") == true)
    #expect(await service.progressJobID() != nil)

    model.cancelCurrentTask()
    try await waitForBatchCompletion(in: model)
}

@Test @MainActor func pausedBatchFinishesRunningItemBeforeSchedulingNext() async throws {
    let service = RecordingProcessingService(autoStretchDelayNanoseconds: 180_000_000)
    let firstInput = URL(fileURLWithPath: "/tmp/photonstack-paused-batch-first.tiff")
    let secondInput = URL(fileURLWithPath: "/tmp/photonstack-paused-batch-second.tiff")
    var project = PhotonStackProject(name: "Paused Batch")
    project.addAssets(from: [firstInput, secondInput])
    project.setBatchMaxConcurrentTasks(1)
    let previewDirectory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackPausedBatch-\(UUID().uuidString)", isDirectory: true)
    defer {
        try? FileManager.default.removeItem(at: previewDirectory)
    }
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: service,
        previewDirectory: previewDirectory
    )
    model.enqueueAllAssets(.stretch)

    model.startBatchQueue()
    try await waitForAutoStretchCall(from: service)
    model.pauseBatchQueue()

    let pauseDeadline = ContinuousClock.now + .seconds(1)
    while ContinuousClock.now < pauseDeadline, model.batchQueue.first?.status != .succeeded {
        try await Task.sleep(nanoseconds: 10_000_000)
    }

    #expect(model.isBatchRunning)
    #expect(model.isBatchPaused)
    #expect(model.batchQueue.map(\.status) == [.succeeded, .queued])
    #expect(model.progressFraction == 0.5)
    #expect(await service.snapshot().autoStretchCalls == 1)

    model.resumeBatchQueue()
    try await waitForBatchCompletion(in: model)
    #expect(model.batchQueue.allSatisfy { $0.status == .succeeded })
    #expect(await service.snapshot().autoStretchCalls == 2)
}

@Test @MainActor func pausedBatchAutomaticallyFinishesWhenNoQueuedItemsRemain() async throws {
    let service = RecordingProcessingService(autoStretchDelayNanoseconds: 180_000_000)
    let input = URL(fileURLWithPath: "/tmp/photonstack-paused-final-item.tiff")
    var project = PhotonStackProject(name: "Paused Final Batch Item")
    project.addAssets(from: [input])
    let previewDirectory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackPausedFinalItem-\(UUID().uuidString)", isDirectory: true)
    defer {
        try? FileManager.default.removeItem(at: previewDirectory)
    }
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: service,
        previewDirectory: previewDirectory
    )
    model.enqueueCurrent(.stretch)

    model.startBatchQueue()
    try await waitForAutoStretchCall(from: service)
    model.pauseBatchQueue()
    try await waitForBatchCompletion(in: model)

    #expect(model.batchQueue.first?.status == .succeeded)
    #expect(model.isBatchRunning == false)
    #expect(model.isBatchPaused == false)
    #expect(model.isProcessing == false)
    #expect(model.progressFraction == nil)
}

@Test @MainActor func batchQueueAttemptCounterDoesNotOverflowAtIntegerMaximum() async throws {
    let input = URL(fileURLWithPath: "/tmp/photonstack-max-attempt-batch.tiff")
    var project = PhotonStackProject(name: "Maximum Batch Attempt")
    project.addAssets(from: [input])
    project.replaceBatchQueue([
        BatchQueueItem(
            title: "Maximum Attempt Stretch",
            kind: .stretch,
            inputURL: input,
            attempts: Int.max
        ),
    ])
    let previewDirectory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackMaximumAttempt-\(UUID().uuidString)", isDirectory: true)
    defer {
        try? FileManager.default.removeItem(at: previewDirectory)
    }
    let service = RecordingProcessingService()
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: service,
        previewDirectory: previewDirectory
    )

    model.startBatchQueue()
    try await waitForBatchCompletion(in: model)

    #expect(model.batchQueue.first?.attempts == Int.max)
    #expect(model.batchQueue.first?.status == .succeeded)
    #expect(await service.snapshot().autoStretchCalls == 1)
}

@Test @MainActor func progressEventsAreScopedToActiveForegroundJob() async throws {
    let service = RecordingProcessingService(autoStretchDelayNanoseconds: 5_000_000_000)
    let input = URL(fileURLWithPath: "/tmp/photonstack-progress-scope.tiff")
    let previewDirectory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackProgressScope-\(UUID().uuidString)", isDirectory: true)
    defer {
        try? FileManager.default.removeItem(at: previewDirectory)
    }
    var project = PhotonStackProject(name: "Progress Scope Test")
    project.addAssets(from: [input])
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: service,
        previewDirectory: previewDirectory
    )

    model.startAutoStretchPreview()
    let activeJobID = try await waitForProgressJobID(from: service)
    #expect(model.progressFraction == 0)

    NotificationCenter.default.post(
        name: .photonStackProcessingProgress,
        object: ProcessingProgressEvent(
            jobID: UUID(),
            task: "other",
            command: "other",
            stage: "work",
            progress: 0.85
        )
    )
    try await Task.sleep(nanoseconds: 20_000_000)
    #expect(model.progressFraction == 0)

    NotificationCenter.default.post(
        name: .photonStackProcessingProgress,
        object: ProcessingProgressEvent(
            jobID: activeJobID,
            task: "stretch",
            command: "stretch",
            stage: "work",
            progress: 0.42,
            step: 2,
            total: 5
        )
    )
    try await Task.sleep(nanoseconds: 20_000_000)
    #expect(model.progressFraction == 0.42)
    #expect(model.progressMessage == "stretch work 2/5")

    NotificationCenter.default.post(
        name: .photonStackProcessingProgress,
        object: ProcessingProgressEvent(
            jobID: activeJobID,
            task: "stretch",
            command: "stretch",
            stage: "invalid-progress",
            progress: .nan
        )
    )
    try await Task.sleep(nanoseconds: 20_000_000)
    #expect(model.progressFraction == 0.42)
    #expect(model.progressMessage == "stretch work 2/5")

    NotificationCenter.default.post(
        name: .photonStackProcessingProgress,
        object: ProcessingProgressEvent(
            jobID: activeJobID,
            task: "stretch",
            command: "stretch",
            stage: "late-old-event",
            progress: 0.21,
            step: 1,
            total: 5
        )
    )
    try await Task.sleep(nanoseconds: 20_000_000)
    #expect(model.progressFraction == 0.42)
    #expect(model.progressMessage == "stretch work 2/5")

    model.cancelCurrentTask()
    try await waitForProcessingCompletion(in: model)
}

@Test @MainActor func batchQueueCannotStartDuringForegroundJob() async throws {
    let service = RecordingProcessingService(autoStretchDelayNanoseconds: 300_000_000)
    let input = URL(fileURLWithPath: "/tmp/photonstack-foreground-before-batch.tiff")
    var project = PhotonStackProject(name: "Foreground Before Batch Test")
    project.addAssets(from: [input])
    let model = PhotonStackWorkspaceModel(project: project, processingService: service)
    model.enqueueCurrent(.denoise)

    model.startAutoStretchPreview()
    _ = try await waitForProgressJobID(from: service)
    model.startBatchQueue()

    #expect(model.isBatchRunning == false)
    #expect(model.batchQueue.first?.status == .queued)

    model.cancelCurrentTask()
    try await waitForProcessingCompletion(in: model)
}

@Test @MainActor func foregroundJobCannotStartDuringBatchQueue() async throws {
    let service = RecordingProcessingService(autoStretchDelayNanoseconds: 300_000_000)
    let input = URL(fileURLWithPath: "/tmp/photonstack-batch-before-foreground.tiff")
    var project = PhotonStackProject(name: "Batch Before Foreground Test")
    project.addAssets(from: [input])
    let model = PhotonStackWorkspaceModel(project: project, processingService: service)
    model.enqueueCurrent(.stretch)
    model.startBatchQueue()
    try await waitForAutoStretchCall(from: service)

    let aggregateProgress = model.progressFraction
    NotificationCenter.default.post(
        name: .photonStackProcessingProgress,
        object: ProcessingProgressEvent(
            jobID: UUID(),
            task: "stretch",
            command: "stretch",
            stage: "complete",
            progress: 1
        )
    )
    model.startDenoisePreview()
    try await Task.sleep(nanoseconds: 20_000_000)

    #expect(model.progressFraction == aggregateProgress)
    #expect(model.jobs.isEmpty)
    #expect(model.isBatchRunning)

    model.cancelCurrentTask()
    try await waitForBatchCompletion(in: model)
}

@Test @MainActor func interruptedForegroundCompletionCannotOverwriteReplacementTask() async throws {
    let service = RecordingProcessingService(
        autoStretchDelayNanoseconds: 180_000_000,
        subsequentAutoStretchDelayNanoseconds: 450_000_000,
        ignoreAutoStretchCancellation: true
    )
    let firstInput = URL(fileURLWithPath: "/tmp/photonstack-interrupted-first.tiff")
    let secondInput = URL(fileURLWithPath: "/tmp/photonstack-interrupted-second.tiff")
    var project = PhotonStackProject(name: "Interrupted Foreground Test")
    project.addAssets(from: [firstInput])
    let model = PhotonStackWorkspaceModel(project: project, processingService: service)

    model.startAutoStretchPreview()
    try await waitForAutoStretchCall(from: service, minimum: 1)

    #expect(model.requestWindowClose() == false)
    #expect(model.confirmPendingProcessingInterruption(.closeWindow) == .closeWindow)
    model.createProject(template: .singleFrame, name: "Replacement Foreground")
    #expect(model.importAssets(from: [secondInput]))
    let secondAsset = try #require(model.project.assets.first { $0.originalURL == secondInput })
    model.select(secondAsset)
    model.startAutoStretchPreview()
    try await waitForAutoStretchCall(from: service, minimum: 2)
    let replacementJobID = try #require(await service.progressJobID())

    try await Task.sleep(nanoseconds: 240_000_000)
    #expect(model.isProcessing)
    #expect(model.canCancel)
    #expect(model.previewURL == secondInput)
    #expect(model.jobs.contains { $0.status == .running })

    NotificationCenter.default.post(
        name: .photonStackProcessingProgress,
        object: ProcessingProgressEvent(
            jobID: replacementJobID,
            task: "replacement",
            command: "stretch",
            stage: "work",
            progress: 0.67
        )
    )
    try await Task.sleep(nanoseconds: 20_000_000)
    #expect(model.progressFraction == 0.67)

    model.cancelCurrentTask()
    try await waitForProcessingCompletion(in: model)
}

@Test @MainActor func interruptedBatchCompletionCannotOverwriteReplacementQueue() async throws {
    let service = RecordingProcessingService(
        autoStretchDelayNanoseconds: 180_000_000,
        subsequentAutoStretchDelayNanoseconds: 450_000_000,
        ignoreAutoStretchCancellation: true
    )
    let firstInput = URL(fileURLWithPath: "/tmp/photonstack-interrupted-batch-first.tiff")
    let secondInput = URL(fileURLWithPath: "/tmp/photonstack-interrupted-batch-second.tiff")
    var project = PhotonStackProject(name: "Interrupted Batch Test")
    project.addAssets(from: [firstInput])
    let previewDirectory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackInterruptedBatch-\(UUID().uuidString)", isDirectory: true)
    defer {
        try? FileManager.default.removeItem(at: previewDirectory)
    }
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: service,
        previewDirectory: previewDirectory
    )

    model.enqueueCurrent(.stretch)
    model.startBatchQueue()
    try await waitForAutoStretchCall(from: service, minimum: 1)

    #expect(model.requestWindowClose() == false)
    #expect(model.confirmPendingProcessingInterruption(.closeWindow) == nil)
    #expect(model.pendingProjectTransition == .closeWindow)
    #expect(model.discardChangesAndPerformPendingProjectTransition() == .closeWindow)
    model.createProject(template: .singleFrame, name: "Replacement Batch")
    model.importAssets(from: [secondInput])
    model.enqueueCurrent(.stretch)
    model.startBatchQueue()
    try await waitForAutoStretchCall(from: service, minimum: 2)

    try await Task.sleep(nanoseconds: 240_000_000)
    #expect(model.project.name == "Replacement Batch")
    #expect(model.batchQueue.count == 1)
    #expect(model.batchQueue.first?.inputURL == secondInput)
    #expect(model.batchQueue.first?.status == .running)
    #expect(model.isBatchRunning)
    #expect(model.isProcessing)
    #expect(model.canCancel)

    model.cancelCurrentTask()
    try await waitForBatchCompletion(in: model)
}

@Test @MainActor func projectLifecycleEntriesCannotInterruptForegroundStartupOrRunningTask() async throws {
    let service = RecordingProcessingService(autoStretchDelayNanoseconds: 300_000_000)
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackLifecycleGate-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    let input = URL(fileURLWithPath: "/tmp/photonstack-lifecycle-gate.tiff")
    var project = PhotonStackProject(name: "Lifecycle Gate")
    project.addAssets(from: [input])
    let model = PhotonStackWorkspaceModel(project: project, processingService: service)
    let saveDirectory = directory.appendingPathComponent("saved", isDirectory: true)
    let otherProjectDirectory = directory.appendingPathComponent("other", isDirectory: true)

    model.startAutoStretchPreview()
    #expect(model.hasActiveProcessing)
    model.startSaveProject(to: saveDirectory)
    model.startLoadProject(from: otherProjectDirectory)
    #expect(model.requestCreateProject(template: .mosaic, name: "Blocked Replacement") == false)
    model.requestOpenProject(from: otherProjectDirectory)
    #expect(model.pendingProjectTransition == nil)
    #expect(model.project.name == "Lifecycle Gate")
    #expect(FileManager.default.fileExists(atPath: saveDirectory.appendingPathComponent("project.json").path) == false)

    #expect(model.requestWindowClose() == false)
    #expect(model.pendingProcessingInterruption == .closeWindow)
    model.cancelPendingProcessingInterruption()
    #expect(model.requestApplicationTermination() == false)
    #expect(model.pendingProcessingInterruption == .terminateApplication)
    model.cancelPendingProcessingInterruption()

    try await waitForAutoStretchCall(from: service)
    model.startSaveProject(to: saveDirectory)
    model.startLoadProject(from: otherProjectDirectory)
    #expect(model.isProcessing)
    #expect(model.jobs.last?.status == .running)

    try await waitForProcessingCompletion(in: model)
    #expect(model.jobs.last?.status == .succeeded)
    #expect(model.hasActiveProcessing == false)

    model.startSaveProject(to: saveDirectory)
    let deadline = ContinuousClock.now + .seconds(2)
    while model.currentProjectDirectory != saveDirectory, ContinuousClock.now < deadline {
        try await Task.sleep(for: .milliseconds(10))
    }
    #expect(model.currentProjectDirectory == saveDirectory)
    #expect(FileManager.default.fileExists(atPath: saveDirectory.appendingPathComponent("project.json").path))
}

@Test @MainActor func rawPreviewCountsAsActiveForProjectLifecycleDecisions() async throws {
    let rawInput = URL(fileURLWithPath: "/tmp/photonstack-lifecycle-raw-preview.NEF")
    var project = PhotonStackProject(name: "RAW Lifecycle Gate")
    project.addAssets(from: [rawInput])
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: RecordingProcessingService()
    )

    model.setRawExposureBias(0.7)
    #expect(model.isRawPreviewUpdating)
    #expect(model.hasActiveProcessing)
    #expect(model.requestWindowClose() == false)
    #expect(model.pendingProcessingInterruption == .closeWindow)
    model.cancelPendingProcessingInterruption()

    model.cancelCurrentTask()
    try await Task.sleep(for: .milliseconds(10))
    #expect(model.isRawPreviewUpdating == false)
    #expect(model.hasActiveProcessing == false)
}

@Test @MainActor func confirmedProcessingInterruptionImmediatelyClosesRunningBatchItemState() async throws {
    let service = RecordingProcessingService(
        autoStretchDelayNanoseconds: 300_000_000,
        ignoreAutoStretchCancellation: true
    )
    let firstInput = URL(fileURLWithPath: "/tmp/photonstack-interrupt-state-first.tiff")
    let secondInput = URL(fileURLWithPath: "/tmp/photonstack-interrupt-state-second.tiff")
    let importedInput = URL(fileURLWithPath: "/tmp/photonstack-interrupt-state-imported.tiff")
    var project = PhotonStackProject(name: "Interrupted Batch State")
    project.addAssets(from: [firstInput, secondInput])
    project.setBatchMaxConcurrentTasks(1)
    let model = PhotonStackWorkspaceModel(project: project, processingService: service)
    model.enqueueAllAssets(.stretch)

    model.startBatchQueue()
    try await waitForAutoStretchCall(from: service)
    #expect(model.importAssets(from: [importedInput]) == false)
    #expect(model.pendingAssetImportCount == 1)
    #expect(model.requestApplicationTermination() == false)
    #expect(model.pendingProcessingInterruption == .terminateApplication)
    #expect(model.confirmPendingProcessingInterruption(.terminateApplication) == nil)
    #expect(model.pendingProjectTransition == .terminateApplication)
    model.cancelPendingProjectTransition()
    #expect(model.pendingAssetImportCount == 0)
    #expect(model.project.assets.contains { $0.originalURL == importedInput })

    #expect(model.isBatchRunning == false)
    #expect(model.isProcessing == false)
    #expect(model.canCancel == false)
    #expect(model.batchQueue.map(\.status) == [.cancelled, .queued])

    try await Task.sleep(nanoseconds: 350_000_000)
    #expect(model.batchQueue.map(\.status) == [.cancelled, .queued])
}

@Test @MainActor func importDuringForegroundProcessingQueuesWithoutCancellingOrReplacingCanvas() async throws {
    let service = RecordingProcessingService(autoStretchDelayNanoseconds: 180_000_000)
    let firstInput = URL(fileURLWithPath: "/tmp/photonstack-import-queued-first.tiff")
    let importedInput = URL(fileURLWithPath: "/tmp/photonstack-import-queued-second.tiff")
    var project = PhotonStackProject(name: "Queued Foreground Import")
    project.addAssets(from: [firstInput])
    let model = PhotonStackWorkspaceModel(project: project, processingService: service)

    model.startAutoStretchPreview()
    try await waitForAutoStretchCall(from: service)

    #expect(model.importAssets(from: [importedInput]) == false)
    #expect(model.pendingAssetImportCount == 1)
    #expect(model.project.assets.contains { $0.originalURL == importedInput } == false)
    #expect(model.isProcessing)
    #expect(model.jobs.last?.status == .running)

    try await waitForProcessingCompletion(in: model)
    let deadline = ContinuousClock.now + .seconds(2)
    while model.pendingAssetImportCount > 0, ContinuousClock.now < deadline {
        try await Task.sleep(for: .milliseconds(10))
    }

    #expect(model.pendingAssetImportCount == 0)
    #expect(model.project.assets.contains { $0.originalURL == importedInput })
    #expect(model.jobs.last?.status == .succeeded)
    #expect(model.previewURL != firstInput)
    #expect(model.selectedAsset?.originalURL == firstInput)
    #expect(model.commandOutput == String(format: model.localized(.assetsImported), 1))
}

@Test @MainActor func workspaceContextCannotSwitchUntilForegroundProcessingFinishes() async throws {
    let service = RecordingProcessingService(autoStretchDelayNanoseconds: 180_000_000)
    let firstInput = URL(fileURLWithPath: "/tmp/photonstack-context-first.tiff")
    let secondInput = URL(fileURLWithPath: "/tmp/photonstack-context-second.tiff")
    let layerInput = URL(fileURLWithPath: "/tmp/photonstack-context-layer.tiff")
    let artifactInput = URL(fileURLWithPath: "/tmp/photonstack-context-artifact.tiff")
    var project = PhotonStackProject(name: "Foreground Context Gate")
    project.addAssets(from: [firstInput, secondInput])
    let layer = ProcessingLayer(name: "Other Layer", kind: .baseImage, inputURL: layerInput)
    let artifact = ProcessingArtifact(kind: .editedImage, name: "Other Object", outputURLs: [artifactInput])
    project.appendLayer(layer)
    project.appendArtifact(artifact)
    let model = PhotonStackWorkspaceModel(project: project, processingService: service)
    let firstAsset = try #require(model.project.assets.first { $0.originalURL == firstInput })
    let secondAsset = try #require(model.project.assets.first { $0.originalURL == secondInput })
    model.select(firstAsset)

    model.startAutoStretchPreview()
    try await waitForAutoStretchCall(from: service)
    model.select(secondAsset)
    model.previewArtifact(artifact)
    model.selectLayer(layer)

    #expect(model.selectedAssetID == firstAsset.id)
    #expect(model.activeLayerID == nil)
    #expect(model.previewURL == firstInput)
    #expect(model.canvasMode == .sourcePreview)
    #expect(model.isProcessing)

    try await waitForProcessingCompletion(in: model)
    #expect(model.selectedAssetID == firstAsset.id)
    #expect(model.previewURL != firstInput)
    #expect(model.activeLayerID != layer.id)
    #expect(model.jobs.last?.status == .succeeded)

    model.select(secondAsset)
    #expect(model.selectedAssetID == secondAsset.id)
    #expect(model.previewURL == secondInput)
}

@Test @MainActor func processingConfigurationCannotChangeDuringForegroundProcessing() async throws {
    let service = RecordingProcessingService(autoStretchDelayNanoseconds: 180_000_000)
    let firstInput = URL(fileURLWithPath: "/tmp/photonstack-config-first.tiff")
    let secondInput = URL(fileURLWithPath: "/tmp/photonstack-config-second.tiff")
    var project = PhotonStackProject(name: "Foreground Configuration Gate")
    project.addAssets(from: [firstInput, secondInput], role: .mosaic)
    var initialParameters = ProcessingParameters()
    initialParameters.stretchTargetBackground = 0.23
    let model = PhotonStackWorkspaceModel(
        project: project,
        parameters: initialParameters,
        processingService: service
    )
    let firstAsset = try #require(model.project.assets.first { $0.originalURL == firstInput })
    let secondAsset = try #require(model.project.assets.first { $0.originalURL == secondInput })
    let originalOrder = model.mosaicAssets.map(\.id)
    let originalSelection = model.batchRegistrationSelection
    model.select(firstAsset)

    model.startAutoStretchPreview()
    try await waitForAutoStretchCall(from: service)
    #expect(model.canModifyProcessingConfiguration == false)

    var changedParameters = model.parameters
    changedParameters.stretchTargetBackground = 0.61
    changedParameters.workflowStackMethod = .average
    model.setProcessingParameters(changedParameters)
    model.setRawWhiteBalanceMode(.daylight)
    model.setExportBitDepth(.sixteen)
    model.setRole(.dark, for: firstAsset)
    model.clearBatchRegistrationFrames()
    model.moveMosaicPanel(secondAsset.id, direction: .up)

    #expect(model.parameters == initialParameters)
    #expect(model.exportOptions.bitDepth == .automatic)
    #expect(model.project.assets.first { $0.id == firstAsset.id }?.role == .mosaic)
    #expect(model.batchRegistrationSelection == originalSelection)
    #expect(model.mosaicAssets.map(\.id) == originalOrder)

    try await waitForProcessingCompletion(in: model)
    #expect(model.canModifyProcessingConfiguration)

    model.setProcessingParameters(changedParameters)
    model.setRawWhiteBalanceMode(.daylight)
    model.setExportBitDepth(.sixteen)
    model.setRole(.dark, for: firstAsset)
    model.clearBatchRegistrationFrames()
    model.moveMosaicPanel(secondAsset.id, direction: .up)

    #expect(model.parameters.stretchTargetBackground == 0.61)
    #expect(model.parameters.workflowStackMethod == .average)
    #expect(model.parameters.rawProcessingOptions.whiteBalanceMode == .daylight)
    #expect(model.exportOptions.bitDepth == .sixteen)
    #expect(model.project.assets.first { $0.id == firstAsset.id }?.role == .dark)
    #expect(model.batchRegistrationSelection.isEmpty)
}

@Test @MainActor func foregroundProcessingFreezesWorkspaceProductsAndEditGraph() async throws {
    let service = RecordingProcessingService(autoStretchDelayNanoseconds: 300_000_000)
    let firstInput = URL(fileURLWithPath: "/tmp/photonstack-workspace-gate-first.tiff")
    let secondInput = URL(fileURLWithPath: "/tmp/photonstack-workspace-gate-second.tiff")
    let maskInput = URL(fileURLWithPath: "/tmp/photonstack-workspace-gate-mask.png")
    let objectInput = URL(fileURLWithPath: "/tmp/photonstack-workspace-gate-object.png")
    var project = PhotonStackProject(name: "Foreground Workspace Gate")
    project.addAssets(from: [firstInput, secondInput])

    let mask = ProcessingArtifact(kind: .mask, name: "Mask", outputURLs: [maskInput])
    let smartObject = ProcessingArtifact(kind: .editedImage, name: "Smart Object", outputURLs: [objectInput])
    let bottomLayer = ProcessingLayer(name: "Bottom", kind: .baseImage, inputURL: firstInput)
    let topLayer = ProcessingLayer(
        name: "Top",
        kind: .adjustment,
        inputURL: objectInput,
        maskArtifactID: mask.id
    )
    let stretch = EditOperation(
        kind: .stretch,
        parameters: ["input": firstInput.path, "output": objectInput.path]
    )
    let denoise = EditOperation(
        kind: .denoise,
        parameters: ["input": objectInput.path, "output": "/tmp/photonstack-workspace-gate-denoise.png"]
    )
    project.appendArtifact(mask)
    project.appendArtifact(smartObject)
    project.appendLayer(bottomLayer)
    project.appendLayer(topLayer)
    project.appendOperation(stretch)
    project.appendOperation(denoise)

    let model = PhotonStackWorkspaceModel(project: project, processingService: service)
    let firstAsset = try #require(model.project.assets.first { $0.originalURL == firstInput })
    let secondAsset = try #require(model.project.assets.first { $0.originalURL == secondInput })
    model.select(firstAsset)

    model.startAutoStretchPreview()
    try await waitForAutoStretchCall(from: service)
    #expect(model.canModifyProcessingConfiguration == false)
    let frozenProject = model.project
    let frozenPreviewURL = model.previewURL
    let frozenActiveLayerID = model.activeLayerID

    model.createLayer(from: secondAsset)
    model.createLayer(from: smartObject)
    model.setLayerVisibility(topLayer.id, isVisible: false)
    model.beginLayerOpacityAdjustment(topLayer.id)
    model.setLayerOpacity(topLayer.id, opacity: 0.35)
    model.setLayerBlendMode(topLayer.id, blendMode: .screen)
    model.moveProcessingLayer(topLayer.id, direction: .down)
    model.deleteProcessingLayer(bottomLayer.id)
    model.setLayerMaskInverted(topLayer.id, inverted: true)
    model.beginLayerMaskAdjustment(topLayer.id)
    model.setLayerMaskDensity(topLayer.id, density: 0.35)
    model.setLayerMask(topLayer.id, artifactID: nil)
    #expect(model.applyMaskBrushEdits(
        [MaskBrushStroke(
            mode: .hide,
            points: [MaskBrushPoint(x: 0.5, y: 0.5)],
            diameterFraction: 0.2
        )],
        to: topLayer.id
    ) == false)
    model.removeArtifact(mask)
    model.removeAsset(secondAsset)
    model.toggleEditOperation(stretch.id)
    model.updateEditOperationParameter(stretch.id, key: "amount", value: "0.75")
    model.moveEditOperation(denoise.id, direction: .up)
    model.deleteEditOperation(stretch.id)
    model.undoEditGraphChange()
    model.redoEditGraphChange()
    model.resetPreviewToOriginal()
    model.stepBackPreview()

    #expect(model.project == frozenProject)
    #expect(model.previewURL == frozenPreviewURL)
    #expect(model.activeLayerID == frozenActiveLayerID)

    try await waitForProcessingCompletion(in: model)
    #expect(model.canModifyProcessingConfiguration)
    model.setLayerVisibility(topLayer.id, isVisible: false)
    #expect(model.layerIsVisible(topLayer.id) == false)
}

@Test @MainActor func rejectedOverlappingAsyncOperationsPreserveReportsAndParameters() async throws {
    let service = RecordingProcessingService(autoStretchDelayNanoseconds: 300_000_000)
    let input = URL(fileURLWithPath: "/tmp/photonstack-overlap-side-effects.tiff")
    var project = PhotonStackProject(name: "Overlapping Operation Side Effects")
    project.addAssets(from: [input])
    let model = PhotonStackWorkspaceModel(project: project, processingService: service)

    await model.detectArtifactTrails()
    let report = try #require(model.artifactTrailReport)
    let reportItems = report.items

    model.startAutoStretchPreview()
    try await waitForAutoStretchCall(from: service)
    let frozenParameters = model.parameters
    let runningJobCount = model.jobs.count

    await model.detectArtifactTrails()
    await model.removeClouds(selectedIndices: nil, strength: 0.12)

    #expect(model.artifactTrailReport?.items == reportItems)
    #expect(model.parameters == frozenParameters)
    #expect(model.jobs.count == runningJobCount)

    try await waitForProcessingCompletion(in: model)
    await model.removeClouds(selectedIndices: nil, strength: 0.12)
    #expect(model.parameters.cloudRemovalStrength == 0.12)
}

@Test @MainActor func runningBatchFreezesQueueShapeAndOutputConfiguration() async throws {
    let service = RecordingProcessingService(autoStretchDelayNanoseconds: 180_000_000)
    let input = URL(fileURLWithPath: "/tmp/photonstack-batch-config-input.tiff")
    let blockedOutputDirectory = URL(fileURLWithPath: "/tmp/photonstack-batch-config-blocked", isDirectory: true)
    var project = PhotonStackProject(name: "Batch Configuration Gate")
    project.addAssets(from: [input])
    let model = PhotonStackWorkspaceModel(project: project, processingService: service)
    model.enqueueCurrent(.stretch)

    model.startBatchQueue()
    try await waitForAutoStretchCall(from: service)
    #expect(model.isBatchRunning)
    #expect(model.batchQueue.count == 1)

    var changedParameters = model.parameters
    changedParameters.stretchTargetBackground = 0.62
    model.setProcessingParameters(changedParameters)
    model.enqueueCurrent(.denoise)
    model.enqueueAllAssets(.sharpen)
    model.retryFailedBatchItems()
    model.clearBatchQueue()
    model.setBatchOutputFormat(.tiff)
    model.setBatchMaxConcurrentTasks(4)
    model.setBatchOutputDirectory(blockedOutputDirectory)

    #expect(model.parameters.stretchTargetBackground != 0.62)
    #expect(model.batchQueue.count == 1)
    #expect(model.project.batchOutputFormat == .png)
    #expect(model.project.batchMaxConcurrentTasks == 1)
    #expect(model.project.batchOutputDirectory == nil)

    model.pauseBatchQueue()
    #expect(model.isBatchPaused)
    model.resumeBatchQueue()
    #expect(model.isBatchPaused == false)

    try await waitForBatchCompletion(in: model)
    model.setBatchOutputFormat(.tiff)
    model.setBatchMaxConcurrentTasks(4)
    model.setBatchOutputDirectory(blockedOutputDirectory)

    #expect(model.project.batchOutputFormat == .tiff)
    #expect(model.project.batchMaxConcurrentTasks == 4)
    #expect(model.project.batchOutputDirectory == blockedOutputDirectory)
}

@Test @MainActor func importDuringBatchQueuesUntilEveryRunningItemFinishes() async throws {
    let service = RecordingProcessingService(autoStretchDelayNanoseconds: 180_000_000)
    let firstInput = URL(fileURLWithPath: "/tmp/photonstack-batch-import-first.tiff")
    let importedInput = URL(fileURLWithPath: "/tmp/photonstack-batch-import-second.tiff")
    var project = PhotonStackProject(name: "Queued Batch Import")
    project.addAssets(from: [firstInput])
    let model = PhotonStackWorkspaceModel(project: project, processingService: service)
    model.enqueueCurrent(.stretch)

    model.startBatchQueue()
    try await waitForAutoStretchCall(from: service)
    #expect(model.importAssets(from: [importedInput]) == false)
    #expect(model.pendingAssetImportCount == 1)
    #expect(model.isBatchRunning)
    #expect(model.batchQueue.first?.status == .running)

    try await waitForBatchCompletion(in: model)
    let deadline = ContinuousClock.now + .seconds(2)
    while model.pendingAssetImportCount > 0, ContinuousClock.now < deadline {
        try await Task.sleep(for: .milliseconds(10))
    }

    #expect(model.pendingAssetImportCount == 0)
    #expect(model.project.assets.contains { $0.originalURL == importedInput })
    #expect(model.batchQueue.first?.status == .succeeded)
}

@Test @MainActor func importingAdditionalOrDuplicateAssetsPreservesExistingWorkspaceCanvas() async throws {
    let service = RecordingProcessingService()
    let firstInput = URL(fileURLWithPath: "/tmp/photonstack-import-preserve-first.tiff")
    let importedInput = URL(fileURLWithPath: "/tmp/photonstack-import-preserve-second.tiff")
    var project = PhotonStackProject(name: "Preserved Import Canvas")
    project.addAssets(from: [firstInput])
    let model = PhotonStackWorkspaceModel(project: project, processingService: service)

    await model.autoStretchPreview()
    let preview = try #require(model.previewURL)
    let activeLayerID = model.activeLayerID
    let canvasMode = model.canvasMode
    let operationIDs = model.project.editGraph.operations.map(\.id)

    #expect(model.importAssets(from: [importedInput]))
    #expect(model.previewURL == preview)
    #expect(model.activeLayerID == activeLayerID)
    #expect(model.canvasMode == canvasMode)
    #expect(model.project.editGraph.operations.map(\.id) == operationIDs)
    #expect(model.selectedAsset?.originalURL == firstInput)

    #expect(model.importAssets(from: [importedInput]) == false)
    #expect(model.previewURL == preview)
    #expect(model.activeLayerID == activeLayerID)
    #expect(model.project.editGraph.operations.map(\.id) == operationIDs)
}

@Test @MainActor func importingPathAliasesCreatesOnlyOneAssetAndSequenceFrame() async throws {
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackImportAliases-\(UUID().uuidString)", isDirectory: true)
    let nestedDirectory = directory.appendingPathComponent("nested", isDirectory: true)
    try FileManager.default.createDirectory(at: nestedDirectory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    let original = directory.appendingPathComponent("light.png")
    let standardizedAlias = nestedDirectory.appendingPathComponent("../light.png")
    let symlink = directory.appendingPathComponent("light-link.png")
    try writeSolidPNG(to: original, red: 0.2, green: 0.4, blue: 0.6, width: 16, height: 8)
    try FileManager.default.createSymbolicLink(at: symlink, withDestinationURL: original)
    let model = PhotonStackWorkspaceModel(
        project: PhotonStackProject(name: "Import Aliases"),
        processingService: RecordingProcessingService()
    )

    #expect(model.importAssets(from: [original, standardizedAlias, symlink]))
    #expect(model.project.assets.count == 1)
    #expect(model.project.assets.first?.originalURL == original)
    let rawSequence = try #require(model.project.artifacts.first { $0.kind == .rawSequence })
    #expect(rawSequence.frames.count == 1)
    #expect(rawSequence.frames.first?.sourceURL == original)
    #expect(model.importAssets(from: [symlink]) == false)
    #expect(model.project.assets.count == 1)
}

@Test @MainActor func cancelledBatchCompletionCannotBeMarkedSucceeded() async throws {
    let service = RecordingProcessingService(
        autoStretchDelayNanoseconds: 180_000_000,
        ignoreAutoStretchCancellation: true
    )
    let input = URL(fileURLWithPath: "/tmp/photonstack-cancelled-batch.tiff")
    var project = PhotonStackProject(name: "Cancelled Batch Test")
    project.addAssets(from: [input])
    let model = PhotonStackWorkspaceModel(project: project, processingService: service)
    model.enqueueCurrent(.stretch)
    let initialPreviewURL = model.previewURL

    model.startBatchQueue()
    try await waitForAutoStretchCall(from: service)
    model.cancelCurrentTask()
    try await waitForBatchCompletion(in: model)

    #expect(model.batchQueue.first?.status == .cancelled)
    #expect(model.batchQueue.first?.outputURL == nil)
    #expect(model.previewURL == initialPreviewURL)
}

@Test @MainActor func timelapseArtifactSequenceCleaningCreatesIntermediateProduct() async throws {
    let service = RecordingProcessingService()
    let frameA = URL(fileURLWithPath: "/tmp/photonstack-timelapse-a.tiff")
    let frameB = URL(fileURLWithPath: "/tmp/photonstack-timelapse-b.tiff")
    var project = PhotonStackProject(name: "Timelapse Artifact Test")
    project.addAssets(from: [frameA, frameB])
    let previewDirectory = FileManager.default.temporaryDirectory.appendingPathComponent(
        "PhotonStackTimelapseReplayCleanup-\(UUID().uuidString)",
        isDirectory: true
    )
    defer {
        try? FileManager.default.removeItem(at: previewDirectory)
    }

    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: service,
        previewDirectory: previewDirectory
    )

    await model.cleanTimelapseArtifactSequence()

    let snapshot = await service.snapshot()
    #expect(snapshot.cleanSequenceInputs == [frameA, frameB])
    #expect(snapshot.cleanSequenceOutputDirectory != nil)
    #expect(snapshot.cleanSequenceOutputFormat == "png")
    let artifact = try #require(model.project.artifacts.first { $0.kind == .cleanedTimelapseSequence })
    #expect(artifact.frameCount == 2)
    #expect(artifact.metrics["cleanedFrames"] == "2")
    #expect(artifact.metrics["removedTrails"] == "2")
    #expect(model.project.layers.contains { $0.sourceArtifactID == artifact.id })
    let operation = try #require(model.project.editGraph.operations.last { $0.kind == .artifactRemove })
    #expect(operation.parameters["mode"] == "cleanSequence")
    #expect(operation.parameters["sourceFrameIDs"] == model.project.assets.map(\.id.uuidString).joined(separator: ","))
    #expect(operation.parameters["inputPath.0"] == frameA.path)
    #expect(operation.parameters["inputPath.1"] == frameB.path)

    let replayService = RecordingProcessingService()
    let replayModel = PhotonStackWorkspaceModel(
        project: model.project,
        processingService: replayService,
        previewDirectory: previewDirectory
    )
    replayModel.startReplayEditGraph()
    try await waitForStartedProcessingCompletion(in: replayModel)
    let replaySnapshot = await replayService.snapshot()
    #expect(replaySnapshot.cleanSequenceInputs == [frameA, frameB])
    #expect(replaySnapshot.cleanSequenceOutputFormat == "png")
    #expect(replayModel.previewURL == frameA)
    let remainingReplayDirectories = try FileManager.default.contentsOfDirectory(
        at: previewDirectory,
        includingPropertiesForKeys: nil
    ).filter { $0.lastPathComponent.hasPrefix("replay-clean-sequence-") }
    #expect(remainingReplayDirectories.isEmpty)
}

@Test @MainActor func timelapseCleaningRejectsIncompleteSuccessReport() async throws {
    let service = RecordingProcessingService(
        cleanSequenceOutputOverride: #"{"type":"complete","command":"artifacts clean-sequence","items":[{"ok":true,"input":"/tmp/frame-a.tiff","output":""}]}"#
    )
    var project = PhotonStackProject(name: "Invalid Timelapse Report Test")
    project.addAssets(from: [
        URL(fileURLWithPath: "/tmp/photonstack-invalid-timelapse-a.tiff"),
        URL(fileURLWithPath: "/tmp/photonstack-invalid-timelapse-b.tiff"),
    ])
    let model = PhotonStackWorkspaceModel(project: project, processingService: service)

    await model.cleanTimelapseArtifactSequence()

    #expect(model.latestJob?.status == .failed)
    #expect(model.errorMessage?.contains("artifacts clean-sequence") == true)
    #expect(model.project.artifacts.contains { $0.kind == .cleanedTimelapseSequence } == false)
    #expect(model.project.layers.contains { $0.sourceArtifactID != nil } == false)
    #expect(model.project.editGraph.operations.contains { $0.parameters["mode"] == "cleanSequence" } == false)
}

@Test @MainActor func timelapseCleaningRejectsReportWhenOneOutputFileIsMissing() async throws {
    let service = RecordingProcessingService(omitLastCleanSequenceOutputFile: true)
    var project = PhotonStackProject(name: "Missing Timelapse Output Test")
    project.addAssets(from: [
        URL(fileURLWithPath: "/tmp/photonstack-missing-timelapse-output-a.tiff"),
        URL(fileURLWithPath: "/tmp/photonstack-missing-timelapse-output-b.tiff"),
    ])
    let previewDirectory = FileManager.default.temporaryDirectory.appendingPathComponent(
        "PhotonStackMissingTimelapseOutput-\(UUID().uuidString)",
        isDirectory: true
    )
    defer { try? FileManager.default.removeItem(at: previewDirectory) }
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: service,
        previewDirectory: previewDirectory
    )

    await model.cleanTimelapseArtifactSequence()

    #expect(model.latestJob?.status == .failed)
    #expect(model.errorMessage?.contains("artifacts clean-sequence") == true)
    #expect(model.project.artifacts.contains { $0.kind == .cleanedTimelapseSequence } == false)
    #expect(model.project.editGraph.operations.contains { $0.parameters["mode"] == "cleanSequence" } == false)
}

@Test @MainActor func timelapseCleaningRejectsOutputsWithTheWrongFormatExtension() async throws {
    let service = RecordingProcessingService(cleanSequenceOutputExtensionOverride: "tiff")
    var project = PhotonStackProject(name: "Mismatched Timelapse Format Test")
    project.addAssets(from: [
        URL(fileURLWithPath: "/tmp/photonstack-mismatched-timelapse-a.tiff"),
        URL(fileURLWithPath: "/tmp/photonstack-mismatched-timelapse-b.tiff"),
    ])
    let previewDirectory = FileManager.default.temporaryDirectory.appendingPathComponent(
        "PhotonStackMismatchedTimelapseFormat-\(UUID().uuidString)",
        isDirectory: true
    )
    defer { try? FileManager.default.removeItem(at: previewDirectory) }
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: service,
        previewDirectory: previewDirectory
    )

    await model.cleanTimelapseArtifactSequence()

    #expect(model.latestJob?.status == .failed)
    #expect(model.errorMessage?.contains("artifacts clean-sequence") == true)
    #expect(model.project.artifacts.contains { $0.kind == .cleanedTimelapseSequence } == false)
    #expect(model.project.editGraph.operations.contains { $0.parameters["mode"] == "cleanSequence" } == false)
}

@Test @MainActor func cancellingTimelapseHistogramPreservesWorkspaceState() async throws {
    let service = RecordingProcessingService(
        histogramDelayNanoseconds: 300_000_000,
        ignoreHistogramCancellation: true
    )
    var project = PhotonStackProject(name: "Cancelled Timelapse Cleaning")
    project.addAssets(from: [
        URL(fileURLWithPath: "/tmp/photonstack-cancel-timelapse-a.tiff"),
        URL(fileURLWithPath: "/tmp/photonstack-cancel-timelapse-b.tiff"),
    ])
    let previewDirectory = FileManager.default.temporaryDirectory.appendingPathComponent(
        "PhotonStackCancelledTimelapseCleaning-\(UUID().uuidString)",
        isDirectory: true
    )
    defer { try? FileManager.default.removeItem(at: previewDirectory) }
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: service,
        previewDirectory: previewDirectory
    )
    let initialPreview = model.previewURL
    let initialArtifacts = model.project.artifacts
    let initialLayers = model.project.layers

    model.startCleanTimelapseArtifactSequence()
    try await waitForHistogramCall(from: service)
    model.cancelCurrentTask()
    try await waitForProcessingCompletion(in: model)

    #expect(model.latestJob?.status == .cancelled)
    #expect(model.previewURL == initialPreview)
    #expect(model.project.artifacts == initialArtifacts)
    #expect(model.project.layers == initialLayers)
    #expect(model.project.editGraph.operations.isEmpty)
}

@Test @MainActor func timelapseSequenceReplayFailsInsteadOfSubstitutingMissingRecordedFrame() async throws {
    let service = RecordingProcessingService()
    let frame = URL(fileURLWithPath: "/tmp/photonstack-timelapse-recorded-frame.tiff")
    let currentExtra = URL(fileURLWithPath: "/tmp/photonstack-timelapse-current-extra.tiff")
    var project = PhotonStackProject(name: "Missing Timelapse Frame Replay Test")
    project.addAssets(from: [frame, currentExtra])
    let recordedFrameID = project.assets[0].id
    project.editGraph = EditGraph(operations: [
        EditOperation(
            kind: .artifactRemove,
            parameters: [
                "mode": "cleanSequence",
                "frames": "2",
                "sourceFrameIDs": [recordedFrameID, UUID()].map(\.uuidString).joined(separator: ","),
                "inputPath.0": frame.path,
                "inputPath.1": "/tmp/photonstack-timelapse-missing-frame.tiff",
                "outputFormat": "png",
            ]
        ),
    ])
    let model = PhotonStackWorkspaceModel(project: project, processingService: service)

    model.startReplayEditGraph()
    try await waitForStartedProcessingCompletion(in: model)

    let snapshot = await service.snapshot()
    #expect(snapshot.cleanSequenceInputs.isEmpty)
    #expect(model.latestJob?.status == .failed)
    #expect(model.errorMessage?.contains("artifactRemove") == true)
}

@Test @MainActor func editGraphUndoRedoRestoresOperationSnapshots() async throws {
    let service = RecordingProcessingService()
    let image = URL(fileURLWithPath: "/tmp/photonstack-edit-graph.tiff")
    let operation = EditOperation(kind: .stretch, parameters: ["targetBackground": "0.25"])
    var project = PhotonStackProject(name: "Edit Graph Test")
    project.addAssets(from: [image])
    project.editGraph = EditGraph(operations: [operation])

    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: service
    )

    model.updateEditOperationParameter(operation.id, key: "targetBackground", value: "0.35")
    #expect(model.canUndoEditGraph)
    #expect(model.project.editGraph.operations[0].parameters["targetBackground"] == "0.35")

    model.undoEditGraphChange()
    #expect(model.project.editGraph.operations[0].parameters["targetBackground"] == "0.25")
    #expect(model.canRedoEditGraph)

    model.redoEditGraphChange()
    #expect(model.project.editGraph.operations[0].parameters["targetBackground"] == "0.35")
    #expect(model.canUndoEditGraph)
}

@Test @MainActor func editGraphOnlyExposesValidatedUserParameters() {
    let operation = EditOperation(
        kind: .stack,
        parameters: [
            "alignment": AlignmentMethod.distortion.rawValue,
            "stackMethod": StackMethod.average.rawValue,
            "sourceArtifactID": UUID().uuidString,
            "sourceFrameIDs": UUID().uuidString,
            "inputPath.0": "/tmp/source.tiff",
            "artifactID": UUID().uuidString,
        ]
    )
    var project = PhotonStackProject(name: "Validated Edit Parameters")
    project.editGraph = EditGraph(operations: [operation])
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: RecordingProcessingService()
    )

    #expect(model.editableEditOperationParameters(for: operation).map(\.key) == ["alignment", "stackMethod"])
    #expect(model.updateEditOperationParameter(operation.id, key: "sourceArtifactID", value: UUID().uuidString) == false)
    #expect(model.project.editGraph.operations[0] == operation)
    #expect(model.canUndoEditGraph == false)

    #expect(model.updateEditOperationParameter(operation.id, key: "stackMethod", value: "bogus") == false)
    #expect(model.project.editGraph.operations[0].parameters["stackMethod"] == StackMethod.average.rawValue)
    #expect(model.errorMessage?.contains(model.localized(.invalidEditOperationParameter)) == true)
    #expect(model.canUndoEditGraph == false)

    #expect(model.updateEditOperationParameter(operation.id, key: "stackMethod", value: StackMethod.sigma.rawValue))
    #expect(model.project.editGraph.operations[0].parameters["stackMethod"] == StackMethod.sigma.rawValue)
    #expect(model.errorMessage == nil)
    #expect(model.canUndoEditGraph)
}

@Test @MainActor func editGraphRejectsInvalidNumericAndCrossFieldParameters() {
    let operation = EditOperation(
        kind: .starMask,
        parameters: [
            "radius": "4",
            "largeRadius": "8",
            "layered": "true",
            "sigmaThreshold": "3.0",
            "minPeak": "0.05",
            "maxStars": "50000",
        ]
    )
    var project = PhotonStackProject(name: "Validated Star Mask Parameters")
    project.editGraph = EditGraph(operations: [operation])
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: RecordingProcessingService()
    )

    #expect(model.updateEditOperationParameter(operation.id, key: "sigmaThreshold", value: "nan") == false)
    #expect(model.updateEditOperationParameter(operation.id, key: "largeRadius", value: "2") == false)
    #expect(model.project.editGraph.operations[0] == operation)
    #expect(model.canUndoEditGraph == false)

    #expect(model.updateEditOperationParameter(operation.id, key: "largeRadius", value: "12"))
    #expect(model.project.editGraph.operations[0].parameters["largeRadius"] == "12")
    #expect(model.errorMessage == nil)
    #expect(model.canUndoEditGraph)
}

@Test @MainActor func cropEditGraphAcceptsTheSameInclusiveMarginRangeAsCLI() {
    let operation = EditOperation(
        kind: .crop,
        parameters: ["aspect": "16:9", "margin": "0.2"]
    )
    var project = PhotonStackProject(name: "Crop Margin Validation")
    project.editGraph = EditGraph(operations: [operation])
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: RecordingProcessingService()
    )

    #expect(model.updateEditOperationParameter(operation.id, key: "margin", value: "0.45"))
    #expect(model.project.editGraph.operations[0].parameters["margin"] == "0.45")
    #expect(model.updateEditOperationParameter(operation.id, key: "margin", value: "0.4501") == false)
    #expect(model.project.editGraph.operations[0].parameters["margin"] == "0.45")
}

@Test @MainActor func editGraphReplayRejectsPersistedInvalidParameterInsteadOfUsingDefault() async throws {
    let service = RecordingProcessingService()
    let input = URL(fileURLWithPath: "/tmp/photonstack-invalid-recorded-parameter.tiff")
    var project = PhotonStackProject(name: "Invalid Recorded Parameter")
    project.addAssets(from: [input])
    project.editGraph = EditGraph(operations: [
        EditOperation(
            kind: .stretch,
            parameters: ["targetBackground": "not-a-number"]
        ),
    ])
    let model = PhotonStackWorkspaceModel(project: project, processingService: service)

    model.startReplayEditGraph()
    try await waitForStartedProcessingCompletion(in: model)

    #expect(await service.snapshot().autoStretchCalls == 0)
    #expect(model.latestJob?.status == .failed)
    #expect(model.errorMessage?.contains(model.localized(.invalidEditOperationParameter)) == true)
    #expect(model.errorMessage?.contains("stretch.targetBackground") == true)
}

@Test @MainActor func editGraphReplayRejectsMissingRequiredCropAspectInsteadOfSkipping() async throws {
    let service = RecordingProcessingService()
    let input = URL(fileURLWithPath: "/tmp/photonstack-missing-crop-aspect.tiff")
    var project = PhotonStackProject(name: "Missing Crop Aspect")
    project.addAssets(from: [input])
    project.editGraph = EditGraph(operations: [
        EditOperation(kind: .crop, parameters: [:]),
    ])
    let model = PhotonStackWorkspaceModel(project: project, processingService: service)

    model.startReplayEditGraph()
    try await waitForStartedProcessingCompletion(in: model)

    #expect(model.latestJob?.status == .failed)
    #expect(model.errorMessage?.contains(model.localized(.invalidEditOperationParameter)) == true)
    #expect(model.errorMessage?.contains("crop.aspect") == true)
    #expect(model.previewURL == nil)
}

@Test @MainActor func editGraphReplayRejectsNonEditingJobInsteadOfSkipping() async throws {
    let input = URL(fileURLWithPath: "/tmp/photonstack-unsupported-edit-job.tiff")
    var project = PhotonStackProject(name: "Unsupported Edit Job")
    project.addAssets(from: [input])
    project.editGraph = EditGraph(operations: [
        EditOperation(kind: .histogram, parameters: [:]),
    ])
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: RecordingProcessingService()
    )

    model.startReplayEditGraph()
    try await waitForStartedProcessingCompletion(in: model)

    #expect(model.latestJob?.status == .failed)
    #expect(model.errorMessage?.contains(model.localized(.editGraphUnsupportedOperation)) == true)
    #expect(model.errorMessage?.contains("histogram") == true)
}

@Test @MainActor func rawDecodeReplayRejectsMissingRecordedAssetInsteadOfUsingSelectedRaw() async throws {
    let first = URL(fileURLWithPath: "/tmp/photonstack-raw-identity-a.nef")
    let second = URL(fileURLWithPath: "/tmp/photonstack-raw-identity-b.nef")
    var project = PhotonStackProject(name: "Missing RAW Identity")
    project.addAssets(from: [first, second])
    project.editGraph = EditGraph(operations: [
        EditOperation(
            kind: .rawDecode,
            parameters: [
                "assetID": UUID().uuidString,
                "width": "1200",
            ]
        ),
    ])
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: RecordingProcessingService()
    )

    model.startReplayEditGraph()
    try await waitForStartedProcessingCompletion(in: model)

    #expect(model.latestJob?.status == .failed)
    #expect(model.errorMessage?.contains(model.localized(.editGraphMissingRecordedAssets)) == true)
    #expect(model.previewURL == nil)
}

@Test @MainActor func fullResolutionExportRejectsMissingCropAspectInsteadOfExportingSource() async throws {
    let service = RecordingProcessingService()
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackMissingCropAspectExport-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    let input = directory.appendingPathComponent("input.png")
    let cropPreview = directory.appendingPathComponent("crop-preview.png")
    let finalOutput = directory.appendingPathComponent("final.png")
    try writeSolidPNG(to: input, red: 0.2, green: 0.4, blue: 0.8, width: 16, height: 8)
    try writeSolidPNG(to: cropPreview, red: 0.4, green: 0.4, blue: 0.4, width: 12, height: 8)

    var project = PhotonStackProject(name: "Missing Crop Aspect Export")
    project.addAssets(from: [input])
    let cropOperation = EditOperation(
        kind: .crop,
        parameters: [
            "input": input.path,
            "output": cropPreview.path,
        ]
    )
    project.editGraph = EditGraph(operations: [cropOperation])
    let artifact = ProcessingArtifact(
        kind: .editedImage,
        name: "Broken Crop Preview",
        operationKind: .crop,
        outputURLs: [cropPreview]
    )
    project.appendArtifact(artifact)
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: service,
        previewDirectory: directory.appendingPathComponent("previews", isDirectory: true)
    )
    model.previewArtifact(artifact)

    await model.exportCurrentImage(to: finalOutput)

    #expect(await service.snapshot().convertInputs.isEmpty)
    #expect(model.latestJob?.status == .failed)
    #expect(model.errorMessage?.contains("crop.aspect") == true)
    #expect(FileManager.default.fileExists(atPath: finalOutput.path) == false)
}

@Test @MainActor func editGraphReplayRejectsPartiallyInvalidSelectedIndices() async throws {
    let service = RecordingProcessingService()
    let input = URL(fileURLWithPath: "/tmp/photonstack-invalid-recorded-indices.tiff")
    var project = PhotonStackProject(name: "Invalid Recorded Indices")
    project.addAssets(from: [input])
    project.editGraph = EditGraph(operations: [
        EditOperation(
            kind: .artifactRemove,
            parameters: [
                "selectedIndices": "0,bad,2",
                "removeAirplanes": "true",
                "removeDrones": "true",
                "removeSatellites": "true",
                "removeMeteors": "false",
            ]
        ),
    ])
    let model = PhotonStackWorkspaceModel(project: project, processingService: service)

    model.startReplayEditGraph()
    try await waitForStartedProcessingCompletion(in: model)

    #expect(await service.snapshot().artifactSelectedIndices == nil)
    #expect(model.latestJob?.status == .failed)
    #expect(model.errorMessage?.contains(model.localized(.invalidEditOperationParameter)) == true)
    #expect(model.errorMessage?.contains("artifactRemove.selectedIndices") == true)
}

@Test @MainActor func editGraphReplayRejectsInvalidStructuralBranchParameter() async throws {
    let service = RecordingProcessingService()
    let input = URL(fileURLWithPath: "/tmp/photonstack-invalid-recorded-mode.tiff")
    var project = PhotonStackProject(name: "Invalid Recorded Mode")
    project.addAssets(from: [input])
    project.editGraph = EditGraph(operations: [
        EditOperation(
            kind: .artifactRemove,
            parameters: [
                "mode": "clean-sequnce",
                "selectedIndices": "",
                "removeAirplanes": "true",
                "removeDrones": "true",
                "removeSatellites": "true",
                "removeMeteors": "false",
            ]
        ),
    ])
    let model = PhotonStackWorkspaceModel(project: project, processingService: service)

    model.startReplayEditGraph()
    try await waitForStartedProcessingCompletion(in: model)

    let snapshot = await service.snapshot()
    #expect(snapshot.artifactSelectedIndices == nil)
    #expect(snapshot.cleanSequenceInputs.isEmpty)
    #expect(model.latestJob?.status == .failed)
    #expect(model.errorMessage?.contains("artifactRemove.mode") == true)
}

@Test @MainActor func legacyTimelapseReplayRejectsIncompleteExplicitFramePaths() async throws {
    let service = RecordingProcessingService()
    let first = URL(fileURLWithPath: "/tmp/photonstack-legacy-timelapse-a.tiff")
    let second = URL(fileURLWithPath: "/tmp/photonstack-legacy-timelapse-b.tiff")
    var project = PhotonStackProject(name: "Incomplete Legacy Timelapse")
    project.addAssets(from: [first, second])
    project.editGraph = EditGraph(operations: [
        EditOperation(
            kind: .artifactRemove,
            parameters: [
                "mode": "cleanSequence",
                "frames": "2",
                "inputPath.0": first.path,
                "outputFormat": "png",
                "minWeight": "0.3",
                "recurrenceThreshold": "2",
            ]
        ),
    ])
    let model = PhotonStackWorkspaceModel(project: project, processingService: service)

    model.startReplayEditGraph()
    try await waitForStartedProcessingCompletion(in: model)

    #expect(await service.snapshot().cleanSequenceInputs.isEmpty)
    #expect(model.latestJob?.status == .failed)
    #expect(model.errorMessage?.contains(model.localized(.editGraphInvalidRecordedAssets)) == true)
}

@Test @MainActor func legacyStackReplayRejectsIncompleteExplicitInputPaths() async throws {
    let service = RecordingProcessingService()
    let first = URL(fileURLWithPath: "/tmp/photonstack-legacy-stack-a.tiff")
    let second = URL(fileURLWithPath: "/tmp/photonstack-legacy-stack-b.tiff")
    var project = PhotonStackProject(name: "Incomplete Legacy Stack")
    project.addAssets(from: [first, second])
    project.editGraph = EditGraph(operations: [
        EditOperation(
            kind: .stack,
            parameters: [
                "inputs": "2",
                "inputPath.0": first.path,
                "sourceArtifact": "lights",
                "stackMethod": StackMethod.average.rawValue,
                "alignment": AlignmentMethod.none.rawValue,
            ]
        ),
    ])
    let model = PhotonStackWorkspaceModel(project: project, processingService: service)

    model.startReplayEditGraph()
    try await waitForStartedProcessingCompletion(in: model)

    #expect(await service.snapshot().stackInputs.isEmpty)
    #expect(model.latestJob?.status == .failed)
    #expect(model.errorMessage?.contains(model.localized(.editGraphInvalidRecordedAssets)) == true)
}

@Test @MainActor func disabledTerminalEditOperationReplaysToItsInput() async throws {
    let service = RecordingProcessingService()
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackDisabledTerminalReplay-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    let input = directory.appendingPathComponent("input.png")
    try writeSolidPNG(to: input, red: 1, green: 0, blue: 0, width: 16, height: 8)
    var project = PhotonStackProject(name: "Disabled Terminal Replay")
    project.addAssets(from: [input])
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: service,
        previewDirectory: directory.appendingPathComponent("previews", isDirectory: true)
    )

    await model.autoStretchPreview()
    let operation = try #require(model.project.editGraph.operations.last)
    let operationLayer = try #require(model.project.layers.first {
        $0.parameters["operationID"] == operation.id.uuidString
    })
    #expect(model.previewOperationID == operation.id)
    #expect(model.layerIsVisible(operationLayer.id))
    model.selectLayer(operationLayer)
    #expect(model.canvasMode == .layerComposite)
    #expect(model.previewOperationID == nil)

    model.toggleEditOperation(operation.id)
    #expect(model.editGraphNeedsReplay)
    #expect(model.previewOperationID == operation.id)
    #expect(model.canExportCurrentImage == false)
    #expect(model.layerIsVisible(operationLayer.id) == false)
    #expect(model.canShowLayer(operationLayer.id) == false)
    #expect(model.project.layers.first(where: { $0.id == operationLayer.id })?
        .parameters["visibilityBeforeOperationDisable"] == "true")
    model.setLayerVisibility(operationLayer.id, isVisible: true)
    #expect(model.layerIsVisible(operationLayer.id) == false)
    #expect(model.errorMessage == model.localized(.disabledOperationLayer))
    model.startReplayEditGraph()
    try await waitForStartedProcessingCompletion(in: model, minimumJobCount: 2)

    let snapshot = await service.snapshot()
    #expect(snapshot.autoStretchCalls == 1)
    #expect(model.previewURL == input)
    #expect(model.previewOperationID == operation.id)
    #expect(model.editGraphNeedsReplay == false)
    #expect(model.canExportCurrentImage)

    await model.exportCurrentImage(to: directory.appendingPathComponent("disabled-operation-export.tiff"))
    let exportSnapshot = await service.snapshot()
    #expect(exportSnapshot.autoStretchCalls == 1)
    #expect(exportSnapshot.convertInput == input)

    model.undoEditGraphChange()
    #expect(model.project.editGraph.operations.first(where: { $0.id == operation.id })?.isEnabled == true)
    #expect(model.layerIsVisible(operationLayer.id))
    #expect(model.canShowLayer(operationLayer.id))
    #expect(model.project.layers.first(where: { $0.id == operationLayer.id })?
        .parameters["visibilityBeforeOperationDisable"] == nil)

    model.redoEditGraphChange()
    #expect(model.project.editGraph.operations.first(where: { $0.id == operation.id })?.isEnabled == false)
    #expect(model.layerIsVisible(operationLayer.id) == false)
}

@Test @MainActor func disabledOperationLayerRestoresPriorManualVisibility() async throws {
    let service = RecordingProcessingService()
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackDisabledLayerVisibility-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    let input = directory.appendingPathComponent("input.png")
    try writeSolidPNG(to: input, red: 0.2, green: 0.4, blue: 0.8, width: 16, height: 8)
    var project = PhotonStackProject(name: "Disabled Layer Visibility")
    project.addAssets(from: [input])
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: service,
        previewDirectory: directory.appendingPathComponent("previews", isDirectory: true)
    )

    await model.autoStretchPreview()
    let operation = try #require(model.project.editGraph.operations.last)
    let operationLayer = try #require(model.project.layers.first {
        $0.parameters["operationID"] == operation.id.uuidString
    })
    model.setLayerVisibility(operationLayer.id, isVisible: false)
    #expect(model.layerIsVisible(operationLayer.id) == false)

    model.toggleEditOperation(operation.id)
    #expect(model.project.layers.first(where: { $0.id == operationLayer.id })?
        .parameters["visibilityBeforeOperationDisable"] == "false")
    model.toggleEditOperation(operation.id)

    #expect(model.project.editGraph.operations.first(where: { $0.id == operation.id })?.isEnabled == true)
    #expect(model.layerIsVisible(operationLayer.id) == false)
    #expect(model.project.layers.first(where: { $0.id == operationLayer.id })?
        .parameters["visibilityBeforeOperationDisable"] == nil)
}

@Test @MainActor func legacyVisibleLayerForDisabledOperationRequiresReplay() throws {
    let input = URL(fileURLWithPath: "/tmp/photonstack-disabled-operation-source.png")
    let output = URL(fileURLWithPath: "/tmp/photonstack-disabled-operation-preview.png")
    let operation = EditOperation(
        kind: .stretch,
        parameters: ["input": input.path, "output": output.path],
        isEnabled: false
    )
    let layer = ProcessingLayer(
        name: "Legacy disabled adjustment",
        kind: .adjustment,
        inputURL: output,
        parameters: [
            "source": "operation",
            "operationID": operation.id.uuidString,
        ]
    )
    var project = PhotonStackProject(name: "Legacy Disabled Layer")
    project.addAssets(from: [input])
    project.appendOperation(operation)
    project.appendLayer(layer)

    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: RecordingProcessingService()
    )

    #expect(model.layerIsVisible(layer.id) == false)
    #expect(model.project.layers.first(where: { $0.id == layer.id })?
        .parameters["visibilityBeforeOperationDisable"] == "true")
    #expect(model.editGraphNeedsReplay)
}

@Test @MainActor func loadingLegacyDisabledOperationLayerPersistsVisibilityRepair() async throws {
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackLoadDisabledLayerRepair-\(UUID().uuidString)", isDirectory: true)
    let projectDirectory = directory.appendingPathComponent("project", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    let input = directory.appendingPathComponent("input.png")
    let output = directory.appendingPathComponent("legacy-preview.png")
    try writeSolidPNG(to: input, red: 0.1, green: 0.3, blue: 0.7, width: 16, height: 8)
    try writeSolidPNG(to: output, red: 0.4, green: 0.5, blue: 0.6, width: 16, height: 8)
    let operation = EditOperation(
        kind: .stretch,
        parameters: ["input": input.path, "output": output.path],
        isEnabled: false
    )
    let layer = ProcessingLayer(
        name: "Legacy disabled adjustment",
        kind: .adjustment,
        inputURL: output,
        parameters: [
            "source": "operation",
            "operationID": operation.id.uuidString,
        ]
    )
    var project = PhotonStackProject(name: "Load Disabled Layer Repair")
    project.addAssets(from: [input])
    project.appendOperation(operation)
    project.appendLayer(layer)
    project.workspaceState = ProjectWorkspaceState(
        selectedAssetID: project.assets.first?.id,
        activeLayerID: layer.id,
        previewURL: output,
        previewOperationID: operation.id,
        editGraphNeedsReplay: false,
        canvasMode: .layerComposite
    )
    let repository = ProjectRepository()
    try await repository.save(project, to: projectDirectory)
    let model = PhotonStackWorkspaceModel(
        processingService: RecordingProcessingService(),
        projectRepository: repository,
        previewDirectory: directory.appendingPathComponent("previews", isDirectory: true)
    )

    await model.loadProject(from: projectDirectory)

    #expect(model.layerIsVisible(layer.id) == false)
    #expect(model.editGraphNeedsReplay)
    #expect(model.project.layers.first(where: { $0.id == layer.id })?
        .parameters["visibilityBeforeOperationDisable"] == "true")

    let deadline = ContinuousClock.now + .seconds(2)
    var persisted = try await repository.load(from: projectDirectory)
    while ContinuousClock.now < deadline,
          persisted.layers.first(where: { $0.id == layer.id })?.isVisible != false {
        try await Task.sleep(nanoseconds: 10_000_000)
        persisted = try await repository.load(from: projectDirectory)
    }
    #expect(persisted.layers.first(where: { $0.id == layer.id })?.isVisible == false)
    #expect(persisted.layers.first(where: { $0.id == layer.id })?
        .parameters["visibilityBeforeOperationDisable"] == "true")
    #expect(persisted.workspaceState?.editGraphNeedsReplay == true)
}

@Test @MainActor func editGraphCanReplayExternalObjectBranchWithoutSelectedAsset() async throws {
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackObjectReplayWithoutAsset-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    let source = directory.appendingPathComponent("external-object.png")
    let stalePreview = directory.appendingPathComponent("stale-preview.png")
    try writeSolidPNG(to: source, red: 0.1, green: 0.2, blue: 0.7, width: 16, height: 8)
    try writeSolidPNG(to: stalePreview, red: 0.4, green: 0.4, blue: 0.4, width: 16, height: 8)
    let operation = EditOperation(
        kind: .stretch,
        parameters: [
            "input": source.path,
            "output": stalePreview.path,
            "targetBackground": "0.31",
        ]
    )
    let layer = ProcessingLayer(
        name: "External Object · stretch",
        kind: .adjustment,
        inputURL: stalePreview,
        parameters: [
            "source": "operation",
            "operationID": operation.id.uuidString,
        ]
    )
    var project = PhotonStackProject(name: "Object Replay Without Asset")
    project.appendOperation(operation)
    project.appendLayer(layer)
    project.workspaceState = ProjectWorkspaceState(
        activeLayerID: layer.id,
        previewURL: stalePreview,
        previewOperationID: operation.id,
        editGraphNeedsReplay: true,
        canvasMode: .layerComposite
    )
    let service = RecordingProcessingService()
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: service,
        previewDirectory: directory.appendingPathComponent("previews", isDirectory: true)
    )

    #expect(model.selectedAsset == nil)
    #expect(model.canReplayEditGraph)
    #expect(model.canExportCurrentImage == false)

    model.startReplayEditGraph()
    try await waitForStartedProcessingCompletion(in: model)

    let snapshot = await service.snapshot()
    let replayedOutput = try #require(snapshot.autoStretchOutputs.last)
    #expect(snapshot.autoStretchInputs == [source])
    #expect(snapshot.autoStretchTargets == [0.31])
    #expect(model.previewURL == replayedOutput)
    #expect(model.project.layers.first(where: { $0.id == layer.id })?.inputURL == replayedOutput)
    #expect(model.editGraphNeedsReplay == false)
    #expect(model.canExportCurrentImage)

    await model.exportCurrentImage(to: directory.appendingPathComponent("external-object-export.tiff"))
    let exportSnapshot = await service.snapshot()
    #expect(exportSnapshot.autoStretchInputs == [source, source])
    #expect(exportSnapshot.convertInput == exportSnapshot.autoStretchOutputs.last)
    #expect(model.latestJob?.status == .succeeded)
}

@Test @MainActor func deletingEditOperationRewiresSuccessorAndUndoRestoresBranch() async throws {
    let service = RecordingProcessingService()
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackDeleteOperationBranch-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    let input = directory.appendingPathComponent("input.png")
    try writeSolidPNG(to: input, red: 0, green: 0, blue: 1, width: 16, height: 8)
    var project = PhotonStackProject(name: "Delete Operation Branch")
    project.addAssets(from: [input])
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: service,
        previewDirectory: directory.appendingPathComponent("previews", isDirectory: true)
    )

    await model.autoStretchPreview()
    await model.denoisePreview()
    let stretch = try #require(model.project.editGraph.operations.first { $0.kind == .stretch })
    let denoise = try #require(model.project.editGraph.operations.first { $0.kind == .denoise })
    let sourceLayer = try #require(model.project.layers.first { $0.parameters["source"] == "automatic" })
    let stretchLayer = try #require(model.project.layers.first {
        $0.parameters["operationID"] == stretch.id.uuidString
    })
    let denoiseLayer = try #require(model.project.layers.first {
        $0.parameters["operationID"] == denoise.id.uuidString
    })
    #expect(denoiseLayer.parameters["sourceLayerID"] == stretchLayer.id.uuidString)
    model.selectLayer(denoiseLayer)
    #expect(model.canvasMode == .layerComposite)
    #expect(model.previewOperationID == nil)

    model.deleteEditOperation(stretch.id)
    #expect(model.editGraphNeedsReplay)
    let rewiredDenoise = try #require(model.project.editGraph.operations.first { $0.id == denoise.id })
    #expect(rewiredDenoise.parameters["input"] == input.path)
    #expect(model.previewOperationID == denoise.id)
    #expect(model.project.layers.contains(where: { $0.id == stretchLayer.id }) == false)
    let rewiredDenoiseLayer = try #require(model.project.layers.first { $0.id == denoiseLayer.id })
    #expect(rewiredDenoiseLayer.parameters["sourceLayerID"] == sourceLayer.id.uuidString)

    model.startReplayEditGraph()
    try await waitForStartedProcessingCompletion(in: model, minimumJobCount: 3)
    var snapshot = await service.snapshot()
    #expect(snapshot.denoiseInput == input)
    #expect(model.editGraphNeedsReplay == false)
    #expect(model.previewOperationID == denoise.id)

    model.undoEditGraphChange()
    #expect(model.editGraphNeedsReplay)
    #expect(model.project.editGraph.operations.map(\.id) == [stretch.id, denoise.id])
    #expect(model.previewOperationID == nil)
    #expect(model.project.layers.contains(where: { $0.id == stretchLayer.id }))
    let restoredDenoiseLayer = try #require(model.project.layers.first { $0.id == denoiseLayer.id })
    #expect(restoredDenoiseLayer.parameters["sourceLayerID"] == stretchLayer.id.uuidString)
    model.startReplayEditGraph()
    try await waitForStartedProcessingCompletion(in: model, minimumJobCount: 4)
    snapshot = await service.snapshot()
    #expect(snapshot.autoStretchInputs.last == input)
    #expect(snapshot.denoiseInput == snapshot.autoStretchOutputs.last)
    #expect(model.editGraphNeedsReplay == false)
}

@Test @MainActor func replayingEditGraphRefreshesOperationBackedLayerInputs() async throws {
    let service = RecordingProcessingService()
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackReplayLayerInputs-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    let input = directory.appendingPathComponent("input.png")
    try writeSolidPNG(to: input, red: 0.1, green: 0.3, blue: 0.8, width: 16, height: 8)
    var project = PhotonStackProject(name: "Replay Layer Inputs")
    project.addAssets(from: [input])
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: service,
        previewDirectory: directory.appendingPathComponent("previews", isDirectory: true)
    )

    await model.autoStretchPreview()
    await model.denoisePreview()
    let stretch = try #require(model.project.editGraph.operations.first { $0.kind == .stretch })
    let denoise = try #require(model.project.editGraph.operations.first { $0.kind == .denoise })
    let originalStretchLayer = try #require(model.project.layers.first {
        $0.parameters["operationID"] == stretch.id.uuidString
    })
    let originalDenoiseLayer = try #require(model.project.layers.first {
        $0.parameters["operationID"] == denoise.id.uuidString
    })
    let originalStretchInput = try #require(originalStretchLayer.inputURL)
    let originalDenoiseInput = try #require(originalDenoiseLayer.inputURL)
    model.selectLayer(originalDenoiseLayer)
    #expect(model.canvasMode == .layerComposite)
    #expect(model.previewOperationID == nil)

    #expect(model.updateEditOperationParameter(
        stretch.id,
        key: "targetBackground",
        value: "0.42"
    ))
    #expect(model.editGraphNeedsReplay)
    #expect(model.previewOperationID == denoise.id)

    model.startReplayEditGraph()
    try await waitForStartedProcessingCompletion(in: model, minimumJobCount: 3)

    let replayedStretchLayer = try #require(model.project.layers.first {
        $0.id == originalStretchLayer.id
    })
    let replayedDenoiseLayer = try #require(model.project.layers.first {
        $0.id == originalDenoiseLayer.id
    })
    let replayedStretchInput = try #require(replayedStretchLayer.inputURL)
    let replayedDenoiseInput = try #require(replayedDenoiseLayer.inputURL)
    #expect(replayedStretchInput != originalStretchInput)
    #expect(replayedDenoiseInput != originalDenoiseInput)
    #expect(FileManager.default.fileExists(atPath: replayedStretchInput.path))
    #expect(FileManager.default.fileExists(atPath: replayedDenoiseInput.path))
    #expect(replayedDenoiseInput == model.previewURL)
    #expect(model.activeLayerID == originalDenoiseLayer.id)
    #expect(model.currentInputURL == replayedDenoiseInput)
    #expect(model.previewOperationID == denoise.id)
    #expect(model.editGraphNeedsReplay == false)
}

@Test @MainActor func movingEditOperationReordersAndRewiresOnlyItsBranch() async throws {
    let service = RecordingProcessingService()
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackMoveOperationBranch-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    let input = directory.appendingPathComponent("input.png")
    try writeSolidPNG(to: input, red: 0, green: 1, blue: 0, width: 16, height: 8)
    var project = PhotonStackProject(name: "Move Operation Branch")
    project.addAssets(from: [input])
    let model = PhotonStackWorkspaceModel(
        project: project,
        processingService: service,
        previewDirectory: directory.appendingPathComponent("previews", isDirectory: true)
    )

    await model.autoStretchPreview()
    await model.denoisePreview()
    let stretch = try #require(model.project.editGraph.operations.first { $0.kind == .stretch })
    let denoise = try #require(model.project.editGraph.operations.first { $0.kind == .denoise })
    let denoiseOutput = try #require(denoise.parameters["output"])
    let sourceLayer = try #require(model.project.layers.first { $0.parameters["source"] == "automatic" })
    let stretchLayer = try #require(model.project.layers.first {
        $0.parameters["operationID"] == stretch.id.uuidString
    })
    let denoiseLayer = try #require(model.project.layers.first {
        $0.parameters["operationID"] == denoise.id.uuidString
    })
    model.selectLayer(denoiseLayer)
    #expect(model.canvasMode == .layerComposite)
    #expect(model.previewOperationID == nil)

    model.moveEditOperation(denoise.id, direction: .up)
    #expect(model.editGraphNeedsReplay)
    #expect(model.project.editGraph.operations.map(\.id) == [denoise.id, stretch.id])
    #expect(model.project.editGraph.operations[0].parameters["input"] == input.path)
    #expect(model.project.editGraph.operations[1].parameters["input"] == denoiseOutput)
    #expect(model.previewOperationID == stretch.id)
    #expect(model.project.layers.map(\.id) == [sourceLayer.id, denoiseLayer.id, stretchLayer.id])
    #expect(model.project.layers.first(where: { $0.id == denoiseLayer.id })?
        .parameters["sourceLayerID"] == sourceLayer.id.uuidString)
    #expect(model.project.layers.first(where: { $0.id == stretchLayer.id })?
        .parameters["sourceLayerID"] == denoiseLayer.id.uuidString)

    model.undoEditGraphChange()
    #expect(model.project.editGraph.operations.map(\.id) == [stretch.id, denoise.id])
    #expect(model.project.layers.map(\.id) == [sourceLayer.id, stretchLayer.id, denoiseLayer.id])
    #expect(model.project.layers.first(where: { $0.id == denoiseLayer.id })?
        .parameters["sourceLayerID"] == stretchLayer.id.uuidString)
    model.redoEditGraphChange()
    #expect(model.project.editGraph.operations.map(\.id) == [denoise.id, stretch.id])
    #expect(model.project.layers.map(\.id) == [sourceLayer.id, denoiseLayer.id, stretchLayer.id])

    model.startReplayEditGraph()
    try await waitForStartedProcessingCompletion(in: model, minimumJobCount: 3)
    let replaySnapshot = await service.snapshot()
    let replayedDenoiseLayer = try #require(model.project.layers.first { $0.id == denoiseLayer.id })
    let replayedStretchLayer = try #require(model.project.layers.first { $0.id == stretchLayer.id })
    #expect(replayedDenoiseLayer.inputURL == replaySnapshot.autoStretchInputs.last)
    #expect(replayedStretchLayer.inputURL == replaySnapshot.autoStretchOutputs.last)
    #expect(replayedStretchLayer.inputURL == model.previewURL)
    #expect(model.previewOperationID == stretch.id)
    #expect(model.editGraphNeedsReplay == false)

    let otherInput = directory.appendingPathComponent("other.png")
    try writeSolidPNG(to: otherInput, red: 1, green: 1, blue: 0, width: 16, height: 8)
    model.importAssets(from: [otherInput])
    model.select(try #require(model.project.assets.first { $0.originalURL == otherInput }))
    await model.autoStretchPreview()
    let otherStretch = try #require(model.project.editGraph.operations.last)
    let operationOrder = model.project.editGraph.operations.map(\.id)
    model.moveEditOperation(otherStretch.id, direction: .up)
    #expect(model.project.editGraph.operations.map(\.id) == operationOrder)
}

private func writeSolidPNG(
    to url: URL,
    red: Double,
    green: Double,
    blue: Double,
    width: Int = 4,
    height: Int = 4
) throws {
    let image = CIImage(color: CIColor(red: red, green: green, blue: blue, alpha: 1))
        .cropped(to: CGRect(x: 0, y: 0, width: width, height: height))
    let context = CIContext()
    guard let colorSpace = CGColorSpace(name: CGColorSpace.sRGB) else {
        throw CocoaError(.coderInvalidValue)
    }
    try context.writePNGRepresentation(of: image, to: url, format: .RGBA8, colorSpace: colorSpace)
}

private func writeSyntheticImage(
    to url: URL,
    red: Double = 0.25,
    green: Double = 0.35,
    blue: Double = 0.45
) throws {
    if ["fit", "fits", "fts"].contains(url.pathExtension.lowercased()) {
        try writeMinimalFITS(to: url)
    } else {
        try writeSolidPNG(to: url, red: red, green: green, blue: blue, width: 2, height: 2)
    }
}

private func writeMinimalFITS(to url: URL, width: Int = 2, height: Int = 2) throws {
    try FileManager.default.createDirectory(
        at: url.deletingLastPathComponent(),
        withIntermediateDirectories: true
    )
    let cards = [
        "SIMPLE  =                    T",
        "BITPIX  =                  -32",
        "NAXIS   =                    3",
        String(format: "NAXIS1  = %20d", width),
        String(format: "NAXIS2  = %20d", height),
        "NAXIS3  =                    4",
        "EXTEND  =                    T",
        "END",
    ]
    var data = Data()
    for card in cards {
        let padded = String(card.prefix(80)).padding(toLength: 80, withPad: " ", startingAt: 0)
        data.append(contentsOf: padded.utf8)
    }
    let headerPadding = (2_880 - data.count % 2_880) % 2_880
    data.append(Data(repeating: 0x20, count: headerPadding))

    let pixelCount = width * height
    for channel in 0..<4 {
        for _ in 0..<pixelCount {
            let value: Float = channel == 3 ? 1 : Float(channel + 1) * 0.2
            var bits = value.bitPattern.bigEndian
            withUnsafeBytes(of: &bits) { data.append(contentsOf: $0) }
        }
    }
    let payloadPadding = (2_880 - data.count % 2_880) % 2_880
    data.append(Data(repeating: 0, count: payloadPadding))
    try data.write(to: url, options: .atomic)
}

private func writeOrientedJPEG(to url: URL, width: Int, height: Int, orientation: Int) throws {
    let extent = CGRect(x: 0, y: 0, width: width, height: height)
    let image = CIImage(color: CIColor(red: 0.08, green: 0.10, blue: 0.18, alpha: 1))
        .cropped(to: extent)
    let context = CIContext()
    guard let colorSpace = CGColorSpace(name: CGColorSpace.sRGB),
          let rendered = context.createCGImage(image, from: extent, format: .RGBA8, colorSpace: colorSpace),
          let destination = CGImageDestinationCreateWithURL(
              url as CFURL,
              "public.jpeg" as CFString,
              1,
              nil
          )
    else {
        throw CocoaError(.coderInvalidValue)
    }
    CGImageDestinationAddImage(destination, rendered, [
        kCGImagePropertyOrientation: orientation,
        kCGImageDestinationLossyCompressionQuality: 0.95,
    ] as CFDictionary)
    guard CGImageDestinationFinalize(destination) else {
        throw CocoaError(.fileWriteUnknown)
    }
}

private func writeSplitMaskPNG(to url: URL, width: Int, height: Int) throws {
    let extent = CGRect(x: 0, y: 0, width: width, height: height)
    let black = CIImage(color: CIColor(red: 0, green: 0, blue: 0, alpha: 1)).cropped(to: extent)
    let white = CIImage(color: CIColor(red: 1, green: 1, blue: 1, alpha: 1))
        .cropped(to: CGRect(x: width / 2, y: 0, width: width - width / 2, height: height))
    let image = white.composited(over: black).cropped(to: extent)
    let context = CIContext()
    guard let colorSpace = CGColorSpace(name: CGColorSpace.sRGB) else {
        throw CocoaError(.coderInvalidValue)
    }
    try context.writePNGRepresentation(of: image, to: url, format: .RGBA8, colorSpace: colorSpace)
}

private func writeTransparentSplitMaskPNG(to url: URL, width: Int, height: Int) throws {
    let extent = CGRect(x: 0, y: 0, width: width, height: height)
    let transparent = CIImage(color: .clear).cropped(to: extent)
    let white = CIImage(color: CIColor(red: 1, green: 1, blue: 1, alpha: 1))
        .cropped(to: CGRect(x: width / 2, y: 0, width: width - width / 2, height: height))
    let image = white.composited(over: transparent).cropped(to: extent)
    let context = CIContext()
    guard let colorSpace = CGColorSpace(name: CGColorSpace.sRGB) else {
        throw CocoaError(.coderInvalidValue)
    }
    try context.writePNGRepresentation(of: image, to: url, format: .RGBA8, colorSpace: colorSpace)
}

private func writeHalfTransparentPNG(
    to url: URL,
    red: Double,
    green: Double,
    blue: Double,
    width: Int,
    height: Int
) throws {
    let extent = CGRect(x: 0, y: 0, width: width, height: height)
    let transparent = CIImage(color: .clear).cropped(to: extent)
    let color = CIImage(color: CIColor(red: red, green: green, blue: blue, alpha: 1))
        .cropped(to: CGRect(x: width / 2, y: 0, width: width - width / 2, height: height))
    let image = color.composited(over: transparent).cropped(to: extent)
    let context = CIContext()
    guard let colorSpace = CGColorSpace(name: CGColorSpace.sRGB) else {
        throw CocoaError(.coderInvalidValue)
    }
    try context.writePNGRepresentation(of: image, to: url, format: .RGBA8, colorSpace: colorSpace)
}

private func rgbaPixel(in url: URL, x: Int, y: Int) throws -> (red: UInt8, green: UInt8, blue: UInt8, alpha: UInt8) {
    guard let image = CIImage(contentsOf: url),
          let colorSpace = CGColorSpace(name: CGColorSpace.sRGB)
    else {
        throw CocoaError(.fileReadCorruptFile)
    }
    var bytes = [UInt8](repeating: 0, count: 4)
    CIContext().render(
        image,
        toBitmap: &bytes,
        rowBytes: 4,
        bounds: CGRect(x: x, y: y, width: 1, height: 1),
        format: .RGBA8,
        colorSpace: colorSpace
    )
    return (bytes[0], bytes[1], bytes[2], bytes[3])
}

@MainActor
private func waitForSelectedMetadata(
    in model: PhotonStackWorkspaceModel,
    timeoutNanoseconds: UInt64 = 5_000_000_000
) async throws -> AssetMetadata {
    let deadline = ContinuousClock.now + .nanoseconds(Int(timeoutNanoseconds))
    while ContinuousClock.now < deadline {
        if let metadata = model.selectedAsset?.metadata {
            return metadata
        }
        try await Task.sleep(nanoseconds: 10_000_000)
    }
    return try #require(model.selectedAsset?.metadata)
}

@MainActor
private func waitForBatchCompletion(
    in model: PhotonStackWorkspaceModel,
    timeoutNanoseconds: UInt64 = 5_000_000_000
) async throws {
    let deadline = ContinuousClock.now + .nanoseconds(Int(timeoutNanoseconds))
    while ContinuousClock.now < deadline {
        if model.isBatchRunning == false {
            return
        }
        try await Task.sleep(nanoseconds: 10_000_000)
    }
    #expect(model.isBatchRunning == false)
}

@MainActor
private func waitForProcessingCompletion(
    in model: PhotonStackWorkspaceModel,
    timeoutNanoseconds: UInt64 = 5_000_000_000
) async throws {
    let deadline = ContinuousClock.now + .nanoseconds(Int(timeoutNanoseconds))
    while ContinuousClock.now < deadline {
        if model.isProcessing == false {
            return
        }
        try await Task.sleep(nanoseconds: 10_000_000)
    }
    #expect(model.isProcessing == false)
}

@MainActor
private func waitForStartedProcessingCompletion(
    in model: PhotonStackWorkspaceModel,
    minimumJobCount: Int = 1,
    timeoutNanoseconds: UInt64 = 5_000_000_000
) async throws {
    let deadline = ContinuousClock.now + .nanoseconds(Int(timeoutNanoseconds))
    while ContinuousClock.now < deadline {
        if model.jobs.count >= minimumJobCount, model.isProcessing == false {
            return
        }
        try await Task.sleep(nanoseconds: 10_000_000)
    }
    #expect(model.jobs.count >= minimumJobCount)
    #expect(model.isProcessing == false)
}

private func waitForSavedProject(
    in directory: URL,
    repository: ProjectRepository,
    expectedRole: CalibrationFrameRole,
    timeoutNanoseconds: UInt64 = 5_000_000_000
) async throws -> PhotonStackProject {
    let deadline = ContinuousClock.now + .nanoseconds(Int(timeoutNanoseconds))
    var lastLoaded: PhotonStackProject?
    while ContinuousClock.now < deadline {
        if let loaded = try? await repository.load(from: directory) {
            lastLoaded = loaded
            if loaded.assets.first?.role == expectedRole {
                return loaded
            }
        }
        try await Task.sleep(nanoseconds: 10_000_000)
    }
    return try #require(lastLoaded)
}

private func waitForProgressJobID(
    from service: RecordingProcessingService,
    timeoutNanoseconds: UInt64 = 5_000_000_000
) async throws -> UUID {
    let deadline = ContinuousClock.now + .nanoseconds(Int(timeoutNanoseconds))
    while ContinuousClock.now < deadline {
        if let jobID = await service.progressJobID() {
            return jobID
        }
        try await Task.sleep(nanoseconds: 10_000_000)
    }
    return try #require(await service.progressJobID())
}

private func waitForAutoStretchCall(
    from service: RecordingProcessingService,
    minimum: Int = 1,
    timeoutNanoseconds: UInt64 = 5_000_000_000
) async throws {
    let deadline = ContinuousClock.now + .nanoseconds(Int(timeoutNanoseconds))
    while ContinuousClock.now < deadline {
        if await service.snapshot().autoStretchCalls >= minimum {
            return
        }
        try await Task.sleep(nanoseconds: 10_000_000)
    }
    #expect(await service.snapshot().autoStretchCalls >= minimum)
}

private func waitForCurvePreviewCall(
    from service: RecordingProcessingService,
    minimum: Int = 1,
    timeoutNanoseconds: UInt64 = 5_000_000_000
) async throws {
    let deadline = ContinuousClock.now + .nanoseconds(Int(timeoutNanoseconds))
    while ContinuousClock.now < deadline {
        if await service.snapshot().makeCurvePreviewCalls >= minimum {
            return
        }
        try await Task.sleep(nanoseconds: 10_000_000)
    }
    #expect(await service.snapshot().makeCurvePreviewCalls >= minimum)
}

@MainActor
private func waitForCurvePreviewCompletion(
    in model: PhotonStackWorkspaceModel,
    timeoutNanoseconds: UInt64 = 5_000_000_000
) async throws {
    let deadline = ContinuousClock.now + .nanoseconds(Int(timeoutNanoseconds))
    while ContinuousClock.now < deadline {
        if model.commandOutput == "Curve preview" {
            return
        }
        try await Task.sleep(nanoseconds: 10_000_000)
    }
    #expect(model.commandOutput == "Curve preview")
}

private func waitForArtifactDetectCall(
    from service: RecordingProcessingService,
    minimum: Int,
    timeoutNanoseconds: UInt64 = 5_000_000_000
) async {
    let deadline = ContinuousClock.now + .nanoseconds(Int(timeoutNanoseconds))
    while ContinuousClock.now < deadline {
        if await service.snapshot().artifactDetectCalls >= minimum {
            return
        }
        try? await Task.sleep(nanoseconds: 10_000_000)
    }
    #expect(await service.snapshot().artifactDetectCalls >= minimum)
}

private func waitForRawAdjustOptions(
    from service: RecordingProcessingService,
    timeoutNanoseconds: UInt64 = 5_000_000_000
) async throws -> RawProcessingOptions {
    let deadline = ContinuousClock.now + .nanoseconds(Int(timeoutNanoseconds))
    while ContinuousClock.now < deadline {
        if let options = await service.snapshot().rawAdjustOptions {
            return options
        }
        try await Task.sleep(nanoseconds: 10_000_000)
    }
    return try #require(await service.snapshot().rawAdjustOptions)
}

private func waitForRawAdjustCall(
    _ minimum: Int,
    from service: RecordingProcessingService,
    timeoutNanoseconds: UInt64 = 5_000_000_000
) async throws {
    let deadline = ContinuousClock.now + .nanoseconds(Int(timeoutNanoseconds))
    while ContinuousClock.now < deadline {
        if await service.snapshot().rawAdjustCalls >= minimum {
            return
        }
        try await Task.sleep(nanoseconds: 10_000_000)
    }
    #expect(await service.snapshot().rawAdjustCalls >= minimum)
}

private func waitForRawAdjustCompletion(
    _ minimum: Int,
    from service: RecordingProcessingService,
    timeoutNanoseconds: UInt64 = 5_000_000_000
) async throws {
    let deadline = ContinuousClock.now + .nanoseconds(Int(timeoutNanoseconds))
    while ContinuousClock.now < deadline {
        if await service.snapshot().rawAdjustCompletions >= minimum {
            return
        }
        try await Task.sleep(nanoseconds: 10_000_000)
    }
    #expect(await service.snapshot().rawAdjustCompletions >= minimum)
}

@MainActor
private func waitForRawPreviewCompletion(
    in model: PhotonStackWorkspaceModel,
    timeoutNanoseconds: UInt64 = 5_000_000_000
) async throws {
    let deadline = ContinuousClock.now + .nanoseconds(Int(timeoutNanoseconds))
    while ContinuousClock.now < deadline {
        if model.isRawPreviewUpdating == false {
            return
        }
        try await Task.sleep(nanoseconds: 10_000_000)
    }
    #expect(model.isRawPreviewUpdating == false)
}

private func waitForInspectCall(
    from service: RecordingProcessingService,
    timeoutNanoseconds: UInt64 = 5_000_000_000
) async throws {
    let deadline = ContinuousClock.now + .nanoseconds(Int(timeoutNanoseconds))
    while ContinuousClock.now < deadline {
        if await service.snapshot().inspectCalls > 0 {
            return
        }
        try await Task.sleep(nanoseconds: 10_000_000)
    }
    #expect(await service.snapshot().inspectCalls > 0)
}

private func waitForHistogramCall(
    from service: RecordingProcessingService,
    minimum: Int = 1,
    timeoutNanoseconds: UInt64 = 5_000_000_000
) async throws {
    let deadline = ContinuousClock.now + .nanoseconds(Int(timeoutNanoseconds))
    while ContinuousClock.now < deadline {
        if await service.histogramCalls() >= minimum {
            return
        }
        try await Task.sleep(nanoseconds: 10_000_000)
    }
    #expect(await service.histogramCalls() >= minimum)
}

private func waitForSettingsLoadStart(
    in store: ControllableAppSettingsStore,
    timeoutNanoseconds: UInt64 = 5_000_000_000
) async throws {
    let deadline = ContinuousClock.now + .nanoseconds(Int(timeoutNanoseconds))
    while ContinuousClock.now < deadline {
        if await store.loadHasStarted() {
            return
        }
        try await Task.sleep(nanoseconds: 10_000_000)
    }
    #expect(await store.loadHasStarted())
}

private func waitForSettingsSaveCount(
    _ minimum: Int,
    in store: ControllableAppSettingsStore,
    timeoutNanoseconds: UInt64 = 5_000_000_000
) async throws {
    let deadline = ContinuousClock.now + .nanoseconds(Int(timeoutNanoseconds))
    while ContinuousClock.now < deadline {
        if await store.saveCount() >= minimum {
            return
        }
        try await Task.sleep(nanoseconds: 10_000_000)
    }
    #expect(await store.saveCount() >= minimum)
}

private actor ControllableAppSettingsStore: AppSettingsStoring {
    private var settings: AppSettings
    private let holdLoad: Bool
    private let delayFirstSaveNanoseconds: UInt64
    private let failingSaveIndices: Set<Int>
    private var loadStarted = false
    private var loadContinuation: CheckedContinuation<Void, Never>?
    private var completedSaveCount = 0

    init(
        settings: AppSettings,
        holdLoad: Bool = false,
        delayFirstSaveNanoseconds: UInt64 = 0,
        failingSaveIndices: Set<Int> = []
    ) {
        self.settings = settings
        self.holdLoad = holdLoad
        self.delayFirstSaveNanoseconds = delayFirstSaveNanoseconds
        self.failingSaveIndices = failingSaveIndices
    }

    func load() async throws -> AppSettings {
        let snapshot = settings
        loadStarted = true
        if holdLoad {
            await withCheckedContinuation { continuation in
                loadContinuation = continuation
            }
        }
        return snapshot
    }

    func save(_ settings: AppSettings) async throws {
        completedSaveCount += 1
        let callIndex = completedSaveCount
        if callIndex == 1, delayFirstSaveNanoseconds > 0 {
            try await Task.sleep(nanoseconds: delayFirstSaveNanoseconds)
        }
        if failingSaveIndices.contains(callIndex) {
            throw ControllableSettingsStoreError.writeFailed
        }
        self.settings = settings
    }

    func releaseLoad() {
        loadContinuation?.resume()
        loadContinuation = nil
    }

    func loadHasStarted() -> Bool {
        loadStarted
    }

    func saveCount() -> Int {
        completedSaveCount
    }

    func currentSettings() -> AppSettings {
        settings
    }
}

private enum ControllableSettingsStoreError: LocalizedError {
    case writeFailed

    var errorDescription: String? {
        "Test settings write failed"
    }
}

private actor RecordingProcessingService: ProcessingService {
    nonisolated let requiresExistingInputFiles: Bool
    private let autoStretchDelayNanoseconds: UInt64
    private let subsequentAutoStretchDelayNanoseconds: UInt64?
    private let ignoreAutoStretchCancellation: Bool
    private let autoStretchProgressToEmit: Double?
    private let stackDelayNanoseconds: UInt64
    private let stackProgressToEmit: Double?
    private let failQuality: Bool
    private let artifactDetectDelayNanoseconds: UInt64
    private let inspectDelayNanoseconds: UInt64
    private let ignoreInspectCancellation: Bool
    private let inspectOutputOverride: String?
    private let previewOutputOverride: String?
    private let rawAdjustDelayNanoseconds: UInt64
    private let ignoreRawAdjustCancellation: Bool
    private let histogramDelayNanoseconds: UInt64
    private let ignoreHistogramCancellation: Bool
    private let failingHistogramCallIndices: Set<Int>
    private let histogramOutputOverride: String?
    private let starDetectOutputOverride: String?
    private let starMaskOutputOverride: String?
    private let artifactDetectOutputOverride: String?
    private let cloudDetectOutputOverride: String?
    private let failCloudRemoval: Bool
    private let failArtifactRemoval: Bool
    private let registerOutputOverride: String?
    private let mosaicOutputOverride: String?
    private var inspectCallCount = 0
    private var makePreviewCallCount = 0
    private var makeCurvePreviewCallCount = 0
    private var lastCurvePreviewWidth: Int?
    private var calibrateCallCount = 0
    private var masterInputGroups: [[URL]] = []
    private var masterMethods: [StackMethod] = []
    private var masterRawOptions: [RawProcessingOptions] = []
    private var calibrateLights: [URL] = []
    private var calibrateDarkInputs: [URL?] = []
    private var calibrateBiasInputs: [URL?] = []
    private var calibrateFlatInputs: [URL?] = []
    private var calibrateDarkBiasStates: [CalibrationBiasState] = []
    private var calibrateFlatBiasStates: [CalibrationBiasState] = []
    private var calibrateRawOptions: [RawProcessingOptions] = []
    private var convertCallCount = 0
    private var lastConvertInput: URL?
    private var convertInputs: [URL] = []
    private var convertOutputs: [URL] = []
    private var convertOptions: [ExportOptions] = []
    private var convertRawOptions: [RawProcessingOptions] = []
    private var lastStackInputs: [URL] = []
    private var lastDenoiseInput: URL?
    private var autoStretchCallCount = 0
    private var autoStretchInputs: [URL] = []
    private var autoStretchOutputs: [URL] = []
    private var autoStretchTargets: [Double] = []
    private var lastBackgroundStrength: Double?
    private var lastBackgroundPreserveBrightness: Bool?
    private var lastBackgroundProtectBrightTargets: Bool?
    private var lastRemoveGreenAmount: Double?
    private var lastRemoveGreenBackgroundLimit: Double?
    private var starDetectCallCount = 0
    private var lastStarDetectionThreshold: Double?
    private var lastStarMinimumPeak: Double?
    private var lastStarMaximumCount: Int?
    private var starMaskCallCount = 0
    private var starMaskInputs: [URL] = []
    private var starMaskOutputs: [URL] = []
    private var lastStarMaskRadius: Int?
    private var lastLargeStarMaskRadius: Int?
    private var lastLayeredStarMask: Bool?
    private var lastStarMaskMaximumCount: Int?
    private var cloudDetectCallCount = 0
    private var lastCloudSelectedIndices: [Int]?
    private var lastCloudRemovalStrength: Double?
    private var artifactDetectCallCount = 0
    private var lastArtifactSelectedIndices: [Int]?
    private var lastArtifactRemoveSatellites: Bool?
    private var lastArtifactRemoveMeteors: Bool?
    private var lastCleanSequenceInputs: [URL] = []
    private var lastCleanSequenceOutputDirectory: URL?
    private var lastCleanSequenceOutputFormat: String?
    private let cleanSequenceOutputOverride: String?
    private let cleanSequenceOutputExtensionOverride: String?
    private let omitLastCleanSequenceOutputFile: Bool
    private var restoreMeteorCallCount = 0
    private var restoreMeteorSources: [URL] = []
    private var registerBatchCallCount = 0
    private var lastRegisterBatchReference: URL?
    private var lastRegisterBatchInputs: [URL] = []
    private var lastRegisterBatchAlignment: AlignmentMethod?
    private var lastRegisterBatchOutputFormat: String?
    private let registerBatchOutputOverride: String?
    private let invalidRegisterBatchReferenceFlags: Bool
    private let omitLastRegisterBatchOutputFile: Bool
    private var lastCLIArguments: [String] = []
    private var rawAdjustCallCount = 0
    private var rawAdjustCompletionCount = 0
    private var rawAdjustOptions: [RawProcessingOptions] = []
    private var lastRawAdjustOptions: RawProcessingOptions?
    private var histogramCallCount = 0
    private var lastProgressJobID: UUID?
    private var lastDrizzleScale: Int?
    private var lastDrizzlePixfrac: Double?
    private var lastDrizzleAlignment: AlignmentMethod?
    private var drizzleInputGroups: [[URL]] = []
    private var mosaicInputGroups: [[URL]] = []

    init(
        autoStretchDelayNanoseconds: UInt64 = 0,
        subsequentAutoStretchDelayNanoseconds: UInt64? = nil,
        ignoreAutoStretchCancellation: Bool = false,
        autoStretchProgressToEmit: Double? = nil,
        stackDelayNanoseconds: UInt64 = 0,
        stackProgressToEmit: Double? = nil,
        failQuality: Bool = false,
        artifactDetectDelayNanoseconds: UInt64 = 0,
        inspectDelayNanoseconds: UInt64 = 0,
        ignoreInspectCancellation: Bool = false,
        inspectOutputOverride: String? = nil,
        previewOutputOverride: String? = nil,
        rawAdjustDelayNanoseconds: UInt64 = 0,
        ignoreRawAdjustCancellation: Bool = false,
        histogramDelayNanoseconds: UInt64 = 0,
        ignoreHistogramCancellation: Bool = false,
        failingHistogramCallIndices: Set<Int> = [],
        histogramOutputOverride: String? = nil,
        starDetectOutputOverride: String? = nil,
        starMaskOutputOverride: String? = nil,
        artifactDetectOutputOverride: String? = nil,
        cloudDetectOutputOverride: String? = nil,
        failCloudRemoval: Bool = false,
        failArtifactRemoval: Bool = false,
        registerOutputOverride: String? = nil,
        mosaicOutputOverride: String? = nil,
        requiresExistingInputFiles: Bool = false,
        registerBatchOutputOverride: String? = nil,
        invalidRegisterBatchReferenceFlags: Bool = false,
        cleanSequenceOutputOverride: String? = nil,
        cleanSequenceOutputExtensionOverride: String? = nil,
        omitLastRegisterBatchOutputFile: Bool = false,
        omitLastCleanSequenceOutputFile: Bool = false
    ) {
        self.requiresExistingInputFiles = requiresExistingInputFiles
        self.autoStretchDelayNanoseconds = autoStretchDelayNanoseconds
        self.subsequentAutoStretchDelayNanoseconds = subsequentAutoStretchDelayNanoseconds
        self.ignoreAutoStretchCancellation = ignoreAutoStretchCancellation
        self.autoStretchProgressToEmit = autoStretchProgressToEmit
        self.stackDelayNanoseconds = stackDelayNanoseconds
        self.stackProgressToEmit = stackProgressToEmit
        self.failQuality = failQuality
        self.artifactDetectDelayNanoseconds = artifactDetectDelayNanoseconds
        self.inspectDelayNanoseconds = inspectDelayNanoseconds
        self.ignoreInspectCancellation = ignoreInspectCancellation
        self.inspectOutputOverride = inspectOutputOverride
        self.previewOutputOverride = previewOutputOverride
        self.rawAdjustDelayNanoseconds = rawAdjustDelayNanoseconds
        self.ignoreRawAdjustCancellation = ignoreRawAdjustCancellation
        self.histogramDelayNanoseconds = histogramDelayNanoseconds
        self.ignoreHistogramCancellation = ignoreHistogramCancellation
        self.failingHistogramCallIndices = failingHistogramCallIndices
        self.histogramOutputOverride = histogramOutputOverride
        self.starDetectOutputOverride = starDetectOutputOverride
        self.starMaskOutputOverride = starMaskOutputOverride
        self.artifactDetectOutputOverride = artifactDetectOutputOverride
        self.cloudDetectOutputOverride = cloudDetectOutputOverride
        self.failCloudRemoval = failCloudRemoval
        self.failArtifactRemoval = failArtifactRemoval
        self.registerOutputOverride = registerOutputOverride
        self.mosaicOutputOverride = mosaicOutputOverride
        self.registerBatchOutputOverride = registerBatchOutputOverride
        self.invalidRegisterBatchReferenceFlags = invalidRegisterBatchReferenceFlags
        self.cleanSequenceOutputOverride = cleanSequenceOutputOverride
        self.cleanSequenceOutputExtensionOverride = cleanSequenceOutputExtensionOverride
        self.omitLastRegisterBatchOutputFile = omitLastRegisterBatchOutputFile
        self.omitLastCleanSequenceOutputFile = omitLastCleanSequenceOutputFile
    }

    func progressJobID() -> UUID? {
        lastProgressJobID
    }

    func histogramCalls() -> Int {
        histogramCallCount
    }

    func snapshot() -> Snapshot {
        Snapshot(
            inspectCalls: inspectCallCount,
            makePreviewCalls: makePreviewCallCount,
            makeCurvePreviewCalls: makeCurvePreviewCallCount,
            curvePreviewWidth: lastCurvePreviewWidth,
            calibrateCalls: calibrateCallCount,
            masterInputGroups: masterInputGroups,
            masterMethods: masterMethods,
            masterRawOptions: masterRawOptions,
            calibrateLights: calibrateLights,
            calibrateDarkInputs: calibrateDarkInputs,
            calibrateBiasInputs: calibrateBiasInputs,
            calibrateFlatInputs: calibrateFlatInputs,
            calibrateDarkBiasStates: calibrateDarkBiasStates,
            calibrateFlatBiasStates: calibrateFlatBiasStates,
            calibrateRawOptions: calibrateRawOptions,
            convertCalls: convertCallCount,
            convertInput: lastConvertInput,
            convertInputs: convertInputs,
            convertOutputs: convertOutputs,
            convertOptions: convertOptions,
            convertRawOptions: convertRawOptions,
            stackInputs: lastStackInputs,
            denoiseInput: lastDenoiseInput,
            autoStretchCalls: autoStretchCallCount,
            autoStretchInputs: autoStretchInputs,
            autoStretchOutputs: autoStretchOutputs,
            autoStretchTargets: autoStretchTargets,
            backgroundStrength: lastBackgroundStrength,
            backgroundPreserveBrightness: lastBackgroundPreserveBrightness,
            backgroundProtectBrightTargets: lastBackgroundProtectBrightTargets,
            removeGreenAmount: lastRemoveGreenAmount,
            removeGreenBackgroundLimit: lastRemoveGreenBackgroundLimit,
            starDetectCalls: starDetectCallCount,
            starDetectionThreshold: lastStarDetectionThreshold,
            starMinimumPeak: lastStarMinimumPeak,
            starMaximumCount: lastStarMaximumCount,
            starMaskCalls: starMaskCallCount,
            starMaskInputs: starMaskInputs,
            starMaskOutputs: starMaskOutputs,
            starMaskRadius: lastStarMaskRadius,
            largeStarMaskRadius: lastLargeStarMaskRadius,
            layeredStarMask: lastLayeredStarMask,
            starMaskMaximumCount: lastStarMaskMaximumCount,
            cloudDetectCalls: cloudDetectCallCount,
            cloudSelectedIndices: lastCloudSelectedIndices,
            cloudRemovalStrength: lastCloudRemovalStrength,
            artifactDetectCalls: artifactDetectCallCount,
            artifactSelectedIndices: lastArtifactSelectedIndices,
            artifactRemoveSatellites: lastArtifactRemoveSatellites,
            artifactRemoveMeteors: lastArtifactRemoveMeteors,
            cleanSequenceInputs: lastCleanSequenceInputs,
            cleanSequenceOutputDirectory: lastCleanSequenceOutputDirectory,
            cleanSequenceOutputFormat: lastCleanSequenceOutputFormat,
            restoreMeteorCalls: restoreMeteorCallCount,
            restoreMeteorSources: restoreMeteorSources,
            registerBatchCalls: registerBatchCallCount,
            registerBatchReference: lastRegisterBatchReference,
            registerBatchInputs: lastRegisterBatchInputs,
            registerBatchAlignment: lastRegisterBatchAlignment,
            registerBatchOutputFormat: lastRegisterBatchOutputFormat,
            cliArguments: lastCLIArguments,
            rawAdjustCalls: rawAdjustCallCount,
            rawAdjustCompletions: rawAdjustCompletionCount,
            rawAdjustOptionHistory: rawAdjustOptions,
            rawAdjustOptions: lastRawAdjustOptions,
            mosaicInputGroups: mosaicInputGroups,
            drizzleInputGroups: drizzleInputGroups,
            drizzleScale: lastDrizzleScale,
            drizzlePixfrac: lastDrizzlePixfrac,
            drizzleAlignment: lastDrizzleAlignment
        )
    }

    func inspect(_ asset: PhotonStackAsset) async throws -> ProcessingCommandResult {
        _ = asset
        inspectCallCount += 1
        if inspectDelayNanoseconds > 0 {
            if ignoreInspectCancellation {
                let delay = inspectDelayNanoseconds
                _ = try? await Task.detached {
                    try await Task.sleep(nanoseconds: delay)
                }.value
            } else {
                try await Task.sleep(nanoseconds: inspectDelayNanoseconds)
            }
        }
        if let inspectOutputOverride {
            return success("inspect", output: inspectOutputOverride)
        }
        return success("inspect", output: """
        {"diagnostic":true,"message":"preflight"}
        sysctlbyname for kern.hv_vmm_present failed
        {
          "width": 6048,
          "height": 4032,
          "channels": 4,
          "bitsPerChannel": 16,
          "format": "RAW",
          "metadata": {
            "cameraMake": "NIKON CORPORATION",
            "cameraModel": "Z6_3",
            "lensModel": "Viltrox AF 16/1.8 Z",
            "captureDate": "2025:08:16 20:12:42",
            "whiteBalance": "manual",
            "exposureBias": "-0.67 EV",
            "exposureTimeSeconds": 25,
            "fNumber": 1.8,
            "focalLengthMM": 16,
            "iso": 1600
          }
        }
        {"diagnostic":true,"message":"postflight"}
        postflight diagnostic after inspect
        """)
    }

    func convert(input: URL, output: URL, options: ExportOptions, rawOptions: RawProcessingOptions) async throws -> ProcessingCommandResult {
        convertCallCount += 1
        lastConvertInput = input
        convertInputs.append(input)
        convertOutputs.append(output)
        convertOptions.append(options)
        convertRawOptions.append(rawOptions)
        materializeCopyIfPossible(input: input, output: output)
        return success("convert")
    }

    func createMaster(output: URL, method: StackMethod, inputs: [URL], rawOptions: RawProcessingOptions) async throws -> ProcessingCommandResult {
        masterInputGroups.append(inputs)
        masterMethods.append(method)
        masterRawOptions.append(rawOptions)
        try? writeSyntheticImage(to: output)
        return success("master")
    }

    func calibrate(
        light: URL,
        output: URL,
        dark: URL?,
        bias: URL?,
        flat: URL?,
        darkBiasState: CalibrationBiasState,
        flatBiasState: CalibrationBiasState,
        rawOptions: RawProcessingOptions
    ) async throws -> ProcessingCommandResult {
        calibrateCallCount += 1
        calibrateLights.append(light)
        calibrateDarkInputs.append(dark)
        calibrateBiasInputs.append(bias)
        calibrateFlatInputs.append(flat)
        calibrateDarkBiasStates.append(darkBiasState)
        calibrateFlatBiasStates.append(flatBiasState)
        calibrateRawOptions.append(rawOptions)
        try? writeSyntheticImage(to: output)
        return success("calibrate")
    }

    func quality(input: URL) async throws -> ProcessingCommandResult {
        if failQuality {
            throw ProcessingServiceError.invalidReport(command: "quality")
        }
        return success("quality")
    }

    func register(reference: URL, moving: URL, output: URL, alignment: AlignmentMethod) async throws -> ProcessingCommandResult {
        _ = (reference, moving)
        if let registerOutputOverride {
            return success("register", output: registerOutputOverride)
        }
        try? writeSyntheticImage(to: output)
        return ProcessingCommandResult(
            command: ["register"],
            exitCode: 0,
            standardOutput: #"""
            sysctlbyname for kern.hv_vmm_present failed
            {"type":"complete","command":"register","mode":"\#(alignment.rawValue)","dx":1,"dy":2,"scale":1,"rotationRadians":0,"matches":8,"detectedReferenceStars":60,"detectedMovingStars":58,"inlierRatio":0.7,"usedFallback":false,"output":"\#(output.path)"}
            postflight diagnostic after register
            """#,
            standardError: ""
        )
    }

    func registerBatch(reference: URL, inputs: [URL], outputDirectory: URL, alignment: AlignmentMethod, outputFormat: String) async throws -> ProcessingCommandResult {
        registerBatchCallCount += 1
        lastRegisterBatchReference = reference
        lastRegisterBatchInputs = inputs
        lastRegisterBatchAlignment = alignment
        lastRegisterBatchOutputFormat = outputFormat
        if let registerBatchOutputOverride {
            return success("register-batch", output: registerBatchOutputOverride)
        }
        try? FileManager.default.createDirectory(at: outputDirectory, withIntermediateDirectories: true)
        let outputExtension = outputFormat == "fits" ? "fits" : (outputFormat == "png" ? "png" : "tiff")
        let referenceOutput = outputDirectory.appendingPathComponent("aligned-ref.\(outputExtension)")
        try? writeSyntheticImage(to: referenceOutput, red: 0.2, green: 0.3, blue: 0.4)
        let movingItems = inputs.enumerated().map { index, input in
            let outputName = index == 0
                ? "aligned-mov.\(outputExtension)"
                : "aligned-mov\(index + 1).\(outputExtension)"
            let output = outputDirectory.appendingPathComponent(outputName)
            if omitLastRegisterBatchOutputFile == false || index != inputs.indices.last {
                try? writeSyntheticImage(to: output, red: 0.3, green: 0.4, blue: 0.5)
            }
            let matches = max(1, 42 - index)
            let isReference = invalidRegisterBatchReferenceFlags && index == 0
            return #"{"ok":true,"reference":\#(isReference),"input":"\#(input.path)","output":"\#(output.path)","matches":\#(matches),"detectedReferenceStars":60,"detectedMovingStars":58,"inlierRatio":0.7,"usedFallback":false}"#
        }
        let referenceFlag = invalidRegisterBatchReferenceFlags == false
        let items = ([#"{"ok":true,"reference":\#(referenceFlag),"input":"\#(reference.path)","output":"\#(referenceOutput.path)","matches":0}"#] + movingItems)
            .joined(separator: ",")
        return success(
            "register-batch",
            output: #"{"type":"complete","command":"register-batch","mode":"\#(alignment.rawValue)","outputFormat":"\#(outputFormat)","frames":\#(inputs.count + 1),"alignedFrames":\#(inputs.count + 1),"failedFrames":0,"outputDirectory":"\#(outputDirectory.path)","items":[\#(items)]}"#
        )
    }

    func stack(inputs: [URL], output: URL, method: StackMethod, alignment: AlignmentMethod) async throws -> ProcessingCommandResult {
        lastStackInputs = inputs
        if let stackProgressToEmit {
            let progress = ProcessingProgressContext.progressScope?.map(stackProgressToEmit) ?? stackProgressToEmit
            NotificationCenter.default.post(
                name: .photonStackProcessingProgress,
                object: ProcessingProgressEvent(
                    jobID: ProcessingProgressContext.jobID,
                    task: "stack",
                    command: "stack",
                    stage: "combine",
                    progress: progress
                )
            )
        }
        if stackDelayNanoseconds > 0 {
            try await Task.sleep(nanoseconds: stackDelayNanoseconds)
        }
        try? writeSyntheticImage(to: output)
        return success("stack")
    }

    func mosaic(
        inputs: [URL],
        output: URL,
        overlapPixels: Int,
        projection: MosaicProjection,
        layout: MosaicLayoutMode,
        alignment: MosaicAlignmentMode,
        blendMode: MosaicBlendMode,
        exposureMatching: Bool,
        columns: Int,
        previewWidth: Int?
    ) async throws -> ProcessingCommandResult {
        mosaicInputGroups.append(inputs)
        if let firstInput = inputs.first {
            materializeCopyIfPossible(input: firstInput, output: output)
        }
        if let mosaicOutputOverride {
            return success("mosaic", output: mosaicOutputOverride)
        }
        let placements = inputs.indices.map { index in
            if index == 0 {
                return #"{"index":0,"x":0,"y":2,"a":1,"b":0,"c":0,"d":1,"dx":0,"dy":0,"model":"manual","matches":0,"references":0,"autoAligned":false,"fallback":false,"reducedModel":false,"coarseAlignment":false}"#
            }
            let x = 72 * index
            return #"{"index":\#(index),"x":\#(x),"y":0,"a":1.009555,"b":-0.02998,"c":0.02998,"d":1.009555,"dx":\#(x),"dy":-2,"model":"affine","matches":24,"references":1,"autoAligned":true,"fallback":false,"reducedModel":false,"coarseAlignment":true}"#
        }.joined(separator: ",")
        return success(
            "mosaic",
            output: #"""
            sysctlbyname for kern.hv_vmm_present failed
            {"type":"complete","command":"mosaic","autoAligned":true,"matches":24,"fallbackPanels":0,"exposureMatched":true,"blend":"multiband","placements":[\#(placements)]}
            postflight diagnostic after mosaic
            """#
        )
    }

    func makePreview(input: URL, output: URL, width: Int, rawOptions: RawProcessingOptions) async throws -> ProcessingCommandResult {
        _ = rawOptions
        makePreviewCallCount += 1
        materializeCopyIfPossible(input: input, output: output)
        if let previewOutputOverride {
            return success("preview", output: previewOutputOverride)
        }
        return success("preview")
    }

    func makeCurvePreview(
        input: URL,
        output: URL,
        width: Int,
        rawOptions: RawProcessingOptions,
        points: [(Double, Double)],
        channel: CurveChannel
    ) async throws -> ProcessingCommandResult {
        _ = (rawOptions, points, channel)
        makeCurvePreviewCallCount += 1
        lastCurvePreviewWidth = width
        materializeCopyIfPossible(input: input, output: output)
        if let previewOutputOverride {
            return success("preview", output: previewOutputOverride)
        }
        return success("preview")
    }

    func adjustRawPreview(input: URL, output: URL, rawOptions: RawProcessingOptions) async throws -> ProcessingCommandResult {
        rawAdjustCallCount += 1
        self.rawAdjustOptions.append(rawOptions)
        lastRawAdjustOptions = rawOptions
        if rawAdjustDelayNanoseconds > 0 {
            if ignoreRawAdjustCancellation {
                let delay = rawAdjustDelayNanoseconds
                _ = try? await Task.detached {
                    try await Task.sleep(nanoseconds: delay)
                }.value
            } else {
                try await Task.sleep(nanoseconds: rawAdjustDelayNanoseconds)
            }
        }
        rawAdjustCompletionCount += 1
        return success("raw adjust")
    }

    func histogram(input: URL, bins: Int) async throws -> ProcessingCommandResult {
        _ = input
        histogramCallCount += 1
        let callIndex = histogramCallCount
        if histogramDelayNanoseconds > 0 {
            if ignoreHistogramCancellation {
                let delay = histogramDelayNanoseconds
                _ = try? await Task.detached {
                    try await Task.sleep(nanoseconds: delay)
                }.value
            } else {
                try await Task.sleep(nanoseconds: histogramDelayNanoseconds)
            }
        }
        if failingHistogramCallIndices.contains(callIndex) {
            throw ProcessingServiceError.invalidReport(command: "histogram")
        }
        if let histogramOutputOverride {
            return success("histogram", output: histogramOutputOverride)
        }
        let binValues = ([String(callIndex)] + Array(repeating: "2", count: max(0, bins - 1)))
            .joined(separator: ",")
        return success(
            "histogram",
            output: #"""
            sysctlbyname for kern.hv_vmm_present failed
            {"type":"complete","command":"histogram","bins":[\#(binValues)],"minimum":0,"maximum":1,"mean":0.5}
            postflight diagnostic after histogram
            """#
        )
    }

    func background(
        input: URL,
        output: URL,
        model: String,
        mode: String,
        strength: Double,
        preserveBrightness: Bool,
        protectBrightTargets: Bool
    ) async throws -> ProcessingCommandResult {
        lastBackgroundStrength = strength
        lastBackgroundPreserveBrightness = preserveBrightness
        lastBackgroundProtectBrightTargets = protectBrightTargets
        materializeCopyIfPossible(input: input, output: output)
        return success("background")
    }

    func detectClouds(input: URL) async throws -> ProcessingCommandResult {
        _ = input
        cloudDetectCallCount += 1
        if let cloudDetectOutputOverride {
            return success("clouds detect", output: cloudDetectOutputOverride)
        }
        return success(
            "clouds detect",
            output: #"{"type":"complete","command":"clouds detect","clouds":2,"removedClouds":0,"items":[{"confidence":0.9,"x":10,"y":20,"width":120,"height":80,"coverage":0.12,"meanLuminance":0.4,"backgroundLuminance":0.18,"mask":[{"x":10,"y":20,"width":40,"height":40},{"x":90,"y":60,"width":40,"height":40}]},{"confidence":0.7,"x":140,"y":30,"width":90,"height":70,"coverage":0.08,"meanLuminance":0.35,"backgroundLuminance":0.18,"mask":[]}]}"#
        )
    }

    func removeClouds(input: URL, output: URL, selectedIndices: [Int]?, strength: Double) async throws -> ProcessingCommandResult {
        lastCloudSelectedIndices = selectedIndices
        lastCloudRemovalStrength = strength
        if failCloudRemoval {
            throw ProcessingServiceError.commandFailed(exitCode: 1, stderr: "cloud removal failed")
        }
        materializeCopyIfPossible(input: input, output: output)
        return success(
            "clouds remove",
            output: #"{"type":"complete","command":"clouds remove","clouds":2,"removedClouds":1,"items":[{"confidence":0.9,"x":10,"y":20,"width":120,"height":80,"coverage":0.12,"meanLuminance":0.4,"backgroundLuminance":0.18},{"confidence":0.7,"x":140,"y":30,"width":90,"height":70,"coverage":0.08,"meanLuminance":0.35,"backgroundLuminance":0.18}]}"#
        )
    }

    func normalize(input: URL, output: URL, targetBackground: Double, targetScale: Double) async throws -> ProcessingCommandResult {
        materializeCopyIfPossible(input: input, output: output)
        return success("normalize")
    }

    func curves(input: URL, output: URL, points: [(Double, Double)], channel: CurveChannel) async throws -> ProcessingCommandResult {
        _ = channel
        return success("curves")
    }

    func autoStretch(input: URL, output: URL, targetBackground: Double) async throws -> ProcessingCommandResult {
        autoStretchCallCount += 1
        autoStretchInputs.append(input)
        autoStretchOutputs.append(output)
        autoStretchTargets.append(targetBackground)
        lastProgressJobID = ProcessingProgressContext.jobID
        if let autoStretchProgressToEmit {
            NotificationCenter.default.post(
                name: .photonStackProcessingProgress,
                object: ProcessingProgressEvent(
                    jobID: lastProgressJobID,
                    task: "stretch",
                    command: "stretch",
                    stage: "work",
                    progress: autoStretchProgressToEmit
                )
            )
        }
        let delay = autoStretchCallCount > 1
            ? subsequentAutoStretchDelayNanoseconds ?? autoStretchDelayNanoseconds
            : autoStretchDelayNanoseconds
        if delay > 0 {
            if ignoreAutoStretchCancellation {
                _ = try? await Task.detached {
                    try await Task.sleep(nanoseconds: delay)
                }.value
            } else {
                try await Task.sleep(nanoseconds: delay)
            }
        }
        materializeCopyIfPossible(input: input, output: output)
        return success("stretch")
    }

    func localContrast(input: URL, output: URL, amount: Double, radius: Int) async throws -> ProcessingCommandResult {
        success("local-contrast")
    }

    func denoise(input: URL, output: URL, amount: Double, chromaAmount: Double, radius: Int) async throws -> ProcessingCommandResult {
        lastDenoiseInput = input
        materializeCopyIfPossible(input: input, output: output)
        return success("denoise")
    }

    func sharpen(input: URL, output: URL, amount: Double, radius: Int) async throws -> ProcessingCommandResult {
        success("sharpen")
    }

    func deconvolve(input: URL, output: URL, iterations: Int, radius: Int, sigma: Double) async throws -> ProcessingCommandResult {
        success("deconvolve")
    }

    func colorNeutralize(input: URL, output: URL, strength: Double) async throws -> ProcessingCommandResult {
        success("color neutralize")
    }

    func colorSaturate(input: URL, output: URL, amount: Double) async throws -> ProcessingCommandResult {
        success("color saturate")
    }

    func removeGreenCast(input: URL, output: URL, amount: Double, backgroundLimit: Double) async throws -> ProcessingCommandResult {
        lastRemoveGreenAmount = amount
        lastRemoveGreenBackgroundLimit = backgroundLimit
        return success("color remove-green")
    }

    func detectStars(input: URL, sigmaThreshold: Double, minPeak: Double, maxStars: Int) async throws -> ProcessingCommandResult {
        _ = input
        starDetectCallCount += 1
        lastStarDetectionThreshold = sigmaThreshold
        lastStarMinimumPeak = minPeak
        lastStarMaximumCount = maxStars
        if let starDetectOutputOverride {
            return success("stars detect", output: starDetectOutputOverride)
        }
        return success(
            "stars detect",
            output: #"{"type":"complete","command":"stars detect","count":42,"stars":[]}"#
        )
    }

    func createStarMask(
        input: URL,
        output: URL,
        radius: Int,
        largeRadius: Int,
        layered: Bool,
        sigmaThreshold: Double,
        minPeak: Double,
        maxStars: Int
    ) async throws -> ProcessingCommandResult {
        starMaskCallCount += 1
        starMaskInputs.append(input)
        starMaskOutputs.append(output)
        lastStarMaskRadius = radius
        lastLargeStarMaskRadius = largeRadius
        lastLayeredStarMask = layered
        lastStarMaskMaximumCount = maxStars
        lastStarDetectionThreshold = sigmaThreshold
        lastStarMinimumPeak = minPeak
        materializeCopyIfPossible(input: input, output: output)
        if let starMaskOutputOverride {
            return success("stars mask", output: starMaskOutputOverride)
        }
        return success(
            "stars mask",
            output: #"{"type":"complete","command":"stars mask","stars":42,"largeStars":5}"#
        )
    }

    func reduceStars(input: URL, output: URL, amount: Double, profileAware: Bool, edgeAware: Bool) async throws -> ProcessingCommandResult {
        _ = edgeAware
        return success("stars reduce")
    }

    func reduceComa(input: URL, output: URL, amount: Double, radius: Int, eccentricity: Double, edgeAware: Bool) async throws -> ProcessingCommandResult {
        success("stars coma")
    }

    func detectArtifacts(input: URL) async throws -> ProcessingCommandResult {
        artifactDetectCallCount += 1
        if artifactDetectDelayNanoseconds > 0 {
            try await Task.sleep(nanoseconds: artifactDetectDelayNanoseconds)
        }
        if let artifactDetectOutputOverride {
            return success("artifacts detect", output: artifactDetectOutputOverride)
        }
        return success(
            "artifacts detect",
            output: """
            sysctlbyname for kern.hv_vmm_present failed
            {"type":"complete","command":"artifacts detect","trails":2,"removedTrails":0,"protectedMeteors":0,"items":[{"kind":"airplane","confidence":0.91,"x1":0,"y1":1,"x2":10,"y2":1,"length":10,"width":2,"meanBrightness":0.04,"weight":0.80,"peakPosition":0.5,"taperScore":0.1},{"kind":"drone","confidence":0.72,"x1":5,"y1":5,"x2":12,"y2":8,"length":8,"width":2,"meanBrightness":0.03,"weight":0.40,"peakPosition":0.6,"taperScore":0.2}]}
            """
        )
    }

    func removeArtifacts(
        input: URL,
        output: URL,
        selectedIndices: [Int]?,
        removeAirplanes: Bool,
        removeDrones: Bool,
        removeSatellites: Bool,
        removeMeteors: Bool
    ) async throws -> ProcessingCommandResult {
        _ = (removeAirplanes, removeDrones)
        lastArtifactSelectedIndices = selectedIndices
        lastArtifactRemoveSatellites = removeSatellites
        lastArtifactRemoveMeteors = removeMeteors
        if failArtifactRemoval {
            throw ProcessingServiceError.commandFailed(exitCode: 1, stderr: "artifact removal failed")
        }
        materializeCopyIfPossible(input: input, output: output)
        return success(
            "artifacts remove",
            output: #"{"type":"complete","command":"artifacts remove","trails":2,"removedTrails":1,"protectedMeteors":0,"items":[{"kind":"airplane","confidence":0.91,"x1":0,"y1":1,"x2":10,"y2":1,"length":10,"width":2,"peakPosition":0.5,"taperScore":0.1},{"kind":"drone","confidence":0.72,"x1":5,"y1":5,"x2":12,"y2":8,"length":8,"width":2,"peakPosition":0.6,"taperScore":0.2}]}"#
        )
    }

    func cleanArtifactSequence(inputs: [URL], outputDirectory: URL, outputFormat: String, minWeight: Double, recurrenceThreshold: Int) async throws -> ProcessingCommandResult {
        _ = (minWeight, recurrenceThreshold)
        lastCleanSequenceInputs = inputs
        lastCleanSequenceOutputDirectory = outputDirectory
        lastCleanSequenceOutputFormat = outputFormat
        if let cleanSequenceOutputOverride {
            return success("artifacts clean-sequence", output: cleanSequenceOutputOverride)
        }
        try? FileManager.default.createDirectory(at: outputDirectory, withIntermediateDirectories: true)
        let items = inputs.enumerated().map { index, input in
            let outputExtension = cleanSequenceOutputExtensionOverride ?? outputFormat
            let output = outputDirectory.appendingPathComponent("clean-\(index + 1).\(outputExtension)")
            if omitLastCleanSequenceOutputFile == false || index != inputs.indices.last {
                try? writeSolidPNG(to: output, red: 0.4, green: 0.5, blue: 0.6, width: 2, height: 2)
            }
            return #"{"ok":true,"input":"\#(input.path)","output":"\#(output.path)","trails":2,"removedTrails":1,"protectedMeteors":0,"recurrentTrails":0,"selectedIndices":[0],"message":"Cleaned transient trails"}"#
        }.joined(separator: ",")
        return success(
            "artifacts clean-sequence",
            output: #"{"type":"complete","command":"artifacts clean-sequence","frames":\#(inputs.count),"cleanedFrames":\#(inputs.count),"failedFrames":0,"trails":\#(inputs.count * 2),"selectedTrails":\#(inputs.count),"removedTrails":\#(inputs.count),"recurrentTrails":0,"outputFormat":"\#(outputFormat)","outputDirectory":"\#(outputDirectory.path)","items":[\#(items)]}"#
        )
    }

    func extractMeteors(input: URL, output: URL) async throws -> ProcessingCommandResult {
        _ = (input, output)
        return success("meteors extract", output: #"{"type":"complete","command":"meteors extract","meteors":1,"restoredMeteors":1,"items":[]}"#)
    }

    func restoreMeteors(base: URL, source: URL, output: URL) async throws -> ProcessingCommandResult {
        _ = (base, output)
        restoreMeteorCallCount += 1
        restoreMeteorSources.append(source)
        return success("meteors restore", output: #"{"type":"complete","command":"meteors restore","meteors":1,"restoredMeteors":1,"items":[]}"#)
    }

    func drizzle(inputs: [URL], output: URL, scale: Int, pixfrac: Double, alignment: AlignmentMethod) async throws -> ProcessingCommandResult {
        _ = output
        drizzleInputGroups.append(inputs)
        lastDrizzleScale = scale
        lastDrizzlePixfrac = pixfrac
        lastDrizzleAlignment = alignment
        return success("drizzle")
    }

    func runWorkflow(_ workflow: URL) async throws -> ProcessingCommandResult {
        success("run")
    }

    func runCLI(arguments: [String]) async throws -> ProcessingCommandResult {
        lastCLIArguments = arguments
        return success("manual cli", output: #"{"type":"complete","command":"manual cli"}"#)
    }

    private func success(_ command: String, output: String = "{}") -> ProcessingCommandResult {
        ProcessingCommandResult(command: [command], exitCode: 0, standardOutput: output, standardError: "")
    }

    private func materializeCopyIfPossible(input: URL, output: URL) {
        try? FileManager.default.createDirectory(
            at: output.deletingLastPathComponent(),
            withIntermediateDirectories: true
        )
        if ["fit", "fits", "fts"].contains(output.pathExtension.lowercased()) {
            try? writeMinimalFITS(to: output)
            return
        }
        if ["fit", "fits", "fts"].contains(input.pathExtension.lowercased()) {
            try? writeSolidPNG(to: output, red: 0.25, green: 0.35, blue: 0.45, width: 2, height: 2)
            return
        }
        guard FileManager.default.fileExists(atPath: input.path),
              let data = try? Data(contentsOf: input)
        else {
            return
        }
        try? data.write(to: output, options: .atomic)
    }

    struct Snapshot: Equatable {
        var inspectCalls: Int
        var makePreviewCalls: Int
        var makeCurvePreviewCalls: Int
        var curvePreviewWidth: Int?
        var calibrateCalls: Int
        var masterInputGroups: [[URL]]
        var masterMethods: [StackMethod]
        var masterRawOptions: [RawProcessingOptions]
        var calibrateLights: [URL]
        var calibrateDarkInputs: [URL?]
        var calibrateBiasInputs: [URL?]
        var calibrateFlatInputs: [URL?]
        var calibrateDarkBiasStates: [CalibrationBiasState]
        var calibrateFlatBiasStates: [CalibrationBiasState]
        var calibrateRawOptions: [RawProcessingOptions]
        var convertCalls: Int
        var convertInput: URL?
        var convertInputs: [URL]
        var convertOutputs: [URL]
        var convertOptions: [ExportOptions]
        var convertRawOptions: [RawProcessingOptions]
        var stackInputs: [URL]
        var denoiseInput: URL?
        var autoStretchCalls: Int
        var autoStretchInputs: [URL]
        var autoStretchOutputs: [URL]
        var autoStretchTargets: [Double]
        var backgroundStrength: Double?
        var backgroundPreserveBrightness: Bool?
        var backgroundProtectBrightTargets: Bool?
        var removeGreenAmount: Double?
        var removeGreenBackgroundLimit: Double?
        var starDetectCalls: Int
        var starDetectionThreshold: Double?
        var starMinimumPeak: Double?
        var starMaximumCount: Int?
        var starMaskCalls: Int
        var starMaskInputs: [URL]
        var starMaskOutputs: [URL]
        var starMaskRadius: Int?
        var largeStarMaskRadius: Int?
        var layeredStarMask: Bool?
        var starMaskMaximumCount: Int?
        var cloudDetectCalls: Int
        var cloudSelectedIndices: [Int]?
        var cloudRemovalStrength: Double?
        var artifactDetectCalls: Int
        var artifactSelectedIndices: [Int]?
        var artifactRemoveSatellites: Bool?
        var artifactRemoveMeteors: Bool?
        var cleanSequenceInputs: [URL]
        var cleanSequenceOutputDirectory: URL?
        var cleanSequenceOutputFormat: String?
        var restoreMeteorCalls: Int
        var restoreMeteorSources: [URL]
        var registerBatchCalls: Int
        var registerBatchReference: URL?
        var registerBatchInputs: [URL]
        var registerBatchAlignment: AlignmentMethod?
        var registerBatchOutputFormat: String?
        var cliArguments: [String]
        var rawAdjustCalls: Int
        var rawAdjustCompletions: Int
        var rawAdjustOptionHistory: [RawProcessingOptions]
        var rawAdjustOptions: RawProcessingOptions?
        var mosaicInputGroups: [[URL]]
        var drizzleInputGroups: [[URL]]
        var drizzleScale: Int?
        var drizzlePixfrac: Double?
        var drizzleAlignment: AlignmentMethod?
    }
}
