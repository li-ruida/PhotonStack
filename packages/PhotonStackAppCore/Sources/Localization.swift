import Foundation

public enum AppLanguage: String, Codable, CaseIterable, Equatable, Sendable {
    case english
    case simplifiedChinese

    public static var systemDefault: AppLanguage {
        let preferred = Locale.preferredLanguages.first?.lowercased() ?? ""
        return preferred.hasPrefix("zh") ? .simplifiedChinese : .english
    }

    public var displayName: String {
        switch self {
        case .english:
            return "English"
        case .simplifiedChinese:
            return "中文"
        }
    }

    public func text(_ key: LocalizedTextKey) -> String {
        LocalizedTextCatalog.text(key, language: self)
    }

    public func roleName(_ role: CalibrationFrameRole) -> String {
        switch (self, role) {
        case (.english, .light):
            return "Light"
        case (.english, .dark):
            return "Dark"
        case (.english, .bias):
            return "Bias"
        case (.english, .flat):
            return "Flat"
        case (.english, .mosaic):
            return "Mosaic"
        case (.english, .reference):
            return "Reference"
        case (.english, .other):
            return "Other"
        case (.simplifiedChinese, .light):
            return "亮场"
        case (.simplifiedChinese, .dark):
            return "暗场"
        case (.simplifiedChinese, .bias):
            return "偏置"
        case (.simplifiedChinese, .flat):
            return "平场"
        case (.simplifiedChinese, .mosaic):
            return "接片"
        case (.simplifiedChinese, .reference):
            return "参考"
        case (.simplifiedChinese, .other):
            return "其他"
        }
    }

    public func calibrationBiasStateName(_ state: CalibrationBiasState) -> String {
        switch (self, state) {
        case (.english, .included):
            return "Bias included"
        case (.english, .removed):
            return "Bias already removed"
        case (.simplifiedChinese, .included):
            return "包含 Bias"
        case (.simplifiedChinese, .removed):
            return "已移除 Bias"
        }
    }

    public func stackMethodName(_ method: StackMethod) -> String {
        switch (self, method) {
        case (.english, .average):
            return "Average"
        case (.english, .weighted):
            return "Weighted"
        case (.english, .median):
            return "Median"
        case (.english, .sigma):
            return "Sigma"
        case (.english, .winsorized):
            return "Winsorized"
        case (.english, .percentile):
            return "Percentile"
        case (.simplifiedChinese, .average):
            return "平均"
        case (.simplifiedChinese, .weighted):
            return "加权"
        case (.simplifiedChinese, .median):
            return "中位数"
        case (.simplifiedChinese, .sigma):
            return "Sigma 剔除"
        case (.simplifiedChinese, .winsorized):
            return "Winsorized"
        case (.simplifiedChinese, .percentile):
            return "百分位"
        }
    }

    public func alignmentMethodName(_ method: AlignmentMethod) -> String {
        switch (self, method) {
        case (.english, .none):
            return "None"
        case (.english, .translation):
            return "Translation"
        case (.english, .similarity):
            return "Similarity"
        case (.english, .affine):
            return "Affine"
        case (.english, .distortion):
            return "Distortion"
        case (.simplifiedChinese, .none):
            return "无"
        case (.simplifiedChinese, .translation):
            return "平移"
        case (.simplifiedChinese, .similarity):
            return "相似变换"
        case (.simplifiedChinese, .affine):
            return "仿射"
        case (.simplifiedChinese, .distortion):
            return "畸变分区"
        }
    }

    public func projectTemplateName(_ template: ProjectTemplate) -> String {
        switch (self, template) {
        case (.english, .deepSky):
            return "Deep Sky"
        case (.english, .lunarPlanetary):
            return "Lunar / Planetary"
        case (.english, .mosaic):
            return "Mosaic"
        case (.english, .singleFrame):
            return "Single Frame"
        case (.simplifiedChinese, .deepSky):
            return "深空"
        case (.simplifiedChinese, .lunarPlanetary):
            return "月面 / 行星"
        case (.simplifiedChinese, .mosaic):
            return "接片"
        case (.simplifiedChinese, .singleFrame):
            return "单张"
        }
    }

    public func batchOutputFormatName(_ format: BatchOutputFormat) -> String {
        switch (self, format) {
        case (.english, .png), (.simplifiedChinese, .png):
            return "PNG"
        case (.english, .tiff), (.simplifiedChinese, .tiff):
            return "TIFF"
        case (.english, .jpeg), (.simplifiedChinese, .jpeg):
            return "JPEG"
        }
    }

    public func exportBitDepthName(_ bitDepth: ExportBitDepth) -> String {
        switch (self, bitDepth) {
        case (.english, .automatic):
            return "Auto"
        case (.english, .eight):
            return "8-bit"
        case (.english, .sixteen):
            return "16-bit"
        case (.simplifiedChinese, .automatic):
            return "自动"
        case (.simplifiedChinese, .eight):
            return "8-bit"
        case (.simplifiedChinese, .sixteen):
            return "16-bit"
        }
    }

    public func exportColorSpaceName(_ colorSpace: ExportColorSpace) -> String {
        switch (self, colorSpace) {
        case (.english, .srgb):
            return "sRGB"
        case (.english, .linearSRGB):
            return "Linear sRGB"
        case (.simplifiedChinese, .srgb):
            return "sRGB"
        case (.simplifiedChinese, .linearSRGB):
            return "线性 sRGB"
        }
    }

    public func fitsValueModeName(_ mode: FITSValueMode) -> String {
        switch (self, mode) {
        case (.english, .display):
            return "Display normalized"
        case (.english, .scientific):
            return "Scientific values"
        case (.simplifiedChinese, .display):
            return "显示归一化"
        case (.simplifiedChinese, .scientific):
            return "保留科学值"
        }
    }

    public func artifactKindName(_ kind: ProcessingArtifactKind) -> String {
        switch (self, kind) {
        case (.english, .rawSequence):
            return "Source Sequence"
        case (.english, .decodedSequence):
            return "Decoded Sequence"
        case (.english, .calibratedSequence):
            return "Calibrated Sequence"
        case (.english, .registeredSequence):
            return "Registered Sequence"
        case (.english, .cleanedTimelapseSequence):
            return "Cleaned Timelapse Sequence"
        case (.english, .stackMaster):
            return "Stack Master"
        case (.english, .editedImage):
            return "Edited Image"
        case (.english, .mask):
            return "Mask"
        case (.english, .meteorLayer):
            return "Meteor Layer"
        case (.english, .starLayer):
            return "Star Layer"
        case (.simplifiedChinese, .rawSequence):
            return "源素材序列"
        case (.simplifiedChinese, .decodedSequence):
            return "解码序列"
        case (.simplifiedChinese, .calibratedSequence):
            return "校准序列"
        case (.simplifiedChinese, .registeredSequence):
            return "已对齐序列"
        case (.simplifiedChinese, .cleanedTimelapseSequence):
            return "去航迹延时序列"
        case (.simplifiedChinese, .stackMaster):
            return "堆栈主图"
        case (.simplifiedChinese, .editedImage):
            return "编辑结果"
        case (.simplifiedChinese, .mask):
            return "蒙版"
        case (.simplifiedChinese, .meteorLayer):
            return "流星层"
        case (.simplifiedChinese, .starLayer):
            return "星点层"
        }
    }

    public func layerKindName(_ kind: ProcessingLayerKind) -> String {
        switch (self, kind) {
        case (.english, .baseImage):
            return "Base Image"
        case (.english, .adjustment):
            return "Adjustment"
        case (.english, .mask):
            return "Mask"
        case (.english, .stars):
            return "Stars"
        case (.english, .meteors):
            return "Meteors"
        case (.english, .background):
            return "Background"
        case (.simplifiedChinese, .baseImage):
            return "基础图层"
        case (.simplifiedChinese, .adjustment):
            return "调整层"
        case (.simplifiedChinese, .mask):
            return "蒙版"
        case (.simplifiedChinese, .stars):
            return "星点层"
        case (.simplifiedChinese, .meteors):
            return "流星层"
        case (.simplifiedChinese, .background):
            return "背景层"
        }
    }

    public func layerBlendModeName(_ mode: ProcessingLayerBlendMode) -> String {
        switch (self, mode) {
        case (.english, .normal):
            return "Normal"
        case (.english, .screen):
            return "Screen"
        case (.english, .lighten):
            return "Lighten"
        case (.english, .multiply):
            return "Multiply"
        case (.english, .overlay):
            return "Overlay"
        case (.simplifiedChinese, .normal):
            return "正常"
        case (.simplifiedChinese, .screen):
            return "滤色"
        case (.simplifiedChinese, .lighten):
            return "变亮"
        case (.simplifiedChinese, .multiply):
            return "正片叠底"
        case (.simplifiedChinese, .overlay):
            return "叠加"
        }
    }

    public func rawWhiteBalanceModeName(_ mode: RawWhiteBalanceMode) -> String {
        switch (self, mode) {
        case (.english, .camera):
            return "Camera"
        case (.english, .auto):
            return "Auto"
        case (.english, .daylight):
            return "Daylight"
        case (.english, .manual):
            return "Manual"
        case (.simplifiedChinese, .camera):
            return "相机"
        case (.simplifiedChinese, .auto):
            return "自动"
        case (.simplifiedChinese, .daylight):
            return "日光"
        case (.simplifiedChinese, .manual):
            return "手动"
        }
    }

    public func rawBlackLevelModeName(_ mode: RawBlackLevelMode) -> String {
        switch (self, mode) {
        case (.english, .camera):
            return "Camera"
        case (.english, .auto):
            return "Auto"
        case (.english, .manual):
            return "Manual"
        case (.simplifiedChinese, .camera):
            return "相机"
        case (.simplifiedChinese, .auto):
            return "自动"
        case (.simplifiedChinese, .manual):
            return "手动"
        }
    }

    public func rawDemosaicQualityName(_ quality: RawDemosaicQuality) -> String {
        switch (self, quality) {
        case (.english, .fast):
            return "Fast draft"
        case (.english, .balanced):
            return "Balanced (legacy)"
        case (.english, .high):
            return "Full quality"
        case (.simplifiedChinese, .fast):
            return "快速草稿"
        case (.simplifiedChinese, .balanced):
            return "均衡（旧版）"
        case (.simplifiedChinese, .high):
            return "完整质量"
        }
    }

    public func mosaicProjectionName(_ projection: MosaicProjection) -> String {
        switch (self, projection) {
        case (.english, .planar):
            return "Planar"
        case (.english, .cylindrical):
            return "Cylindrical"
        case (.simplifiedChinese, .planar):
            return "平面"
        case (.simplifiedChinese, .cylindrical):
            return "圆柱"
        }
    }

    public func mosaicLayoutName(_ layout: MosaicLayoutMode) -> String {
        switch (self, layout) {
        case (.english, .horizontal):
            return "Horizontal"
        case (.english, .grid):
            return "Grid"
        case (.simplifiedChinese, .horizontal):
            return "横向"
        case (.simplifiedChinese, .grid):
            return "网格"
        }
    }

    public func mosaicAlignmentName(_ alignment: MosaicAlignmentMode) -> String {
        switch (self, alignment) {
        case (.english, .manual):
            return "Manual"
        case (.english, .auto):
            return "Auto affine"
        case (.simplifiedChinese, .manual):
            return "手动"
        case (.simplifiedChinese, .auto):
            return "自动仿射"
        }
    }

    public func mosaicBlendModeName(_ blendMode: MosaicBlendMode) -> String {
        switch (self, blendMode) {
        case (.english, .average):
            return "Average"
        case (.english, .feather):
            return "Feather"
        case (.english, .multiband):
            return "Multiband"
        case (.simplifiedChinese, .average):
            return "平均"
        case (.simplifiedChinese, .feather):
            return "羽化"
        case (.simplifiedChinese, .multiband):
            return "多频段"
        }
    }

    public func curvePresetName(_ preset: CurvePreset) -> String {
        switch (self, preset) {
        case (.english, .custom):
            return "Custom"
        case (.english, .linear):
            return "Linear"
        case (.english, .softStretch):
            return "Soft Stretch"
        case (.english, .deepSkyContrast):
            return "Deep Sky Contrast"
        case (.english, .shadowLift):
            return "Shadow Lift"
        case (.english, .highlightProtect):
            return "Highlight Protect"
        case (.simplifiedChinese, .custom):
            return "自定义"
        case (.simplifiedChinese, .linear):
            return "线性"
        case (.simplifiedChinese, .softStretch):
            return "柔和拉伸"
        case (.simplifiedChinese, .deepSkyContrast):
            return "深空对比"
        case (.simplifiedChinese, .shadowLift):
            return "暗部提升"
        case (.simplifiedChinese, .highlightProtect):
            return "高光保护"
        }
    }

    public func curveChannelName(_ channel: CurveChannel) -> String {
        switch (self, channel) {
        case (.english, .rgb):
            return "RGB"
        case (.english, .red):
            return "Red"
        case (.english, .green):
            return "Green"
        case (.english, .blue):
            return "Blue"
        case (.english, .luminance):
            return "Luminance"
        case (.simplifiedChinese, .rgb):
            return "RGB"
        case (.simplifiedChinese, .red):
            return "红色"
        case (.simplifiedChinese, .green):
            return "绿色"
        case (.simplifiedChinese, .blue):
            return "蓝色"
        case (.simplifiedChinese, .luminance):
            return "亮度"
        }
    }
}

public enum LocalizedTextKey: String, CaseIterable, Codable, Sendable {
    case importAction
    case importFilesAction
    case importFolderAction
    case importFilesMessage
    case importFolderMessage
    case assetImportQueued
    case assetsImported
    case openProjectMessage
    case saveProjectMessage
    case exportImageMessage
    case importCurvePresetsMessage
    case exportCurvePresetsMessage
    case chooseBatchOutputDirectoryMessage
    case openAction
    case saveAction
    case unsavedChangesTitle
    case unsavedChangesMessage
    case saveChangesAction
    case discardChangesAction
    case unsavedChangesStatus
    case processingInterruptionTitle
    case processingInterruptionCloseMessage
    case processingInterruptionQuitMessage
    case stopProcessingAndCloseAction
    case stopProcessingAndQuitAction
    case inspectAction
    case previewAction
    case sourcePreviewAction
    case currentImageLabel
    case stretchAction
    case contrastAction
    case denoiseAction
    case sharpenAction
    case starsAction
    case comaAction
    case backAction
    case resetAction
    case cancelAction
    case exportAction
    case exportSettingsSection
    case exportBitDepth
    case exportColorSpace
    case fitsValueMode
    case fitsValueModeHelp
    case jpegQuality
    case exportSidecarFailed
    case noImage
    case assetSection
    case removeAssetAction
    case removeAssetHelp
    case removeAssetConfirmationTitle
    case removeAssetsConfirmationTitle
    case removeAssetConfirmationMessage
    case removeAssetsConfirmationMessage
    case removeArtifactAction
    case removeArtifactHelp
    case removeArtifactConfirmationTitle
    case removeArtifactConfirmationMessage
    case relinkAssetAction
    case relinkAssetHelp
    case relinkAssetMessage
    case relinkAssetSuccess
    case relinkAssetInvalid
    case relinkAssetDuplicate
    case missingAssetStatus
    case missingAssetExportError
    case missingAssetProcessingError
    case missingWorkspaceSourceError
    case imageProcessingRequiresImageLayer
    case emptyAssetsTitle
    case emptyAssetsMessage
    case fitToWindow
    case actualSize
    case zoomIn
    case zoomOut
    case originalPreview
    case markedPreview
    case artifactMarkedPreview
    case markedPreviewRenderFailed
    case nameLabel
    case kindLabel
    case pathLabel
    case roleLabel
    case noAssetSelected
    case workflowSection
    case processMenu
    case masterLabel
    case darkBiasStateLabel
    case flatBiasStateLabel
    case calibrationBiasStateHelp
    case flatBiasMissingWarning
    case stackLabel
    case alignLabel
    case restoreMeteorsAfterStack
    case runStackWorkflow
    case exportProductManifest
    case registrationSection
    case referenceFrame
    case movingFrame
    case registerFrames
    case registerBatchFrames
    case batchRegistrationFrames
    case selectAllFrames
    case clearFrameSelection
    case onlyCurrentFrame
    case registrationNeedsTwoFrames
    case registrationNoFramesSelected
    case registrationReport
    case registrationMatches
    case registrationOffset
    case registrationScale
    case registrationRotation
    case parametersSection
    case advancedProcessingSection
    case backgroundModel
    case backgroundMode
    case backgroundStrength
    case preserveBackgroundBrightness
    case preserveBackgroundBrightnessHelp
    case protectBrightBackgroundTargets
    case protectBrightBackgroundTargetsHelp
    case globalModel
    case gridModel
    case subtractMode
    case divideMode
    case applyBackground
    case cloudsSection
    case detectClouds
    case removeSelectedClouds
    case cloudRemovalStrength
    case cloudSummary
    case topCloudRegions
    case selectAllClouds
    case clearCloudSelection
    case selectedCloudPreviewHint
    case cloudTop20Hint
    case normalizeAction
    case targetBackground
    case targetScale
    case colorSection
    case neutralizeAction
    case saturationAction
    case neutralizeStrength
    case saturationAmount
    case starToolsSection
    case detectStarsAction
    case createStarMaskAction
    case starDetectionThreshold
    case starMinimumPeak
    case starMaskRadius
    case largeStarMaskRadius
    case layeredStarMask
    case detectedStarsLabel
    case deconvolutionSection
    case deconvolveAction
    case deconvolutionIterations
    case deconvolutionRadius
    case deconvolutionSigma
    case drizzleSection
    case drizzleAction
    case drizzleScale
    case drizzlePixfrac
    case drizzleAlign
    case artifactTrailsSection
    case detectArtifactTrails
    case removeArtificialTrails
    case cleanTimelapseTrails
    case extractMeteorLayer
    case restoreMeteorsAction
    case removeSelectedTrails
    case allowMeteorRemoval
    case artifactTrailSummary
    case protectedMeteors
    case topArtifactTrails
    case artifactTrailWeight
    case artifactTrailTop20Hint
    case selectAllArtificialTrails
    case clearArtifactSelection
    case selectedArtifactPreviewHint
    case airplaneTrail
    case droneTrail
    case satelliteTrail
    case meteorTrail
    case rawPipelineSection
    case rawProfessionalNote
    case rawMetadataSection
    case cameraLabel
    case lensLabel
    case captureDateLabel
    case exposureLabel
    case exposureBiasLabel
    case fNumberLabel
    case isoLabel
    case focalLengthLabel
    case colorProfileLabel
    case rawDecoderLabel
    case rawDecoderAppleActive
    case rawDecoderImageIOFallback
    case rawWhiteBalance
    case rawTemperature
    case rawTint
    case rawExposureBias
    case rawBlackLevel
    case rawManualBlackLevel
    case rawDemosaicQuality
    case rawLinearOutput
    case rawAutoPreview
    case rawAppliedParameters
    case rawDeferredParameters
    case previewWidth
    case stretchParameter
    case starReduction
    case profileAwareStars
    case edgeAwareStars
    case comaReduction
    case comaRadius
    case comaEccentricity
    case edgeAwareComa
    case localContrast
    case contrastRadius
    case denoiseParameter
    case chromaDenoiseParameter
    case denoiseRadius
    case sharpenParameter
    case sharpenRadius
    case tasksSection
    case noTasksYet
    case processingStatus
    case consoleStatus
    case readyStatus
    case consoleInputPlaceholder
    case consoleRunCommand
    case consolePopOut
    case consoleExpand
    case consoleCollapse
    case consoleZoomIn
    case consoleZoomOut
    case consoleInputEmpty
    case consoleInputUnclosedQuote
    case languageLabel
    case savedProject
    case openedProject
    case cancellingTask
    case cancelledTask
    case restoredPreview
    case workflowNoLights
    case toolsPanel
    case expandAllTools
    case collapseAllTools
    case inspectorProjectGroup
    case inspectorAssetGroup
    case inspectorPhotoInfoGroup
    case inspectorWorkspaceGroup
    case inspectorWorkflowGroup
    case inspectorAdjustmentsGroup
    case inspectorBatchGroup
    case inspectorHistoryGroup
    case inspectorProjectGroupIntro
    case inspectorAssetGroupIntro
    case inspectorPhotoInfoGroupIntro
    case inspectorWorkspaceGroupIntro
    case inspectorWorkflowGroupIntro
    case inspectorAdjustmentsGroupIntro
    case inspectorBatchGroupIntro
    case inspectorHistoryGroupIntro
    case photoInfoSection
    case imageSizeLabel
    case formatLabel
    case channelsLabel
    case bitDepthLabel
    case orientationLabel
    case metadataUnavailable
    case projectSection
    case templateLabel
    case newProject
    case projectName
    case createProject
    case createAndImport
    case autosaveLabel
    case recentProjects
    case noRecentProjects
    case editGraphSection
    case noEditOperations
    case undoEditGraph
    case redoEditGraph
    case replayEditGraph
    case editGraphNeedsReplay
    case disabledOperationLayer
    case editGraphInvalidRecordedAssets
    case editGraphAmbiguousProvenance
    case editGraphMissingRecordedAssets
    case editGraphMissingRecordedArtifact
    case editGraphUnsupportedOperation
    case invalidEditOperationParameter
    case moveOperationUp
    case moveOperationDown
    case deleteOperation
    case productsSection
    case workspaceAssetsTab
    case workspaceObjectsTab
    case workspaceLayersTab
    case noSourceAssets
    case addLayerMenu
    case addLayerFromSource
    case addLayerFromObject
    case activeLayerLabel
    case noActiveLayer
    case artifactsLabel
    case layersLabel
    case noArtifacts
    case noLayers
    case framesLabel
    case sourceLabel
    case previewProduct
    case previewLayerStack
    case blendModeLabel
    case opacityLabel
    case layerMaskLabel
    case noLayerMask
    case detachLayerMask
    case editLayerMask
    case invertLayerMask
    case maskDensity
    case maskFeather
    case maskBrushHide
    case maskBrushReveal
    case maskBrushSize
    case maskBrushOpacity
    case undoMaskStroke
    case resetMaskStrokes
    case maskEditorTitle
    case visibleLabel
    case showLayer
    case hideLayer
    case autosavedProject
    case settingsSaveFailed
    case batchQueueSection
    case queueStretch
    case queueDenoise
    case queueSharpen
    case queueStars
    case queueAllStretch
    case queueAllDenoise
    case queueAllSharpen
    case queueAllStars
    case startQueue
    case pauseQueue
    case resumeQueue
    case retryFailed
    case clearQueue
    case noQueuedTasks
    case batchOutputDirectory
    case batchOutputFormat
    case batchConcurrency
    case chooseOutputDirectory
    case clearOutputDirectory
    case previewCache
    case clearPreviewCache
    case clearPreviewCacheConfirmationTitle
    case clearPreviewCacheConfirmationMessage
    case previewCacheCleared
    case cacheHitLabel
    case executedLabel
    case histogramCurvesSection
    case refreshHistogram
    case applyCurve
    case curveChannel
    case addCurvePoint
    case deleteCurvePoint
    case resetCurvePoints
    case curvePreset
    case customCurvePresets
    case saveCurvePreset
    case importCurvePresets
    case exportCurvePresets
    case curvePresetImportTooLarge
    case curvePresetImportTooMany
    case blackPoint
    case midInput
    case midOutput
    case whitePoint
    case mosaicSection
    case mosaicPanels
    case mosaicEstimatedSize
    case mosaicMissingMetadata
    case mosaicHeightMismatch
    case estimateOverlap
    case mosaicProjection
    case mosaicLayout
    case mosaicAlignment
    case mosaicBlend
    case mosaicExposureMatch
    case mosaicQualityReport
    case mosaicFallbackPanels
    case mosaicColumns
    case mosaicPreviewWidth
    case previewMosaic
    case overlapPixels
    case runMosaic
    case mosaicNoPanels
}

private enum LocalizedTextCatalog {
    private static let english: [LocalizedTextKey: String] = [
        .importAction: "Import",
        .importFilesAction: "Import Files...",
        .importFolderAction: "Import Folder...",
        .importFilesMessage: "Choose one or more RAW, FITS, TIFF, JPEG, PNG, or HEIF files.",
        .importFolderMessage: "Choose a folder. Supported images in its subfolders will also be imported.",
        .assetImportQueued: "%d asset(s) will be imported after the current task finishes.",
        .assetsImported: "Imported %d asset(s).",
        .openProjectMessage: "Choose a PhotonStack project folder.",
        .saveProjectMessage: "Choose where to save the PhotonStack project folder.",
        .exportImageMessage: "Choose a file name and format for the finished image.",
        .importCurvePresetsMessage: "Choose a PhotonStack curve preset library.",
        .exportCurvePresetsMessage: "Choose where to save the PhotonStack curve preset library.",
        .chooseBatchOutputDirectoryMessage: "Choose a folder for completed batch images.",
        .openAction: "Open",
        .saveAction: "Save",
        .unsavedChangesTitle: "Save changes before continuing?",
        .unsavedChangesMessage: "The current project has changes that have not been saved. Save them before replacing or closing it.",
        .saveChangesAction: "Save Changes",
        .discardChangesAction: "Don't Save",
        .unsavedChangesStatus: "Unsaved changes",
        .processingInterruptionTitle: "Processing is still running",
        .processingInterruptionCloseMessage: "Closing this window will stop the current operation and discard its unfinished result.",
        .processingInterruptionQuitMessage: "Quitting PhotonStack will stop the current operation and discard its unfinished result.",
        .stopProcessingAndCloseAction: "Stop and Close",
        .stopProcessingAndQuitAction: "Stop and Quit",
        .inspectAction: "Inspect",
        .previewAction: "Preview",
        .sourcePreviewAction: "Preview Source",
        .currentImageLabel: "Current image",
        .stretchAction: "Stretch",
        .contrastAction: "Contrast",
        .denoiseAction: "Denoise",
        .sharpenAction: "Sharpen",
        .starsAction: "Stars",
        .comaAction: "Coma",
        .backAction: "Back",
        .resetAction: "Reset",
        .cancelAction: "Cancel",
        .exportAction: "Export",
        .exportSettingsSection: "Export Settings",
        .exportBitDepth: "Bit depth",
        .exportColorSpace: "Color space",
        .fitsValueMode: "FITS input values",
        .fitsValueModeHelp: "Scientific values are preserved only in FITS. PNG, JPEG, HEIF, and TIFF exports normalize or quantize the data.",
        .jpegQuality: "JPEG quality",
        .exportSidecarFailed: "The image was exported, but its PhotonStack sidecar could not be saved.",
        .noImage: "No Image",
        .assetSection: "Asset",
        .removeAssetAction: "Remove Asset",
        .removeAssetHelp: "Remove this asset from the project without deleting the source file.",
        .removeAssetConfirmationTitle: "Remove this asset?",
        .removeAssetsConfirmationTitle: "Remove these assets?",
        .removeAssetConfirmationMessage: "The source file will stay on disk. Layers, intermediate products, and queued work that depend on this asset will also be removed from the project.",
        .removeAssetsConfirmationMessage: "The source files will stay on disk. Layers, intermediate products, and queued work that depend on these assets will also be removed from the project.",
        .removeArtifactAction: "Remove Product",
        .removeArtifactHelp: "Remove this intermediate product from the project without deleting its files.",
        .removeArtifactConfirmationTitle: "Remove this intermediate product?",
        .removeArtifactConfirmationMessage: "Files will stay on disk. Dependent intermediate products and layers will also be removed from the project; surviving layers will detach this product as a mask.",
        .relinkAssetAction: "Relink Asset",
        .relinkAssetHelp: "Choose the moved or renamed source file and update every project reference.",
        .relinkAssetMessage: "Choose the replacement source file for this PhotonStack asset.",
        .relinkAssetSuccess: "Relinked asset",
        .relinkAssetInvalid: "Choose an existing supported image file.",
        .relinkAssetDuplicate: "That file is already linked as another project asset.",
        .missingAssetStatus: "Source file missing",
        .missingAssetExportError: "Relink the missing source before full-resolution export",
        .missingAssetProcessingError: "Relink the missing source before processing",
        .missingWorkspaceSourceError: "This workspace source is missing; replay or regenerate it first",
        .imageProcessingRequiresImageLayer: "Select an image layer before using image processing tools",
        .emptyAssetsTitle: "No assets yet",
        .emptyAssetsMessage: "Start by importing RAW, FITS, TIFF, JPEG, PNG, or HEIF files.",
        .fitToWindow: "Fit to Window",
        .actualSize: "1:1",
        .zoomIn: "Zoom In",
        .zoomOut: "Zoom Out",
        .originalPreview: "Original",
        .markedPreview: "Marked",
        .artifactMarkedPreview: "Compare original and detected trail markers",
        .markedPreviewRenderFailed: "Detection finished, but the marked preview could not be rendered.",
        .nameLabel: "Name",
        .kindLabel: "Kind",
        .pathLabel: "Path",
        .roleLabel: "Role",
        .noAssetSelected: "No asset selected",
        .workflowSection: "Workflow",
        .processMenu: "Process",
        .masterLabel: "Master",
        .darkBiasStateLabel: "Dark pedestal",
        .flatBiasStateLabel: "Flat pedestal",
        .calibrationBiasStateHelp: "Choose whether the source master still contains the camera bias pedestal.",
        .flatBiasMissingWarning: "A flat marked as containing Bias requires Bias frames.",
        .stackLabel: "Stack",
        .alignLabel: "Align",
        .restoreMeteorsAfterStack: "Restore meteors after stack",
        .runStackWorkflow: "Run Stack Workflow",
        .exportProductManifest: "Export Product Manifest",
        .registrationSection: "Registration",
        .referenceFrame: "Reference",
        .movingFrame: "Moving frame",
        .registerFrames: "Register Frames",
        .registerBatchFrames: "Batch Register Frames",
        .batchRegistrationFrames: "Frames to align",
        .selectAllFrames: "All",
        .clearFrameSelection: "Clear",
        .onlyCurrentFrame: "Current",
        .registrationNeedsTwoFrames: "Import at least two frames to run registration.",
        .registrationNoFramesSelected: "Select at least one moving frame for batch registration.",
        .registrationReport: "Registration report",
        .registrationMatches: "Matches",
        .registrationOffset: "Offset",
        .registrationScale: "Scale",
        .registrationRotation: "Rotation",
        .parametersSection: "Parameters",
        .advancedProcessingSection: "Advanced Processing",
        .backgroundModel: "Background model",
        .backgroundMode: "Background mode",
        .backgroundStrength: "Background strength",
        .preserveBackgroundBrightness: "Preserve background brightness",
        .preserveBackgroundBrightnessHelp: "Normalize the local background to the image's global background instead of 1.0.",
        .protectBrightBackgroundTargets: "Protect bright targets",
        .protectBrightBackgroundTargetsHelp: "Estimate grid backgrounds from lower samples so stars, nebulae, and foreground subjects are less likely to become part of the background model.",
        .globalModel: "Global",
        .gridModel: "Grid",
        .subtractMode: "Subtract",
        .divideMode: "Divide",
        .applyBackground: "Apply Background",
        .cloudsSection: "Clouds / Haze",
        .detectClouds: "Detect Clouds",
        .removeSelectedClouds: "Remove Selected Clouds",
        .cloudRemovalStrength: "Cloud removal strength",
        .cloudSummary: "Cloud candidates",
        .topCloudRegions: "Top cloud regions",
        .selectAllClouds: "Select clouds",
        .clearCloudSelection: "Clear",
        .selectedCloudPreviewHint: "Checked cloud regions are highlighted in the marked preview.",
        .cloudTop20Hint: "Showing the top 20 cloud candidates first.",
        .normalizeAction: "Normalize",
        .targetBackground: "Target background",
        .targetScale: "Target scale",
        .colorSection: "Color",
        .neutralizeAction: "Neutralize",
        .saturationAction: "Saturate",
        .neutralizeStrength: "Neutralize strength",
        .saturationAmount: "Saturation",
        .starToolsSection: "Star Detection & Mask",
        .detectStarsAction: "Detect Stars",
        .createStarMaskAction: "Create Star Mask",
        .starDetectionThreshold: "Detection threshold",
        .starMinimumPeak: "Minimum peak",
        .starMaskRadius: "Mask radius",
        .largeStarMaskRadius: "Large-star radius",
        .layeredStarMask: "Layered mask",
        .detectedStarsLabel: "Detected stars",
        .deconvolutionSection: "Deconvolution",
        .deconvolveAction: "Deconvolve",
        .deconvolutionIterations: "Iterations",
        .deconvolutionRadius: "Radius",
        .deconvolutionSigma: "Sigma",
        .drizzleSection: "Drizzle",
        .drizzleAction: "Run Drizzle",
        .drizzleScale: "Scale",
        .drizzlePixfrac: "Pixel fraction",
        .drizzleAlign: "Alignment",
        .artifactTrailsSection: "Aircraft / Drone / Satellite / Meteor",
        .detectArtifactTrails: "Detect Trails",
        .removeArtificialTrails: "Remove Artificial",
        .cleanTimelapseTrails: "Clean Timelapse Sequence",
        .extractMeteorLayer: "Extract Meteor Layer",
        .restoreMeteorsAction: "Restore Meteors",
        .removeSelectedTrails: "Remove Selected",
        .allowMeteorRemoval: "Allow selected meteors",
        .artifactTrailSummary: "Detected",
        .protectedMeteors: "Protected meteors",
        .topArtifactTrails: "Top candidates",
        .artifactTrailWeight: "Weight",
        .artifactTrailTop20Hint: "Showing the top 20 candidates by weight.",
        .selectAllArtificialTrails: "Select artificial",
        .clearArtifactSelection: "Clear",
        .selectedArtifactPreviewHint: "Checked candidates are bright and numbered in the marked preview.",
        .airplaneTrail: "Airplane",
        .droneTrail: "Drone",
        .satelliteTrail: "Satellite",
        .meteorTrail: "Meteor",
        .rawPipelineSection: "RAW Pipeline",
        .rawProfessionalNote: "RAW decoding prefers Apple Core Image RAW/ProRAW and falls back to ImageIO when that backend is unavailable. Sensor-level controls remain planned for the professional RAW pipeline.",
        .rawMetadataSection: "Camera Metadata",
        .cameraLabel: "Camera",
        .lensLabel: "Lens",
        .captureDateLabel: "Captured",
        .exposureLabel: "Shutter",
        .exposureBiasLabel: "Exposure bias",
        .fNumberLabel: "Aperture",
        .isoLabel: "ISO",
        .focalLengthLabel: "Focal length",
        .colorProfileLabel: "Color profile",
        .rawDecoderLabel: "Decoder",
        .rawDecoderAppleActive: "Apple RAW active",
        .rawDecoderImageIOFallback: "ImageIO fallback; decode quality is system-controlled",
        .rawWhiteBalance: "White balance",
        .rawTemperature: "Temperature (K)",
        .rawTint: "Tint",
        .rawExposureBias: "Exposure bias",
        .rawBlackLevel: "Black level",
        .rawManualBlackLevel: "Black offset",
        .rawDemosaicQuality: "RAW decode",
        .rawLinearOutput: "Linear output",
        .rawAutoPreview: "Auto preview updates after RAW changes.",
        .rawAppliedParameters: "Preview applies only selected white balance, exposure, and black level.",
        .rawDeferredParameters: "Linear output is saved for processing/export and is not used as a display enhancement.",
        .previewWidth: "Preview",
        .stretchParameter: "Stretch",
        .starReduction: "Star reduction",
        .profileAwareStars: "Profile-aware stars",
        .edgeAwareStars: "Edge-aware stars",
        .comaReduction: "Coma reduction",
        .comaRadius: "Coma radius",
        .comaEccentricity: "Coma eccentricity",
        .edgeAwareComa: "Edge-aware coma",
        .localContrast: "Local contrast",
        .contrastRadius: "Contrast radius",
        .denoiseParameter: "Denoise",
        .chromaDenoiseParameter: "Chroma denoise",
        .denoiseRadius: "Denoise radius",
        .sharpenParameter: "Sharpen",
        .sharpenRadius: "Sharpen radius",
        .tasksSection: "Tasks",
        .noTasksYet: "No tasks yet",
        .processingStatus: "Processing",
        .consoleStatus: "Console",
        .readyStatus: "Ready",
        .consoleInputPlaceholder: "CLI arguments, e.g. help or inspect /path/image.tiff",
        .consoleRunCommand: "Run",
        .consolePopOut: "Pop Out",
        .consoleExpand: "Expand",
        .consoleCollapse: "Collapse",
        .consoleZoomIn: "Enlarge",
        .consoleZoomOut: "Restore",
        .consoleInputEmpty: "Enter a PhotonStack CLI command.",
        .consoleInputUnclosedQuote: "Command has an unclosed quote.",
        .languageLabel: "Language",
        .savedProject: "Saved project",
        .openedProject: "Opened project",
        .cancellingTask: "Cancelling current task...",
        .cancelledTask: "Task cancelled",
        .restoredPreview: "Restored previous preview",
        .workflowNoLights: "Assign at least one imported asset as Light before running the workflow.",
        .toolsPanel: "Tools",
        .expandAllTools: "Expand all",
        .collapseAllTools: "Collapse all",
        .inspectorProjectGroup: "Project & Settings",
        .inspectorAssetGroup: "Asset",
        .inspectorPhotoInfoGroup: "Photo Info",
        .inspectorWorkspaceGroup: "Workspace",
        .inspectorWorkflowGroup: "Deep Sky Workflow",
        .inspectorAdjustmentsGroup: "Image Adjustments",
        .inspectorBatchGroup: "Batch",
        .inspectorHistoryGroup: "History & Tasks",
        .inspectorProjectGroupIntro: "Create projects, choose templates, manage autosave, and reopen recent workspaces.",
        .inspectorAssetGroupIntro: "Review the selected file, assign calibration roles, and tune RAW decoding before processing.",
        .inspectorPhotoInfoGroupIntro: "Inspect image dimensions, format, camera, lens, exposure, ISO, aperture, focal length, white balance, and decoder details.",
        .inspectorWorkspaceGroupIntro: "Work with source assets, smart intermediate products, and the visible layer stack.",
        .inspectorWorkflowGroupIntro: "Analyze and review light frames, then stack and develop them in one workbench.",
        .inspectorAdjustmentsGroupIntro: "Use histogram, RGB curves, stretch, contrast, denoise, sharpen, star tools, background correction, trails, meteors, and cloud cleanup.",
        .inspectorBatchGroupIntro: "Queue repeated operations, pause or retry failed work, and keep long processing jobs organized.",
        .inspectorHistoryGroupIntro: "Review editable processing steps, replay the edit graph, and inspect completed or running tasks.",
        .photoInfoSection: "Photo Parameters",
        .imageSizeLabel: "Size",
        .formatLabel: "Format",
        .channelsLabel: "Channels",
        .bitDepthLabel: "Bit depth",
        .orientationLabel: "Orientation",
        .metadataUnavailable: "No camera metadata was found for this file.",
        .projectSection: "Project",
        .templateLabel: "Template",
        .newProject: "New Project",
        .projectName: "Project Name",
        .createProject: "Create",
        .createAndImport: "Create & Import",
        .autosaveLabel: "Autosave",
        .recentProjects: "Recent Projects",
        .noRecentProjects: "No recent projects",
        .editGraphSection: "Edit Graph",
        .noEditOperations: "No edit operations",
        .undoEditGraph: "Undo edit graph change",
        .redoEditGraph: "Redo edit graph change",
        .replayEditGraph: "Replay",
        .editGraphNeedsReplay: "Replay the edited graph before continuing or exporting.",
        .disabledOperationLayer: "Enable the linked edit operation before showing this layer.",
        .editGraphInvalidRecordedAssets: "Cannot replay because the recorded source asset list is invalid",
        .editGraphAmbiguousProvenance: "Cannot replay or export because the preview matches multiple edit operations",
        .editGraphMissingRecordedAssets: "Cannot replay because one or more recorded source assets are missing from the project",
        .editGraphMissingRecordedArtifact: "Cannot replay because the recorded intermediate product is missing or incomplete",
        .editGraphUnsupportedOperation: "This operation cannot be used as an edit graph step",
        .invalidEditOperationParameter: "Invalid edit operation parameter",
        .moveOperationUp: "Move up",
        .moveOperationDown: "Move down",
        .deleteOperation: "Delete",
        .productsSection: "Workspace",
        .workspaceAssetsTab: "Assets",
        .workspaceObjectsTab: "Smart Objects",
        .workspaceLayersTab: "Layers",
        .noSourceAssets: "No source assets yet",
        .addLayerMenu: "Add Layer",
        .addLayerFromSource: "Add Source Layer",
        .addLayerFromObject: "Add Object Layer",
        .activeLayerLabel: "Active layer",
        .noActiveLayer: "No active layer",
        .artifactsLabel: "Artifacts",
        .layersLabel: "Layers",
        .noArtifacts: "No intermediate products yet",
        .noLayers: "No layers yet",
        .framesLabel: "Frames",
        .sourceLabel: "Source",
        .previewProduct: "Preview",
        .previewLayerStack: "Preview Stack",
        .blendModeLabel: "Blend",
        .opacityLabel: "Opacity",
        .layerMaskLabel: "Layer Mask",
        .noLayerMask: "No Mask",
        .detachLayerMask: "Detach Mask",
        .editLayerMask: "Edit Mask",
        .invertLayerMask: "Invert Mask",
        .maskDensity: "Mask Density",
        .maskFeather: "Mask Feather",
        .maskBrushHide: "Hide",
        .maskBrushReveal: "Reveal",
        .maskBrushSize: "Brush Size",
        .maskBrushOpacity: "Brush Opacity",
        .undoMaskStroke: "Undo Stroke",
        .resetMaskStrokes: "Clear Strokes",
        .maskEditorTitle: "Layer Mask Editor",
        .visibleLabel: "Visible",
        .showLayer: "Show layer",
        .hideLayer: "Hide layer",
        .autosavedProject: "Autosaved project",
        .settingsSaveFailed: "Could not save application settings",
        .batchQueueSection: "Batch Queue",
        .queueStretch: "Queue Stretch",
        .queueDenoise: "Queue Denoise",
        .queueSharpen: "Queue Sharpen",
        .queueStars: "Queue Stars",
        .queueAllStretch: "All Stretch",
        .queueAllDenoise: "All Denoise",
        .queueAllSharpen: "All Sharpen",
        .queueAllStars: "All Stars",
        .startQueue: "Start Queue",
        .pauseQueue: "Pause",
        .resumeQueue: "Resume",
        .retryFailed: "Retry Failed",
        .clearQueue: "Clear",
        .noQueuedTasks: "No queued tasks",
        .batchOutputDirectory: "Output directory",
        .batchOutputFormat: "Output format",
        .batchConcurrency: "Concurrency",
        .chooseOutputDirectory: "Choose output directory",
        .clearOutputDirectory: "Clear output directory",
        .previewCache: "Preview cache",
        .clearPreviewCache: "Clear preview cache",
        .clearPreviewCacheConfirmationTitle: "Clear preview cache?",
        .clearPreviewCacheConfirmationMessage: "%d unreferenced preview cache files (%@) will be deleted. Project assets, intermediate products, and active history are preserved. This cannot be undone.",
        .previewCacheCleared: "Preview cache cleared",
        .cacheHitLabel: "cache",
        .executedLabel: "run",
        .histogramCurvesSection: "Histogram / Curves",
        .refreshHistogram: "Refresh Histogram",
        .applyCurve: "Apply Curve",
        .curveChannel: "Channel",
        .addCurvePoint: "Add point",
        .deleteCurvePoint: "Delete point",
        .resetCurvePoints: "Reset points",
        .curvePreset: "Curve preset",
        .customCurvePresets: "Custom presets",
        .saveCurvePreset: "Save current curve",
        .importCurvePresets: "Import presets",
        .exportCurvePresets: "Export presets",
        .curvePresetImportTooLarge: "Curve preset file exceeds the 4 MB import limit.",
        .curvePresetImportTooMany: "Curve preset file contains more than 1,000 presets.",
        .blackPoint: "Black point",
        .midInput: "Mid input",
        .midOutput: "Mid output",
        .whitePoint: "White point",
        .mosaicSection: "Mosaic",
        .mosaicPanels: "Panels",
        .mosaicEstimatedSize: "Estimated size",
        .mosaicMissingMetadata: "Inspect panels to estimate size",
        .mosaicHeightMismatch: "Panel heights differ",
        .estimateOverlap: "Estimate overlap",
        .mosaicProjection: "Projection",
        .mosaicLayout: "Layout",
        .mosaicAlignment: "Alignment",
        .mosaicBlend: "Blend",
        .mosaicExposureMatch: "Exposure match",
        .mosaicQualityReport: "Quality report",
        .mosaicFallbackPanels: "Fallback panels",
        .mosaicColumns: "Columns",
        .mosaicPreviewWidth: "Preview width",
        .previewMosaic: "Preview Mosaic",
        .overlapPixels: "Overlap pixels",
        .runMosaic: "Run Mosaic",
        .mosaicNoPanels: "Assign at least one asset as Mosaic before running mosaic.",
    ]

    private static let simplifiedChinese: [LocalizedTextKey: String] = [
        .importAction: "导入",
        .importFilesAction: "导入文件...",
        .importFolderAction: "导入文件夹...",
        .importFilesMessage: "选择一个或多个 RAW、FITS、TIFF、JPEG、PNG 或 HEIF 文件。",
        .importFolderMessage: "选择一个文件夹；其子文件夹中的受支持图像也会导入。",
        .assetImportQueued: "已排队 %d 个素材，将在当前任务结束后导入。",
        .assetsImported: "已导入 %d 个素材。",
        .openProjectMessage: "选择一个 PhotonStack 项目文件夹。",
        .saveProjectMessage: "选择 PhotonStack 项目文件夹的保存位置。",
        .exportImageMessage: "选择最终成片的文件名和格式。",
        .importCurvePresetsMessage: "选择一个 PhotonStack 曲线预设库。",
        .exportCurvePresetsMessage: "选择 PhotonStack 曲线预设库的保存位置。",
        .chooseBatchOutputDirectoryMessage: "选择批处理成片的输出文件夹。",
        .openAction: "打开",
        .saveAction: "保存",
        .unsavedChangesTitle: "继续前保存更改？",
        .unsavedChangesMessage: "当前项目包含尚未保存的更改。替换或关闭项目前，可以先保存这些更改。",
        .saveChangesAction: "保存更改",
        .discardChangesAction: "不保存",
        .unsavedChangesStatus: "有未保存的更改",
        .processingInterruptionTitle: "处理仍在运行",
        .processingInterruptionCloseMessage: "关闭窗口会停止当前操作，并丢弃尚未完成的结果。",
        .processingInterruptionQuitMessage: "退出 PhotonStack 会停止当前操作，并丢弃尚未完成的结果。",
        .stopProcessingAndCloseAction: "停止并关闭",
        .stopProcessingAndQuitAction: "停止并退出",
        .inspectAction: "检查",
        .previewAction: "预览",
        .sourcePreviewAction: "预览原片",
        .currentImageLabel: "当前图像",
        .stretchAction: "拉伸",
        .contrastAction: "对比",
        .denoiseAction: "降噪",
        .sharpenAction: "锐化",
        .starsAction: "星点",
        .comaAction: "去慧差",
        .backAction: "上一步",
        .resetAction: "重置",
        .cancelAction: "取消",
        .exportAction: "导出",
        .exportSettingsSection: "导出设置",
        .exportBitDepth: "位深",
        .exportColorSpace: "色彩空间",
        .fitsValueMode: "FITS 输入数值",
        .fitsValueModeHelp: "仅 FITS 能保留浮点科学值；PNG、JPEG、HEIF 和 TIFF 导出会进行显示归一化或整数量化。",
        .jpegQuality: "JPEG 质量",
        .exportSidecarFailed: "图像已导出，但无法保存 PhotonStack 辅助信息文件。",
        .noImage: "无图像",
        .assetSection: "素材",
        .removeAssetAction: "移除素材",
        .removeAssetHelp: "从项目中移除该素材，不删除原始文件。",
        .removeAssetConfirmationTitle: "移除这个素材？",
        .removeAssetsConfirmationTitle: "移除这些素材？",
        .removeAssetConfirmationMessage: "原始文件会保留在磁盘上；依赖该素材的图层、中间产物和队列任务也会从项目中移除。",
        .removeAssetsConfirmationMessage: "原始文件会保留在磁盘上；依赖这些素材的图层、中间产物和队列任务也会从项目中移除。",
        .removeArtifactAction: "移除中间产物",
        .removeArtifactHelp: "从项目中移除该中间产物，不删除磁盘文件。",
        .removeArtifactConfirmationTitle: "移除这个中间产物？",
        .removeArtifactConfirmationMessage: "磁盘文件会保留；依赖它的中间产物和图层也会从项目中移除，仍存在的图层会解除对它的蒙版引用。",
        .relinkAssetAction: "重新链接素材",
        .relinkAssetHelp: "选择移动或改名后的原始文件，并同步更新项目中的全部引用。",
        .relinkAssetMessage: "为当前 PhotonStack 素材选择替代原始文件。",
        .relinkAssetSuccess: "已重新链接素材",
        .relinkAssetInvalid: "请选择一个存在且受支持的图像文件。",
        .relinkAssetDuplicate: "该文件已经作为项目中的另一个素材使用。",
        .missingAssetStatus: "原始文件已失联",
        .missingAssetExportError: "全分辨率导出前请重新链接失联素材",
        .missingAssetProcessingError: "处理前请重新链接失联素材",
        .missingWorkspaceSourceError: "工作区来源已失效，请先重放或重新生成",
        .imageProcessingRequiresImageLayer: "请先选择图像图层，再使用图像处理工具",
        .emptyAssetsTitle: "还没有素材",
        .emptyAssetsMessage: "先导入 RAW、FITS、TIFF、JPEG、PNG 或 HEIF 文件。",
        .fitToWindow: "适配窗口",
        .actualSize: "1:1",
        .zoomIn: "放大",
        .zoomOut: "缩小",
        .originalPreview: "原图",
        .markedPreview: "标记",
        .artifactMarkedPreview: "对比原图和航迹标记图",
        .markedPreviewRenderFailed: "识别已完成，但无法渲染标记预览。",
        .nameLabel: "名称",
        .kindLabel: "类型",
        .pathLabel: "路径",
        .roleLabel: "角色",
        .noAssetSelected: "未选择素材",
        .workflowSection: "工作流",
        .processMenu: "处理",
        .masterLabel: "Master",
        .darkBiasStateLabel: "Dark 偏置状态",
        .flatBiasStateLabel: "Flat 偏置状态",
        .calibrationBiasStateHelp: "选择源 Master 是否仍包含相机 Bias 基线。",
        .flatBiasMissingWarning: "标记为包含 Bias 的 Flat 需要提供 Bias 素材。",
        .stackLabel: "堆栈",
        .alignLabel: "配准",
        .restoreMeteorsAfterStack: "堆栈后贴回流星",
        .runStackWorkflow: "运行堆栈工作流",
        .exportProductManifest: "导出产品清单",
        .registrationSection: "星点配准",
        .referenceFrame: "参考帧",
        .movingFrame: "待对齐帧",
        .registerFrames: "运行星点对齐",
        .registerBatchFrames: "批量星点对齐",
        .batchRegistrationFrames: "参与对齐",
        .selectAllFrames: "全选",
        .clearFrameSelection: "清空",
        .onlyCurrentFrame: "仅当前",
        .registrationNeedsTwoFrames: "至少导入两张图像后才能运行星点配准。",
        .registrationNoFramesSelected: "请至少选择一张待对齐帧用于批量星点对齐。",
        .registrationReport: "配准报告",
        .registrationMatches: "匹配星点",
        .registrationOffset: "偏移",
        .registrationScale: "缩放",
        .registrationRotation: "旋转",
        .parametersSection: "参数",
        .advancedProcessingSection: "高级处理",
        .backgroundModel: "背景模型",
        .backgroundMode: "背景模式",
        .backgroundStrength: "背景强度",
        .preserveBackgroundBrightness: "保持背景亮度",
        .preserveBackgroundBrightnessHelp: "把局部背景归一到整图背景亮度，而不是归一到 1.0。",
        .protectBrightBackgroundTargets: "保护亮目标",
        .protectBrightBackgroundTargetsHelp: "从网格中的较暗样本估计背景，减少星点、星云和地景主体被误纳入背景模型。",
        .globalModel: "全局",
        .gridModel: "网格",
        .subtractMode: "减背景",
        .divideMode: "除背景",
        .applyBackground: "应用背景",
        .cloudsSection: "云雾 / 薄云",
        .detectClouds: "识别云雾",
        .removeSelectedClouds: "去除已选云雾",
        .cloudRemovalStrength: "去云强度",
        .cloudSummary: "云雾候选",
        .topCloudRegions: "候选区域 Top20",
        .selectAllClouds: "全选云雾",
        .clearCloudSelection: "清空",
        .selectedCloudPreviewHint: "勾选的云雾区域会在标记预览中高亮。",
        .cloudTop20Hint: "候选较多时先展示置信度最高的 20 个区域。",
        .normalizeAction: "归一化",
        .targetBackground: "目标背景",
        .targetScale: "目标尺度",
        .colorSection: "颜色",
        .neutralizeAction: "背景中和",
        .saturationAction: "饱和度",
        .neutralizeStrength: "中和强度",
        .saturationAmount: "饱和度",
        .starToolsSection: "星点检测与蒙版",
        .detectStarsAction: "检测星点",
        .createStarMaskAction: "生成星点蒙版",
        .starDetectionThreshold: "检测阈值",
        .starMinimumPeak: "最低峰值",
        .starMaskRadius: "蒙版半径",
        .largeStarMaskRadius: "大星半径",
        .layeredStarMask: "分层蒙版",
        .detectedStarsLabel: "识别星点",
        .deconvolutionSection: "反卷积",
        .deconvolveAction: "反卷积",
        .deconvolutionIterations: "迭代",
        .deconvolutionRadius: "半径",
        .deconvolutionSigma: "Sigma",
        .drizzleSection: "Drizzle",
        .drizzleAction: "运行 Drizzle",
        .drizzleScale: "倍率",
        .drizzlePixfrac: "像素收缩比例",
        .drizzleAlign: "配准模型",
        .artifactTrailsSection: "飞机 / 无人机 / 卫星 / 流星",
        .detectArtifactTrails: "识别航迹",
        .removeArtificialTrails: "一键去除人造航迹",
        .cleanTimelapseTrails: "清理延时序列",
        .extractMeteorLayer: "提取流星层",
        .restoreMeteorsAction: "贴回流星",
        .removeSelectedTrails: "去除已选",
        .allowMeteorRemoval: "允许去除选中的流星",
        .artifactTrailSummary: "识别数量",
        .protectedMeteors: "已保护流星",
        .topArtifactTrails: "候选 Top20",
        .artifactTrailWeight: "权重",
        .artifactTrailTop20Hint: "候选较多时先展示权重最高的 20 条。",
        .selectAllArtificialTrails: "全选人造",
        .clearArtifactSelection: "清空",
        .selectedArtifactPreviewHint: "勾选项会在标记预览中高亮并显示编号。",
        .airplaneTrail: "飞机",
        .droneTrail: "无人机",
        .satelliteTrail: "卫星",
        .meteorTrail: "流星",
        .rawPipelineSection: "RAW 管线",
        .rawProfessionalNote: "RAW 优先使用 Apple Core Image RAW/ProRAW；该后端不可用时会回退到 ImageIO。传感器级控制将在专业 RAW 管线继续推进。",
        .rawMetadataSection: "相机元数据",
        .cameraLabel: "相机",
        .lensLabel: "镜头",
        .captureDateLabel: "拍摄时间",
        .exposureLabel: "快门",
        .exposureBiasLabel: "曝光补偿",
        .fNumberLabel: "光圈",
        .isoLabel: "ISO",
        .focalLengthLabel: "焦距",
        .colorProfileLabel: "色彩配置",
        .rawDecoderLabel: "解码器",
        .rawDecoderAppleActive: "Apple RAW 已启用",
        .rawDecoderImageIOFallback: "已回退 ImageIO；解码质量由系统控制",
        .rawWhiteBalance: "白平衡",
        .rawTemperature: "色温 (K)",
        .rawTint: "色调",
        .rawExposureBias: "曝光补偿",
        .rawBlackLevel: "黑电平",
        .rawManualBlackLevel: "黑位偏移",
        .rawDemosaicQuality: "RAW 解码",
        .rawLinearOutput: "线性输出",
        .rawAutoPreview: "RAW 参数变化后会自动刷新预览。",
        .rawAppliedParameters: "预览只应用当前选择的白平衡、曝光补偿和黑电平。",
        .rawDeferredParameters: "线性输出会保存到后续处理/导出参数，不作为显示增强叠加到预览。",
        .previewWidth: "预览",
        .stretchParameter: "拉伸",
        .starReduction: "缩星",
        .profileAwareStars: "保轮廓缩星",
        .edgeAwareStars: "边缘优先缩星",
        .comaReduction: "慧差减弱",
        .comaRadius: "慧差半径",
        .comaEccentricity: "慧差偏心阈值",
        .edgeAwareComa: "边缘优先慧差",
        .localContrast: "局部对比",
        .contrastRadius: "对比半径",
        .denoiseParameter: "降噪",
        .chromaDenoiseParameter: "色彩降噪",
        .denoiseRadius: "降噪半径",
        .sharpenParameter: "锐化",
        .sharpenRadius: "锐化半径",
        .tasksSection: "任务",
        .noTasksYet: "暂无任务",
        .processingStatus: "处理中",
        .consoleStatus: "控制台",
        .readyStatus: "就绪",
        .consoleInputPlaceholder: "输入 CLI 参数，例如 help 或 inspect /path/image.tiff",
        .consoleRunCommand: "运行",
        .consolePopOut: "弹出",
        .consoleExpand: "展开",
        .consoleCollapse: "收起",
        .consoleZoomIn: "放大",
        .consoleZoomOut: "还原",
        .consoleInputEmpty: "请输入 PhotonStack CLI 命令。",
        .consoleInputUnclosedQuote: "命令里有未闭合的引号。",
        .languageLabel: "语言",
        .savedProject: "已保存项目",
        .openedProject: "已打开项目",
        .cancellingTask: "正在取消当前任务...",
        .cancelledTask: "任务已取消",
        .restoredPreview: "已恢复上一张预览",
        .workflowNoLights: "运行工作流前至少需要一个 Light 素材。",
        .toolsPanel: "工具",
        .expandAllTools: "全部展开",
        .collapseAllTools: "全部收起",
        .inspectorProjectGroup: "项目与设置",
        .inspectorAssetGroup: "素材",
        .inspectorPhotoInfoGroup: "照片信息",
        .inspectorWorkspaceGroup: "操作区",
        .inspectorWorkflowGroup: "深空工作流",
        .inspectorAdjustmentsGroup: "图像调整",
        .inspectorBatchGroup: "批处理",
        .inspectorHistoryGroup: "历史与任务",
        .inspectorProjectGroupIntro: "用于新建项目、选择模板、管理自动保存，并快速打开最近处理过的工作区。",
        .inspectorAssetGroupIntro: "查看当前素材、分配亮场/暗场等校准角色，并在处理前调整 RAW 解码参数。",
        .inspectorPhotoInfoGroupIntro: "查看尺寸、格式、相机、镜头、曝光、ISO、光圈、焦距、白平衡和解码器等摄影参数。",
        .inspectorWorkspaceGroupIntro: "管理源素材、智能中间对象和最终可见的图层栈。",
        .inspectorWorkflowGroupIntro: "在同一个工作台中分析亮场、筛选原片，并完成校准、对齐、堆栈和显影。",
        .inspectorAdjustmentsGroupIntro: "包含直方图、RGB 曲线、拉伸、对比度、降噪、锐化、缩星、背景校正、航迹/流星/云雾处理等图像工具。",
        .inspectorBatchGroupIntro: "把重复处理加入队列，支持暂停、继续和重试，适合长时间批量任务。",
        .inspectorHistoryGroupIntro: "查看可重放的处理步骤、撤销/重做编辑图，并检查正在运行或已完成的任务。",
        .photoInfoSection: "摄影参数",
        .imageSizeLabel: "尺寸",
        .formatLabel: "格式",
        .channelsLabel: "通道",
        .bitDepthLabel: "位深",
        .orientationLabel: "方向",
        .metadataUnavailable: "这个文件没有读取到相机元数据。",
        .projectSection: "项目",
        .templateLabel: "模板",
        .newProject: "新建项目",
        .projectName: "项目名称",
        .createProject: "创建",
        .createAndImport: "创建并导入",
        .autosaveLabel: "自动保存",
        .recentProjects: "最近项目",
        .noRecentProjects: "暂无最近项目",
        .editGraphSection: "编辑图",
        .noEditOperations: "暂无编辑步骤",
        .undoEditGraph: "撤销编辑图修改",
        .redoEditGraph: "重做编辑图修改",
        .replayEditGraph: "重放",
        .editGraphNeedsReplay: "编辑图已修改，请先重放再继续处理或导出。",
        .disabledOperationLayer: "请先启用关联的编辑操作，再显示此图层。",
        .editGraphInvalidRecordedAssets: "无法重放：记录的源素材列表无效",
        .editGraphAmbiguousProvenance: "无法重放或导出：当前预览对应多个编辑操作，来源不唯一",
        .editGraphMissingRecordedAssets: "无法重放：项目中缺少一个或多个已记录的源素材",
        .editGraphMissingRecordedArtifact: "无法重放：记录的中间产物缺失或不完整",
        .editGraphUnsupportedOperation: "无法重放：此操作不能作为编辑步骤",
        .invalidEditOperationParameter: "编辑操作参数无效",
        .moveOperationUp: "上移",
        .moveOperationDown: "下移",
        .deleteOperation: "删除",
        .productsSection: "操作区",
        .workspaceAssetsTab: "素材",
        .workspaceObjectsTab: "智能对象",
        .workspaceLayersTab: "图层",
        .noSourceAssets: "还没有源素材",
        .addLayerMenu: "添加图层",
        .addLayerFromSource: "从素材建图层",
        .addLayerFromObject: "从对象建图层",
        .activeLayerLabel: "活动图层",
        .noActiveLayer: "未选择活动图层",
        .artifactsLabel: "中间产物",
        .layersLabel: "图层",
        .noArtifacts: "还没有中间产物",
        .noLayers: "还没有图层",
        .framesLabel: "帧数",
        .sourceLabel: "来源",
        .previewProduct: "预览",
        .previewLayerStack: "预览图层栈",
        .blendModeLabel: "混合",
        .opacityLabel: "不透明度",
        .layerMaskLabel: "图层蒙版",
        .noLayerMask: "无蒙版",
        .detachLayerMask: "解绑蒙版",
        .editLayerMask: "编辑蒙版",
        .invertLayerMask: "反相蒙版",
        .maskDensity: "蒙版密度",
        .maskFeather: "蒙版羽化",
        .maskBrushHide: "隐藏",
        .maskBrushReveal: "显示",
        .maskBrushSize: "画笔大小",
        .maskBrushOpacity: "画笔不透明度",
        .undoMaskStroke: "撤销笔触",
        .resetMaskStrokes: "清空笔触",
        .maskEditorTitle: "图层蒙版编辑器",
        .visibleLabel: "可见",
        .showLayer: "显示图层",
        .hideLayer: "隐藏图层",
        .autosavedProject: "已自动保存项目",
        .settingsSaveFailed: "无法保存应用设置",
        .batchQueueSection: "批处理队列",
        .queueStretch: "加入拉伸",
        .queueDenoise: "加入降噪",
        .queueSharpen: "加入锐化",
        .queueStars: "加入缩星",
        .queueAllStretch: "全部拉伸",
        .queueAllDenoise: "全部降噪",
        .queueAllSharpen: "全部锐化",
        .queueAllStars: "全部缩星",
        .startQueue: "开始队列",
        .pauseQueue: "暂停",
        .resumeQueue: "继续",
        .retryFailed: "重试失败",
        .clearQueue: "清空",
        .noQueuedTasks: "暂无队列任务",
        .batchOutputDirectory: "输出目录",
        .batchOutputFormat: "输出格式",
        .batchConcurrency: "并发数",
        .chooseOutputDirectory: "选择输出目录",
        .clearOutputDirectory: "清除输出目录",
        .previewCache: "预览缓存",
        .clearPreviewCache: "清理预览缓存",
        .clearPreviewCacheConfirmationTitle: "清理预览缓存？",
        .clearPreviewCacheConfirmationMessage: "将删除 %d 个未被引用的预览缓存文件（%@）。项目素材、中间产物和当前历史会被保留。此操作无法撤销。",
        .previewCacheCleared: "已清理预览缓存",
        .cacheHitLabel: "缓存",
        .executedLabel: "执行",
        .histogramCurvesSection: "直方图 / 曲线",
        .refreshHistogram: "刷新直方图",
        .applyCurve: "应用曲线",
        .curveChannel: "通道",
        .addCurvePoint: "添加控制点",
        .deleteCurvePoint: "删除控制点",
        .resetCurvePoints: "重置控制点",
        .curvePreset: "曲线预设",
        .customCurvePresets: "自定义预设",
        .saveCurvePreset: "保存当前曲线",
        .importCurvePresets: "导入预设",
        .exportCurvePresets: "导出预设",
        .curvePresetImportTooLarge: "曲线预设文件超过 4 MB 导入上限。",
        .curvePresetImportTooMany: "曲线预设文件包含超过 1000 条预设。",
        .blackPoint: "黑点",
        .midInput: "中点输入",
        .midOutput: "中点输出",
        .whitePoint: "白点",
        .mosaicSection: "接片",
        .mosaicPanels: "面板",
        .mosaicEstimatedSize: "预计尺寸",
        .mosaicMissingMetadata: "检查面板后可估算尺寸",
        .mosaicHeightMismatch: "面板高度不一致",
        .estimateOverlap: "估算重叠",
        .mosaicProjection: "投影",
        .mosaicLayout: "布局",
        .mosaicAlignment: "配准",
        .mosaicBlend: "融合",
        .mosaicExposureMatch: "曝光匹配",
        .mosaicQualityReport: "质量报告",
        .mosaicFallbackPanels: "回退面板",
        .mosaicColumns: "列数",
        .mosaicPreviewWidth: "预览宽度",
        .previewMosaic: "预览接片",
        .overlapPixels: "重叠像素",
        .runMosaic: "运行接片",
        .mosaicNoPanels: "运行接片前至少需要一个接片素材。",
    ]

    static func text(_ key: LocalizedTextKey, language: AppLanguage) -> String {
        switch language {
        case .english:
            return english[key] ?? key.rawValue
        case .simplifiedChinese:
            return simplifiedChinese[key] ?? english[key] ?? key.rawValue
        }
    }
}
