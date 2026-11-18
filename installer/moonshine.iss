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
; A small installer: the program, its scripts and manifest.json, about 100 MB. The 5.5 GB of
; weights, the database, the voices and the mod are [Files] entries with the download flag
; (Stage.ps1 writes them to download_files.iss from the manifest), so Setup fetches them
; itself during the install step: one progress bar over the whole, the file's name in the
; status line, the sha256 checked as each lands, Setup's own Retry/Skip/Cancel when one
; fails. A file already in place with the right hash is left alone, so running the
; installer again after a failed download, or over an older version, fetches only what is
; missing or changed. Moonshine.exe reads the same manifest for "Repair files" and for its
; update (app/gui/fetch.cpp), as Fetch-Package.ps1 does for the console launcher.
;
; What it installs is a window, Moonshine.exe, and the app it starts, moonshine_app.exe.
; The one Start Menu entry is that window; the check, the logs, updating, repairing and
; removing are inside it, where earlier versions had a Start Menu entry each. The window
; needs Microsoft Edge WebView2, which Windows 11 has and nearly every Windows 10; where
; it is missing, Microsoft's small bootstrapper is run, as vc_redist is. The console
; launcher (Moonshine.ps1, with Fetch-Package.ps1 and Collect-Logs.ps1) is still installed:
; Moonshine.exe offers it when it cannot make a window, and the verify scripts drive it.
;
; Moonshine.exe updates itself by downloading the new one of these and running it
; /SILENT /DIR=<where it is> /RELAUNCH=1, then closing; and removes itself by running the
; uninstaller /SILENT, then closing. Both wait here for that window to have gone (it
; holds a mutex for as long as its process lives), and /RELAUNCH=1 opens it again at the
; end of a silent install.
;
; The mod is one of the downloaded parts, and a task, on by default, copies it into the
; game's mods\ folder: the mod and the app then always come from the same build. The
; alternative is the Steam Workshop item, which updates on its own; somebody subscribed to
; it unticks the task, since both at once is a conflict (two mods, one id), which the
; task's text says. Silent installs that must not touch the game's folder pass
; /MERGETASKS=!copymod. /MIRROR=<folder> names a folder laid out like the installed one
; (release\stage\Moonshine on the build machine): a file found there with the right hash
; is copied, not downloaded, which is how the verify scripts install without fetching 5.5 GB.

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
; /DTestInstall: an installer for trying the install, the window's update and its uninstall
; on a machine that has a real Moonshine, without touching it. It is another application
; as far as Windows is concerned (its own AppId, so its own uninstall entry and its own
; memory of the last install), in its own folder, with its own Start Menu group and
; shortcut names. Pass /MERGETASKS=!copymod as well: the game's mods folder is shared.
#ifdef TestInstall
  #define AppIdValue "{{7C2E3C1A-5B7D-4E7E-9C64-3A1B2C4D5EFE}"
  #define Folder "MoonshineTest"
  #define Shortcut "Moonshine Test"
#else
  #define AppIdValue "{{7C2E3C1A-5B7D-4E7E-9C64-3A1B2C4D5E6F}"
  #define Folder "Moonshine"
  #define Shortcut "Moonshine"
#endif

[Setup]
AppId={#AppIdValue}
AppName={#AppName}
AppVersion={#Version}
AppVerName=Moonshine {#Version} (tester build)
AppPublisher=Pete Warden
DefaultDirName={localappdata}\{#Folder}
DefaultGroupName={#Shortcut}
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
UninstallDisplayName={#Shortcut} (tester build {#Version})
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
CloseApplicationsFilter=Moonshine.exe,moonshine_app.exe
; The window holds this while it is open (app/gui/main_gui.cpp), and gives it up before it
; starts this installer or the uninstaller itself. Anybody else is asked to close it.
AppMutex=Moonshine.KCD2.Window
RestartApplications=no
SetupLogging=yes
#ifdef Sign
; Stage.ps1 passes /DSign and /Sazure=<stage>\sign.cmd $f: signtool through Azure Artifact
; Signing. Setup.exe and the uninstaller get the signature; moonshine_app.exe was signed in
; the stage. A few retries, since the signing is a web service.
SignTool=azure
SignedUninstaller=yes
SignToolRetryCount=3
SignToolRetryDelay=2000
#endif

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Messages]
WelcomeLabel2=This will install Moonshine {#Version}, an early tester build, on your computer.%n%nIt puts a language model and a voice on your PC that let the unnamed people of Kuttenberg talk back. Around 5.5 GB is downloaded during the installation, so keep the internet connection up; if it is interrupted, run the installer again and it carries on from the files it already has. The mod is copied into the game's mods folder; nothing else in the game's folder is changed.%n%nIt is recommended that you close the game before continuing.

[Tasks]
Name: "desktopicon"; Description: "Put a Moonshine shortcut on the desktop"; GroupDescription: "Shortcuts:"
Name: "copymod"; Description: "Install the mod into the game's mods folder (untick ONLY if you subscribed to it on the Steam Workshop; both at once loads it twice)"; GroupDescription: "The mod:"; Check: GameDirKnown

[Files]
; The program and its scripts, compressed.
Source: "{#Pkg}\Moonshine.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Pkg}\moonshine_app.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Pkg}\*.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Pkg}\Moonshine.ps1"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Pkg}\Fetch-Package.ps1"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Pkg}\Collect-Logs.ps1"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Pkg}\README.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Pkg}\VERSION.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Pkg}\setting.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Pkg}\manifest.json"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Pkg}\LICENSES\*"; DestDir: "{app}\LICENSES"; Flags: ignoreversion recursesubdirs
Source: "{#Source}\moonshine.ico"; DestDir: "{app}"; Flags: ignoreversion
; The VC++ runtime, unpacked to run and then thrown away.
Source: "{#Stage}\extra\vc_redist.x64.exe"; DestDir: "{tmp}"; Flags: deleteafterinstall; Check: NeedsRedist
; Microsoft's WebView2 bootstrapper, likewise, where there is no WebView2 for the window.
Source: "{#Stage}\extra\MicrosoftEdgeWebview2Setup.exe"; DestDir: "{tmp}"; Flags: deleteafterinstall; Check: NeedsWebView2
; The downloads (data\, models\, mod\) and the mod's copy into the game: written by Stage.ps1.
#include Stage + "\download_files.iss"

[Dirs]
Name: "{app}\logs"

[Icons]
; One entry: the window. What used to be entries of their own here (the check, download or
; repair, update, collect logs, uninstall) is inside it; [InstallDelete] takes away the
; ones an earlier version left. Windows' own "Installed apps" still lists the uninstaller,
; and the guide keeps its entry.
Name: "{group}\{#Shortcut}"; Filename: "{app}\Moonshine.exe"; WorkingDir: "{app}"; Comment: "Start Moonshine and the game together"
Name: "{group}\Read me"; Filename: "{app}\README.txt"
Name: "{autodesktop}\{#Shortcut}"; Filename: "{app}\Moonshine.exe"; WorkingDir: "{app}"; Comment: "Start Moonshine and the game together"; Tasks: desktopicon

[Run]
; The runtime first, quietly; it asks for elevation itself. 1638 is "a newer one is
; already here" and 3010 is "reboot to finish", neither a failure for our purposes.
Filename: "{tmp}\vc_redist.x64.exe"; Parameters: "/install /quiet /norestart"; StatusMsg: "Installing the Microsoft Visual C++ runtime ..."; Flags: waituntilterminated; Check: NeedsRedist
; WebView2, where Windows does not have it: the bootstrapper downloads the runtime (about
; 150 MB) and installs it for this user, or for everybody when Setup is elevated.
Filename: "{tmp}\MicrosoftEdgeWebview2Setup.exe"; Parameters: "/silent /install"; StatusMsg: "Installing Microsoft Edge WebView2 ..."; Flags: waituntilterminated; Check: NeedsWebView2
; Then the app itself, whose first screen is the check of the installation, so a bad
; install is seen here and not as a silent app on the first evening. --no-play: opened
; from the shortcut the window presses Play itself and Steam starts the game, and the end
; of an install is not the moment for that.
Filename: "{app}\Moonshine.exe"; Parameters: "--no-play"; WorkingDir: "{app}"; Description: "Open Moonshine now"; Flags: postinstall nowait skipifsilent
; The guide was the page before the install; here it is offered, not opened unasked.
Filename: "{app}\README.txt"; Description: "Read the guide"; Flags: postinstall shellexec nowait skipifsilent unchecked
; An update the window started (/SILENT /RELAUNCH=1) ends with the window open again, and
; again without the game being started: the player asked for an update, not for the game.
Filename: "{app}\Moonshine.exe"; Parameters: "--no-play"; WorkingDir: "{app}"; Flags: nowait skipifnotsilent runasoriginaluser; Check: WantsRelaunch

[InstallDelete]
; What an earlier version put here and this one does not: the Qwen3-TTS voice engine
; (to 0.9.96) with its 2.5 GB model, its OpenBLAS, its cloned recordings, and the lines
; spoken ahead in those voices, which would be heard in the wrong voice if they were ever
; asked for. Deleted before the files are copied, so this version's own phrases\ then
; lands clean. Setup does not otherwise touch a file it did not install.
Type: files; Name: "{app}\libopenblas.dll"
Type: filesandordirs; Name: "{app}\models\qwen3-tts-0.6b"
Type: filesandordirs; Name: "{app}\data\kcd2\voices\clones"
Type: filesandordirs; Name: "{app}\data\kcd2\voices\phrases"
; The Start Menu entries of the console launcher (to 0.9.102), whose jobs are in the window.
Type: files; Name: "{group}\Check the Moonshine installation.lnk"
Type: files; Name: "{group}\Download or repair Moonshine files.lnk"
Type: files; Name: "{group}\Update Moonshine.lnk"
Type: files; Name: "{group}\Collect Moonshine logs.lnk"
Type: files; Name: "{group}\Uninstall Moonshine.lnk"

[UninstallDelete]
Type: filesandordirs; Name: "{app}\logs"
Type: files; Name: "{app}\moonshine.cfg"
; The window's own WebView2 profile (cache and the like; it keeps nothing of the player's).
Type: filesandordirs; Name: "{app}\webview"
; What a repair or an update put there after the install, which the uninstall log does not know.
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

// The 2015-2022 x64 runtime, 14.44 or later. The prebuilt Moonshine Voice library is built
// with a recent toolset (newer than 14.41) against the dynamic runtime, and its STL calls (the vectorised
// search, find_end, remove and find_first_of helpers) are not in an older msvcp140.dll,
// so an older one would link and then fail to start. vc_redist.x64.exe from aka.ms is
// current, and a no-op on a machine that already has it. The Installed value and the
// version are read from the registry the redistributable writes.
function NeedsRedist: Boolean;
var
  Installed, Major, Minor: Cardinal;
begin
  Result := True;
  if RegQueryDWordValue(HKLM, 'SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64', 'Installed', Installed) and (Installed = 1) then
    if RegQueryDWordValue(HKLM, 'SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64', 'Major', Major) and
       RegQueryDWordValue(HKLM, 'SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64', 'Minor', Minor) then
      Result := not ((Major > 14) or ((Major = 14) and (Minor >= 44)));
  if not Result then Log('VC++ runtime already present, not running vc_redist');
end;

// The window is drawn by Microsoft Edge WebView2. Windows 11 comes with it and Edge's own
// updates have put it on nearly every Windows 10, so this is rarely true. The test is
// Microsoft's: a version under the runtime's client key, for the machine or for this user.
function WebView2Version(const Root: Integer): String;
begin
  Result := '';
  if not RegQueryStringValue(Root, 'SOFTWARE\Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}', 'pv', Result) then
    Result := '';
  if Result = '0.0.0.0' then Result := '';
end;

function NeedsWebView2: Boolean;
var
  Found: String;
begin
  Found := WebView2Version(HKLM32);
  if Found = '' then Found := WebView2Version(HKCU);
  Result := Found = '';
  if Result then Log('no WebView2 runtime; running the bootstrapper')
  else Log('WebView2 runtime ' + Found + ' already present');
end;

// /RELAUNCH=1: the window started this install to update itself, and wants to be opened
// again when it is done.
function WantsRelaunch: Boolean;
begin
  Result := ExpandConstant('{param:RELAUNCH|0}') = '1';
end;

// The window that started an update or an uninstall closes itself straight after, and its
// file cannot be replaced or deleted until it has. It holds this mutex from its first
// moment to its last (the one in AppMutex it gives up early, so as not to be asked to
// close itself), so its going is waited for here: a second or so, fifteen at the most.
// Both waits come after the AppMutex question, so a window the player simply left open is
// asked about first, and then given the time it takes to stop the engine and go.
procedure WaitForTheWindowToGo;
var
  I: Integer;
begin
  for I := 1 to 100 do begin
    if not CheckForMutexes('Moonshine.KCD2.Process') then exit;
    Sleep(150);
  end;
  Log('Moonshine.exe is still running after 15 s');
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  WaitForTheWindowToGo;
  Result := '';
end;

// A key for a model API, if the player gave the window one, is kept by Windows (Credential
// Manager), not in this folder. The window removes it when the uninstall is started from
// it; this is for an uninstall started from Windows' own list. cmdkey says "not found"
// when there is none, which is as good.
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Code: Integer;
begin
  if CurUninstallStep = usUninstall then WaitForTheWindowToGo;
  if CurUninstallStep = usPostUninstall then
    Exec(ExpandConstant('{sys}\cmdkey.exe'), '/delete:"Moonshine: model API key"', '', SW_HIDE, ewWaitUntilTerminated, Code);
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
// does not record. The window's own check will say more, and its Settings can be told
// where the game is.
function InitializeSetup: Boolean;
begin
  Result := True;
  RightOnes := TStringList.Create;
  WrongOnes := TStringList.Create;
  if ExpandConstant('{param:MIRROR}') <> '' then Log('mirror: ' + ExpandConstant('{param:MIRROR}'));
  if GameDir('') = '' then
    if SuppressibleMsgBox('Kingdom Come: Deliverance 2 was not found in a Steam library on this PC.' + #13#10#13#10 +
        'Moonshine can be installed anyway, but the mod cannot be copied into the game''s folder, and Moonshine will look for the game again when it opens (its folder can be chosen in Moonshine''s Settings). Continue?',
        mbConfirmation, MB_YESNO, IDYES) = IDNO then
      Result := False;
end;
