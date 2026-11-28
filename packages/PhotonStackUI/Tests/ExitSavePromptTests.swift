import Foundation
import Testing
import PhotonStackAppCore
import PhotonStackProcessing
@testable import PhotonStackUI

@Test func exitSavePromptDefaultsOffForNewAndExistingSettings() throws {
    #expect(!AppSettings().promptToSaveOnExit)
    #expect(!(try JSONDecoder().decode(AppSettings.self,from:Data("{}".utf8))).promptToSaveOnExit)
    let settings=AppSettings(promptToSaveOnExit:true)
    #expect(try JSONDecoder().decode(AppSettings.self,from:JSONEncoder().encode(settings)).promptToSaveOnExit)
}

@Test @MainActor func exitSavePromptOnlyControlsCloseAndQuit() throws {
    let asset=PhotonStackAsset(originalURL:URL(fileURLWithPath:"/tmp/exit-save-prompt.fits"),kind:.fits)
    let model=PhotonStackWorkspaceModel(project:PhotonStackProject(name:"Exit test",assets:[asset]),autosaveEnabled:false,processingService:UnavailableProcessingService())
    model.setRole(.dark,for:asset)
    #expect(model.hasUnsavedChanges)
    #expect(!model.promptToSaveOnExit)
    #expect(model.requestApplicationTermination())
    #expect(model.requestWindowClose())
    #expect(model.pendingProjectTransition==nil)
    // Replacing a project still asks before discarding changes.
    #expect(!model.requestCreateProject(template:.mosaic))
    #expect(model.pendingProjectTransition != nil)
    model.cancelPendingProjectTransition()
}

@Test @MainActor func exitSavePromptPersistsAndCanBeDisabledAgain() async throws {
    let directory=FileManager.default.temporaryDirectory.appendingPathComponent("ExitPrompt-"+UUID().uuidString)
    defer{try? FileManager.default.removeItem(at:directory)}
    let repository=AppSettingsRepository(settingsURL:directory.appendingPathComponent("settings.json"))
    let asset=PhotonStackAsset(originalURL:URL(fileURLWithPath:"/tmp/exit-save-prompt.fits"),kind:.fits)
    let model=PhotonStackWorkspaceModel(project:PhotonStackProject(name:"Exit test",assets:[asset]),autosaveEnabled:false,processingService:UnavailableProcessingService(),settingsRepository:repository)
    model.setPromptToSaveOnExit(true)
    await model.waitForPendingUserSettingsPersistence()
    #expect(try await repository.load().promptToSaveOnExit)
    #expect(model.applicationCommandSnapshot.promptToSaveOnExit)
    model.setRole(.dark,for:asset)
    #expect(!model.requestApplicationTermination())
    #expect(model.pendingProjectTransition == .terminateApplication)
    model.cancelPendingProjectTransition()
    #expect(!model.requestWindowClose())
    #expect(model.pendingProjectTransition == .closeWindow)
    model.cancelPendingProjectTransition()
    let reopened=PhotonStackWorkspaceModel(processingService:UnavailableProcessingService(),settingsRepository:repository)
    await reopened.loadUserSettings()
    await reopened.waitForPendingUserSettingsPersistence()
    #expect(reopened.promptToSaveOnExit)
    model.setPromptToSaveOnExit(false)
    await model.waitForPendingUserSettingsPersistence()
    #expect(!(try await repository.load()).promptToSaveOnExit)
    #expect(model.requestApplicationTermination())
    #expect(model.requestWindowClose())
}
