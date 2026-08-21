; VoiceMorph installer
; Build with: iscc installer\VoiceMorph.iss
; Expects the CMake build to have already run into ..\build

#define AppName        "VoiceMorph"
#define AppVersion     "0.1.0"
#define Publisher      "IndieAudio"
#define AppExeName     "VoiceMorph.exe"

; JUCE writes its output here. Override on the command line with
;   iscc /DArtefacts="..\build\VoiceMorph_artefacts\Release" installer\VoiceMorph.iss
#ifndef Artefacts
  #define Artefacts "..\build\VoiceMorph_artefacts\Release"
#endif

[Setup]
AppId={{7C1E2F84-3B6A-4D19-9E52-0A4F6B8C1D33}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher={#Publisher}
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
OutputDir=..\dist
OutputBaseFilename=VoiceMorph-{#AppVersion}-Windows-Setup
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible

; Writing to Common Files\VST3 needs elevation.
PrivilegesRequired=admin

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Types]
Name: "full";   Description: "Everything"
Name: "app";    Description: "Standalone app only"
Name: "plugin"; Description: "VST3 plugin only"
Name: "custom"; Description: "Choose what to install"; Flags: iscustom

[Components]
Name: "standalone"; Description: "Standalone app (no DAW required)"; Types: full app custom; Flags: checkablealone
Name: "vst3";       Description: "VST3 plugin";                      Types: full plugin custom
Name: "onnx";       Description: "Neural conversion runtime";        Types: full; Check: OnnxRuntimePresent

[Files]
; --- Standalone -------------------------------------------------------------
Source: "{#Artefacts}\Standalone\{#AppExeName}"; \
    DestDir: "{app}"; Components: standalone; Flags: ignoreversion

; --- VST3 bundle (a folder on Windows, not a single file) -------------------
Source: "{#Artefacts}\VST3\{#AppName}.vst3\*"; \
    DestDir: "{commoncf64}\VST3\{#AppName}.vst3"; Components: vst3; \
    Flags: ignoreversion recursesubdirs createallsubdirs

; --- ONNX Runtime -----------------------------------------------------------
; The DLL has to sit next to each binary that loads it, so it goes in twice.
Source: "onnxruntime.dll"; DestDir: "{app}"; \
    Components: onnx and standalone; Flags: ignoreversion skipifsourcedoesntexist
Source: "onnxruntime.dll"; DestDir: "{commoncf64}\VST3\{#AppName}.vst3\Contents\x86_64-win"; \
    Components: onnx and vst3; Flags: ignoreversion skipifsourcedoesntexist

Source: "..\README.md"; DestDir: "{app}"; DestName: "README.txt"; Flags: isreadme ignoreversion

[Icons]
Name: "{group}\{#AppName}";       Filename: "{app}\{#AppExeName}"; Components: standalone
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExeName}"; Components: standalone; Tasks: desktopicon

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; Components: standalone; Flags: unchecked

[Run]
Filename: "{app}\{#AppExeName}"; Description: "Launch {#AppName}"; \
    Components: standalone; Flags: nowait postinstall skipifsilent

[Code]
function OnnxRuntimePresent: Boolean;
begin
  Result := FileExists(ExpandConstant('{src}\onnxruntime.dll'))
         or FileExists(ExpandConstant('{tmp}\onnxruntime.dll'));
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  Cable: Boolean;
begin
  if CurStep = ssPostInstall then
  begin
    // A voice changer is useless to Discord or OBS unless something can carry
    // its output back in as a microphone. Point this out once, at the end,
    // rather than burying it in a readme nobody opens.
    Cable := RegKeyExists(HKLM, 'SOFTWARE\VB-Audio\Cable')
          or RegKeyExists(HKLM, 'SOFTWARE\VB-Audio\Voicemeeter');

    if not Cable then
      MsgBox('VoiceMorph is installed.' + #13#10#13#10 +
             'To use it as a microphone in Discord, OBS or a game, you also need ' +
             'a virtual audio cable. VB-Cable is free: set it as VoiceMorph''s ' +
             'output, then select it as the input in the other app.' + #13#10#13#10 +
             'Without one, you can still hear the effect through your own speakers.',
             mbInformation, MB_OK);
  end;
end;
