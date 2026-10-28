; Moonshine for Kingdom Come: Deliverance 2 -- the tester installer.
;
; Compiled on the Windows laptop by installer\Stage.ps1 (Inno Setup 6.5 or later) after it
; has assembled the package tree and written manifest.json and download_files.iss:
;
;   ISCC.exe /DVersion=0.9.94 /DStage=<repo>\release\stage /DSource=<repo>\installer /Oout moonshine.iss
;
; Per-user, no administrator prompt: it installs under %LOCALAPPDATA%\Moonshine, which is
; also where the app writes its logs, and the one thing that would want elevation (the
; VC++ runtime) elevates itself when it has to.
;
; A small installer: the program, its scripts and manifest.json, about 100 MB. The 8 GB of
; weights, the database, the voices and the mod are [Files] entries with the download flag
; (Stage.ps1 writes them to download_files.iss from the manifest), so Setup fetches them
; itself during the install step: one progress bar over the whole, the file's name in the
; status line, the sha256 checked as each lands, Setup's own Retry/Skip/Cancel when one
; fails. A file already in place with the right hash is left alone, so running the
; installer again after a failed download, or over an older version, fetches only what is
; missing or changed. Fetch-Package.ps1 reads the same manifest for the Start Menu's
; "Download or repair Moonshine files" and for Moonshine.ps1 -Update.
;
; The mod is one of the downloaded parts, and a task, on by default, copies it into the
; game's mods\ folder: the mod and the app then always come from the same build. The
; alternative is the Steam Workshop item, which updates on its own; somebody subscribed to
; it unticks the task, since both at once is a conflict (two mods, one id), which the
; task's text says. Silent installs that must not touch the game's folder pass
; /MERGETASKS=!copymod. /MIRROR=<folder> names a folder laid out like the installed one
; (release\stage\Moonshine on the build machine): a file found there with the right hash
; is copied, not downloaded, which is how the verify scripts install without fetching 8 GB.

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
Compression=lzma2/max
SolidCompression=yes
LZMAUseSeparateProcess=yes
; The downloads are counted through their ExternalSize; this is the runtime installer's
; unpacking and the logs.
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
WelcomeLabel2=This will install Moonshine {#Version}, an early tester build, on your computer.%n%nIt puts a language model and a voice on your PC that let the unnamed people of Kuttenberg talk back. Around 8 GB is downloaded during the installation, so keep the internet connection up; if it is interrupted, run the installer again and it carries on from the files it already has. The mod is copied into the game's mods folder; nothing else in the game's folder is changed.%n%nIt is recommended that you close the game before continuing.

[Tasks]
Name: "desktopicon"; Description: "Put a Moonshine shortcut on the desktop"; GroupDescription: "Shortcuts:"
Name: "copymod"; Description: "Install the mod into the game's mods folder (untick ONLY if you subscribed to it on the Steam Workshop; both at once loads it twice)"; GroupDescription: "The mod:"; Check: GameDirKnown

[Files]
; The program and its scripts, compressed.
Source: "{#Pkg}\moonshine_app.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Pkg}\*.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Pkg}\Moonshine.ps1"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Pkg}\Fetch-Package.ps1"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Pkg}\Collect-Logs.ps1"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Pkg}\README.txt"; DestDir: "{app}"; Flags: ignoreversion isreadme
Source: "{#Pkg}\VERSION.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Pkg}\setting.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Pkg}\manifest.json"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Pkg}\LICENSES\*"; DestDir: "{app}\LICENSES"; Flags: ignoreversion recursesubdirs
Source: "{#Source}\moonshine.ico"; DestDir: "{app}"; Flags: ignoreversion
; The VC++ runtime, unpacked to run and then thrown away.
Source: "{#Stage}\extra\vc_redist.x64.exe"; DestDir: "{tmp}"; Flags: deleteafterinstall; Check: NeedsRedist
; The downloads (data\, models\, mod\) and the mod's copy into the game: written by Stage.ps1.
#include Stage + "\download_files.iss"

[Dirs]
Name: "{app}\logs"

[Icons]
Name: "{group}\Moonshine"; Filename: "powershell.exe"; Parameters: "-NoExit -ExecutionPolicy Bypass -File ""{app}\Moonshine.ps1"""; WorkingDir: "{app}"; IconFilename: "{app}\moonshine.ico"; Comment: "Start Moonshine and the game together"
Name: "{group}\Check the Moonshine installation"; Filename: "powershell.exe"; Parameters: "-NoExit -ExecutionPolicy Bypass -File ""{app}\Moonshine.ps1"" -Check"; WorkingDir: "{app}"; IconFilename: "{app}\moonshine.ico"
Name: "{group}\Download or repair Moonshine files"; Filename: "powershell.exe"; Parameters: "-NoExit -ExecutionPolicy Bypass -File ""{app}\Fetch-Package.ps1"""; WorkingDir: "{app}"; IconFilename: "{app}\moonshine.ico"; Comment: "Fetch again any model or data file that is missing or damaged"
Name: "{group}\Update Moonshine"; Filename: "powershell.exe"; Parameters: "-NoExit -ExecutionPolicy Bypass -File ""{app}\Moonshine.ps1"" -Update"; WorkingDir: "{app}"; IconFilename: "{app}\moonshine.ico"; Comment: "Look for a newer Moonshine and install it"
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
; What Fetch-Package.ps1 put there after the install, which the uninstall log does not know.
Type: filesandordirs; Name: "{app}\data"
Type: filesandordirs; Name: "{app}\models"
Type: filesandordirs; Name: "{app}\mod"

[Code]
var
  GameDirCache: String;
  GameDirLooked: Boolean;
  RightOnes, WrongOnes: TStringList;   // paths already looked at: installed and correct, or not

// --- the downloads -------------------------------------------------------------------------
// Each file in download_files.iss has two entries: one that copies it from the /MIRROR
// folder, one that downloads it. Both are skipped when the file already installed has the
// right size and sha256, which is what makes a rerun after a broken download, or over an
// older version, fetch only what differs. The size is looked at first: a 2.5 GB hash is
// seconds, and a wrong size settles it without one. Checks can be asked more than once, so
// the verdicts are kept.
function Right(const Path, Bytes, Sha: String): Boolean;
var
  Full, Got: String;
  Size: Int64;
begin
  if RightOnes.IndexOf(Path) >= 0 then begin
    Result := True;
    exit;
  end;
  if WrongOnes.IndexOf(Path) >= 0 then begin
    Result := False;
    exit;
  end;
  Result := False;
  Full := ExpandConstant('{app}') + '\' + Path;
  if FileExists(Full) and FileSize64(Full, Size) and (IntToStr(Size) = Bytes) then begin
    try
      Got := GetSHA256OfFile(Full);
      Result := CompareText(Got, Sha) = 0;
    except
      Result := False;
    end;
  end;
  if Result then RightOnes.Add(Path) else WrongOnes.Add(Path);
  if Result then Log('already right: ' + Path);
end;

function MirrorHas(const Path, Bytes: String): Boolean;
var
  Full: String;
  Size: Int64;
begin
  Result := False;
  if ExpandConstant('{param:MIRROR}') = '' then exit;
  Full := ExpandConstant('{param:MIRROR}') + '\' + Path;
  Result := FileExists(Full) and FileSize64(Full, Size) and (IntToStr(Size) = Bytes);
end;

function FromMirror(const Path, Bytes, Sha: String): Boolean;
begin
  Result := (not Right(Path, Bytes, Sha)) and MirrorHas(Path, Bytes);
  if Result then Log('from the mirror: ' + Path);
end;

function FromWeb(const Path, Bytes, Sha: String): Boolean;
begin
  Result := (not Right(Path, Bytes, Sha)) and (not MirrorHas(Path, Bytes));
  if Result then Log('to download: ' + Path);
end;

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
  RightOnes := TStringList.Create;
  WrongOnes := TStringList.Create;
  if ExpandConstant('{param:MIRROR}') <> '' then Log('mirror: ' + ExpandConstant('{param:MIRROR}'));
  if GameDir('') = '' then
    if SuppressibleMsgBox('Kingdom Come: Deliverance 2 was not found in a Steam library on this PC.' + #13#10#13#10 +
        'Moonshine can be installed anyway, but the mod cannot be copied into the game''s folder, and the app will look for the game again when it starts (its folder can be given in moonshine.cfg). Continue?',
        mbConfirmation, MB_YESNO, IDYES) = IDNO then
      Result := False;
end;
