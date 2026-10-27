; Moonshine for Kingdom Come: Deliverance 2 -- the tester installer.
;
; Compiled on the Windows laptop by installer\Stage.ps1 after it has assembled the
; package tree:
;
;   ISCC.exe /DVersion=0.9.94 /DStage=<repo>\release\stage /DSource=<repo>\installer /Oout moonshine.iss
;
; Per-user, no administrator prompt: it installs under %LOCALAPPDATA%\Moonshine, which is
; also where the app writes its logs, and the one thing that would want elevation (the
; VC++ runtime) elevates itself when it has to. Close to 8 GB of weights go in, so the
; output is sliced (setup.exe + .bin files, 2 GB each) and the weights are stored rather
; than compressed: they are already dense and LZMA over them costs an hour for nothing.
;
; The mod is NOT installed here. Testers subscribe to it on the Steam Workshop and the
; game loads it from its workshop folder. An unchecked task offers to drop a copy in the
; game's mods\ folder instead, for a tester without the Workshop route; both at once is a
; conflict (two mods, one id), which is why it is off by default and says so.

#ifndef Version
  #error Pass /DVersion=x.y.z
#endif
#ifndef Stage
  #define Stage "..\release\stage"
#endif
#ifndef Source
  #define Source "."
#endif
#define AppName "Moonshine for Kingdom Come: Deliverance 2"
#define Pkg Stage + "\Moonshine"

[Setup]
AppId={{7C2E3C1A-5B7D-4E7E-9C64-3A1B2C4D5E6F}
AppName={#AppName}
AppVersion={#Version}
AppVerName=Moonshine {#Version} (tester build)
AppPublisher=Pete Warden
DefaultDirName={localappdata}\Moonshine
DefaultGroupName=Moonshine
DisableProgramGroupPage=yes
DisableWelcomePage=no
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
OutputBaseFilename=Moonshine-{#Version}-setup
SetupIconFile={#Source}\moonshine.ico
UninstallDisplayIcon={app}\moonshine.ico
UninstallDisplayName=Moonshine (tester build {#Version})
LicenseFile={#Pkg}\LICENSES\README.txt
InfoBeforeFile={#Pkg}\README.txt
; The slices: Inno's 2 GB-ish ceiling per file. Setup.exe stays small and the weights
; land in Moonshine-x.y.z-setup-1.bin, -2.bin ... all of which must sit beside it.
DiskSpanning=yes
SlicesPerDisk=1
DiskSliceSize=2100000000
Compression=lzma2/fast
SolidCompression=no
LZMAUseSeparateProcess=yes
; Room for the package plus the runtime installer's own unpacking.
ExtraDiskSpaceRequired=314572800
WizardStyle=modern
ShowLanguageDialog=no
CloseApplications=yes
CloseApplicationsFilter=moonshine_app.exe
RestartApplications=no
SetupLogging=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Messages]
WelcomeLabel2=This will install Moonshine {#Version}, an early tester build, on your computer.%n%nIt puts a language model and a voice on your PC that let the unnamed people of Kuttenberg talk back. Around 8 GB goes on disk. Nothing in the game's folder is changed; the mod itself comes from the Steam Workshop.%n%nIt is recommended that you close the game before continuing.

[Tasks]
Name: "desktopicon"; Description: "Put a Moonshine shortcut on the desktop"; GroupDescription: "Shortcuts:"
Name: "copymod"; Description: "Also copy the mod into the game's mods folder (NOT needed if you subscribed on the Steam Workshop; both at once loads it twice)"; GroupDescription: "Without the Workshop:"; Flags: unchecked; Check: GameDirKnown

[Files]
; The program and its scripts, compressed.
Source: "{#Pkg}\moonshine_app.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Pkg}\*.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Pkg}\Moonshine.ps1"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Pkg}\Collect-Logs.ps1"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Pkg}\README.txt"; DestDir: "{app}"; Flags: ignoreversion isreadme
Source: "{#Pkg}\VERSION.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Pkg}\setting.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Pkg}\LICENSES\*"; DestDir: "{app}\LICENSES"; Flags: ignoreversion recursesubdirs
Source: "{#Source}\moonshine.ico"; DestDir: "{app}"; Flags: ignoreversion
; The voices and the words: small, compressed.
Source: "{#Pkg}\data\kcd2\voices\*"; DestDir: "{app}\data\kcd2\voices"; Flags: ignoreversion recursesubdirs
; The weights and the database: stored. A file this size is also a bad candidate for
; "ignoreversion" comparisons, so they are simply replaced.
Source: "{#Pkg}\data\kcd2\npcs.sqlite"; DestDir: "{app}\data\kcd2"; Flags: ignoreversion nocompression
Source: "{#Pkg}\models\*.gguf"; DestDir: "{app}\models"; Flags: ignoreversion nocompression
Source: "{#Pkg}\models\llm\*"; DestDir: "{app}\models\llm"; Flags: ignoreversion nocompression recursesubdirs
Source: "{#Pkg}\models\stt\*"; DestDir: "{app}\models\stt"; Flags: ignoreversion nocompression recursesubdirs
Source: "{#Pkg}\models\qwen3-tts-0.6b\*"; DestDir: "{app}\models\qwen3-tts-0.6b"; Flags: ignoreversion nocompression recursesubdirs
; The VC++ runtime, unpacked to run and then thrown away.
Source: "{#Stage}\extra\vc_redist.x64.exe"; DestDir: "{tmp}"; Flags: deleteafterinstall; Check: NeedsRedist
; The mod, only for the task that asks for it.
Source: "{#Stage}\mod\moonshine_names\*"; DestDir: "{code:GameDir}\mods\moonshine_names"; Flags: ignoreversion recursesubdirs uninsneveruninstall; Tasks: copymod

[Dirs]
Name: "{app}\logs"

[Icons]
Name: "{group}\Moonshine"; Filename: "powershell.exe"; Parameters: "-NoExit -ExecutionPolicy Bypass -File ""{app}\Moonshine.ps1"""; WorkingDir: "{app}"; IconFilename: "{app}\moonshine.ico"; Comment: "Start Moonshine and the game together"
Name: "{group}\Check the Moonshine installation"; Filename: "powershell.exe"; Parameters: "-NoExit -ExecutionPolicy Bypass -File ""{app}\Moonshine.ps1"" -Check"; WorkingDir: "{app}"; IconFilename: "{app}\moonshine.ico"
Name: "{group}\Collect Moonshine logs"; Filename: "powershell.exe"; Parameters: "-ExecutionPolicy Bypass -File ""{app}\Collect-Logs.ps1"""; WorkingDir: "{app}"; IconFilename: "{app}\moonshine.ico"
Name: "{group}\Read me"; Filename: "{app}\README.txt"
Name: "{group}\Uninstall Moonshine"; Filename: "{uninstallexe}"
Name: "{autodesktop}\Moonshine"; Filename: "powershell.exe"; Parameters: "-NoExit -ExecutionPolicy Bypass -File ""{app}\Moonshine.ps1"""; WorkingDir: "{app}"; IconFilename: "{app}\moonshine.ico"; Tasks: desktopicon

[Run]
; The runtime first, quietly; it asks for elevation itself. 1638 is "a newer one is
; already here" and 3010 is "reboot to finish", neither a failure for our purposes.
Filename: "{tmp}\vc_redist.x64.exe"; Parameters: "/install /quiet /norestart"; StatusMsg: "Installing the Microsoft Visual C++ runtime ..."; Flags: waituntilterminated; Check: NeedsRedist
; Then the package's own check, in a window the tester can read, so a bad install is seen
; here and not as a silent app on the first evening.
Filename: "powershell.exe"; Parameters: "-NoExit -ExecutionPolicy Bypass -File ""{app}\Moonshine.ps1"" -Check"; WorkingDir: "{app}"; Description: "Check the installation now (recommended)"; Flags: postinstall nowait skipifsilent

[UninstallDelete]
Type: filesandordirs; Name: "{app}\logs"
Type: files; Name: "{app}\moonshine.cfg"

[Code]
var
  GameDirCache: String;
  GameDirLooked: Boolean;

// Where Steam put the game: the install folder Steam records for app 1771300, failing
// that each library folder Steam lists, failing that the default. Empty when none of
// them has the game, in which case the copy task is not offered at all.
function ReadLibraryFolders(const SteamPath: String; var Folders: TArrayOfString): Boolean;
var
  Lines: TArrayOfString;
  I, P: Integer;
  Line, Value: String;
begin
  Result := False;
  SetArrayLength(Folders, 0);
  if not LoadStringsFromFile(SteamPath + '\steamapps\libraryfolders.vdf', Lines) then exit;
  for I := 0 to GetArrayLength(Lines) - 1 do begin
    Line := Trim(Lines[I]);
    if Pos('"path"', Line) = 1 then begin
      Value := Trim(Copy(Line, 7, Length(Line)));
      StringChangeEx(Value, '"', '', True);
      StringChangeEx(Value, '\\', '\', True);
      P := GetArrayLength(Folders);
      SetArrayLength(Folders, P + 1);
      Folders[P] := Trim(Value);
      Result := True;
    end;
  end;
end;

function FindGameDir: String;
var
  SteamPath, Candidate: String;
  Folders: TArrayOfString;
  I: Integer;
begin
  Result := '';
  if not RegQueryStringValue(HKCU, 'Software\Valve\Steam', 'SteamPath', SteamPath) then
    if not RegQueryStringValue(HKLM, 'SOFTWARE\WOW6432Node\Valve\Steam', 'InstallPath', SteamPath) then
      SteamPath := ExpandConstant('{commonpf32}') + '\Steam';
  StringChangeEx(SteamPath, '/', '\', True);
  if RegQueryStringValue(HKLM, 'SOFTWARE\WOW6432Node\Valve\Steam\Apps\1771300', 'InstallLocation', Candidate) then
    if FileExists(Candidate + '\Bin\Win64MasterMasterSteamPGO\KingdomCome.exe') then begin
      Result := Candidate;
      exit;
    end;
  if ReadLibraryFolders(SteamPath, Folders) then
    for I := 0 to GetArrayLength(Folders) - 1 do begin
      Candidate := Folders[I] + '\steamapps\common\KingdomComeDeliverance2';
      if FileExists(Candidate + '\Bin\Win64MasterMasterSteamPGO\KingdomCome.exe') then begin
        Result := Candidate;
        exit;
      end;
    end;
  Candidate := SteamPath + '\steamapps\common\KingdomComeDeliverance2';
  if FileExists(Candidate + '\Bin\Win64MasterMasterSteamPGO\KingdomCome.exe') then
    Result := Candidate;
end;

function GameDir(Param: String): String;
begin
  if not GameDirLooked then begin
    GameDirCache := FindGameDir;
    GameDirLooked := True;
    Log('game folder: ' + GameDirCache);
  end;
  Result := GameDirCache;
end;

function GameDirKnown: Boolean;
begin
  Result := GameDir('') <> '';
end;

// The 2015-2022 x64 runtime, 14.30 or later (what the build links against). The
// Installed value and the version are read from the registry the redistributable writes.
function NeedsRedist: Boolean;
var
  Installed, Major, Minor: Cardinal;
begin
  Result := True;
  if RegQueryDWordValue(HKLM, 'SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64', 'Installed', Installed) and (Installed = 1) then
    if RegQueryDWordValue(HKLM, 'SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64', 'Major', Major) and
       RegQueryDWordValue(HKLM, 'SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64', 'Minor', Minor) then
      Result := not ((Major > 14) or ((Major = 14) and (Minor >= 30)));
  if not Result then Log('VC++ runtime already present, not running vc_redist');
end;

// The app looks for the game under a few fixed paths (Program Files, X:\SteamLibrary); the
// folder Steam actually records is handed to it through moonshine.cfg's `root`, which the
// launcher passes as --root. Only written when the file does not already say where the
// game is, so a hand-edited one survives a reinstall.
procedure CurStepChanged(CurStep: TSetupStep);
var
  Cfg: String;
  Text: AnsiString;
begin
  if (CurStep = ssPostInstall) and GameDirKnown then begin
    Cfg := ExpandConstant('{app}\moonshine.cfg');
    Text := '';
    if FileExists(Cfg) then LoadStringFromFile(Cfg, Text);
    if Pos('root', Lowercase(String(Text))) = 0 then begin
      if (Text <> '') and (Copy(Text, Length(Text), 1) <> #10) then Text := Text + #13#10;
      Text := Text + AnsiString('# the game, as Steam records it; written by the installer' + #13#10 +
              'root = ' + GameDir('') + #13#10);
      SaveStringToFile(Cfg, Text, False);
    end;
  end;
end;

// A warning, not a refusal: the game may be about to be installed, or be somewhere Steam
// does not record. The launcher's own check will say more.
function InitializeSetup: Boolean;
begin
  Result := True;
  if GameDir('') = '' then
    if SuppressibleMsgBox('Kingdom Come: Deliverance 2 was not found in a Steam library on this PC.' + #13#10#13#10 +
        'Moonshine can be installed anyway and will look for the game again when it starts (its folder can be given in moonshine.cfg). Continue?',
        mbConfirmation, MB_YESNO, IDYES) = IDNO then
      Result := False;
end;
