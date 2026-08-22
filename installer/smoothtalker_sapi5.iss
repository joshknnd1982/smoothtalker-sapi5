; SmoothTalker SAPI5 installer
;
; Installs both the 32-bit and the 64-bit SAPI5 engine, the SmoothTalker ROM
; and everything it needs to run, and the configuration utility.
;
; Accessibility notes, since this installer is for people who use screen
; readers:
;
;   * The wizard is built entirely from Inno Setup's standard pages, which are
;     ordinary Win32 controls that Narrator, NVDA and JAWS read correctly.  No
;     custom-drawn pages, no owner-draw controls, and no page whose only
;     content is an image.
;   * No page is disabled.  Skipping the welcome or ready pages saves a
;     sighted user two keystrokes and costs a screen reader user the summary
;     of what is about to happen, which is a bad trade.
;   * Every task and post-install checkbox has real label text, so it is
;     announced rather than being read as an anonymous check box.
;   * The installer never steals focus with a dialog that has no text.
;
; Build with: iscc installer\smoothtalker_sapi5.iss
; (build_all.bat does this for you after building both architectures.)

#define AppName        "SmoothTalker SAPI5"
#define AppShortName   "SmoothTalker"
#define AppVersion     "1.0.0"
#define AppPublisher   "SmoothTalker SAPI5"
#define ConfigExe      "SmoothTalkerConfig.exe"
#define SapiDll        "SmoothTalkerSAPI.dll"

#define SrcX86 "..\build_x86\bin\Release"
#define SrcX64 "..\build_x64\bin\Release"

[Setup]
AppId={{9F76A9D0-B0CD-4597-8173-BF79119D0662}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppPublisher}
DefaultDirName={autopf}\{#AppShortName}
DefaultGroupName={#AppShortName}
OutputDir=..\output
OutputBaseFilename=SmoothTalkerSAPI5_Setup
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
UninstallDisplayName={#AppName}
UninstallDisplayIcon={app}\{#ConfigExe}
; Registering a SAPI5 voice writes to HKLM: SAPI reads its list of token
; enumerators only from HKLM, and ignores an identical key under HKCU without
; saying so.  There is no per-user install of a SAPI5 voice.
PrivilegesRequired=admin
ArchitecturesInstallIn64BitMode=x64compatible
; Writes a full transcript to %TEMP%\Setup Log*.txt, which the [Code] section
; below also copies next to the engine's own logs.
SetupLogging=yes
; A running screen reader may have the SAPI DLL loaded; let Setup notice and
; offer to close what it can rather than failing on a locked file.
CloseApplications=yes
RestartApplications=no

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut for SmoothTalker Configuration"; GroupDescription: "Shortcuts:"

[Files]
; -- 32-bit: the SAPI5 engine every 32-bit application will load ------------
; restartreplace covers the upgrade case where a screen reader still has the
; old DLL mapped; without it Setup can only fail on the locked file.
Source: "{#SrcX86}\{#SapiDll}";   DestDir: "{app}"; Flags: ignoreversion restartreplace uninsrestartdelete
Source: "{#SrcX86}\unicorn.dll";  DestDir: "{app}"; Flags: ignoreversion restartreplace uninsrestartdelete
Source: "{#SrcX86}\{#ConfigExe}"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SrcX86}\st_render.exe"; DestDir: "{app}"; Flags: ignoreversion

; -- the ROM ---------------------------------------------------------------
; SmoothTalker 3.5's entire vocabulary, pronunciation rules and single male
; US-English voice live inside this one 640 KB memory image.  There are no
; separate voice or language files to install: see README.txt.
Source: "{#SrcX86}\engine.bin";   DestDir: "{app}"; Flags: ignoreversion

; -- 64-bit ----------------------------------------------------------------
; A copy of the ROM goes beside the 64-bit engine as well.  It could be found
; one directory up, and the engine does look there, but 640 KB is a small
; price for each architecture's folder being self-contained.
Source: "{#SrcX64}\{#SapiDll}";   DestDir: "{app}\x64"; Flags: ignoreversion restartreplace uninsrestartdelete; Check: Is64BitInstallMode
Source: "{#SrcX64}\unicorn.dll";  DestDir: "{app}\x64"; Flags: ignoreversion restartreplace uninsrestartdelete; Check: Is64BitInstallMode
Source: "{#SrcX64}\engine.bin";   DestDir: "{app}\x64"; Flags: ignoreversion; Check: Is64BitInstallMode
Source: "{#SrcX64}\st_render.exe"; DestDir: "{app}\x64"; Flags: ignoreversion; Check: Is64BitInstallMode

Source: "README.txt";             DestDir: "{app}"; Flags: ignoreversion isreadme
; Installed, but deliberately not shown as a LicenseFile wizard page: the GPL
; is not a click-through agreement for *using* the software, and presenting it
; as one that must be accepted before installing misrepresents it.
Source: "..\LICENSE";             DestDir: "{app}"; DestName: "LICENSE.txt"; Flags: ignoreversion

[Icons]
Name: "{group}\SmoothTalker Configuration"; Filename: "{app}\{#ConfigExe}"; Comment: "Adjust the rate, pitch, tone and volume of the SmoothTalker voice"
Name: "{group}\SmoothTalker Read Me"; Filename: "{app}\README.txt"
Name: "{group}\Licence (GPL v2)"; Filename: "{app}\LICENSE.txt"
Name: "{group}\{cm:UninstallProgram,{#AppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\SmoothTalker Configuration"; Filename: "{app}\{#ConfigExe}"; Comment: "Adjust the rate, pitch, tone and volume of the SmoothTalker voice"; Tasks: desktopicon

[Run]
Filename: "{app}\{#ConfigExe}"; Description: "Open SmoothTalker Configuration now"; Flags: postinstall nowait skipifsilent

; Nothing here on purpose.  The obvious entry would be the engine's log
; folder under {localappdata}, but Setup runs elevated, so {localappdata}
; is the administrator's folder and not the folder the engine actually wrote
; to -- it would delete the wrong user's logs and leave the right user's
; behind.  The logs are capped at 4 MB and README.txt says where they are.
; settings.ini is left alone deliberately, so reinstalling keeps the user's
; rate, pitch, tone and volume.

[Code]

var
  RegistrationFailed: Boolean;

// Where this installer's own transcript is kept.  Deliberately the install
// folder and not the engine's log folder under LocalAppData: Setup runs
// elevated, so LocalAppData would be the administrator's profile rather than
// the profile of the person who will actually be using the voice.
//
// (These comments use // rather than Pascal's { }, because a comment
// containing an Inno constant in braces would close itself at the constant.)
function InstallLogPath(): String;
begin
  Result := ExpandConstant('{app}\install.log');
end;

// Run regsvr32 and report what happened.  Returns the process exit code, or
// -1 if regsvr32 could not be started at all -- two failures worth telling
// apart, because the second one usually means the wrong regsvr32 path.
function RunRegSvr(const RegSvrPath, DllPath: String; Unregister: Boolean): Integer;
var
  Params: String;
  ResultCode: Integer;
begin
  Params := '/s';
  if Unregister then
    Params := Params + ' /u';
  Params := Params + ' "' + DllPath + '"';

  Log('SmoothTalker: ' + RegSvrPath + ' ' + Params);

  if not FileExists(RegSvrPath) then
  begin
    Log('SmoothTalker: regsvr32 not found at ' + RegSvrPath);
    Result := -1;
    Exit;
  end;
  if not FileExists(DllPath) then
  begin
    Log('SmoothTalker: DLL not found at ' + DllPath);
    Result := -1;
    Exit;
  end;

  if not Exec(RegSvrPath, Params, '', SW_HIDE, ewWaitUntilTerminated, ResultCode) then
  begin
    Log('SmoothTalker: could not start regsvr32, system error ' + IntToStr(ResultCode));
    Result := -1;
    Exit;
  end;

  Log('SmoothTalker: regsvr32 exit code ' + IntToStr(ResultCode));
  Result := ResultCode;
end;

// The 64-bit regsvr32 lives in System32 and the 32-bit one in SysWOW64 --
// which is the opposite of what the names suggest, and getting it backwards
// registers each DLL into the other architecture's registry view where its
// own SAPI will never look.
function RegSvr64(): String;
begin
  Result := ExpandConstant('{sys}\regsvr32.exe');
end;

function RegSvr32(): String;
begin
  if Is64BitInstallMode then
    Result := ExpandConstant('{syswow64}\regsvr32.exe')
  else
    Result := ExpandConstant('{sys}\regsvr32.exe');
end;

procedure RegisterEngines();
var
  Code: Integer;
begin
  RegistrationFailed := False;

  Log('SmoothTalker: registering the 32-bit SAPI5 engine');
  Code := RunRegSvr(RegSvr32(), ExpandConstant('{app}\{#SapiDll}'), False);
  if Code <> 0 then
    RegistrationFailed := True;

  if Is64BitInstallMode then
  begin
    Log('SmoothTalker: registering the 64-bit SAPI5 engine');
    Code := RunRegSvr(RegSvr64(), ExpandConstant('{app}\x64\{#SapiDll}'), False);
    if Code <> 0 then
      RegistrationFailed := True;
  end
  else
    Log('SmoothTalker: 32-bit Windows, so there is no 64-bit engine to register');

  if RegistrationFailed then
    MsgBox('SmoothTalker was installed, but registering the SAPI5 voice did not ' +
           'fully succeed, so the voice may not appear in your applications.' + #13#10 + #13#10 +
           'The installation log records what happened. You can find it at:' + #13#10 +
           InstallLogPath() + #13#10 + #13#10 +
           'Running the installer again as an administrator usually fixes this.',
           mbError, MB_OK);
end;

procedure UnregisterEngines();
begin
  Log('SmoothTalker: unregistering the 32-bit SAPI5 engine');
  RunRegSvr(RegSvr32(), ExpandConstant('{app}\{#SapiDll}'), True);

  if Is64BitInstallMode then
  begin
    Log('SmoothTalker: unregistering the 64-bit SAPI5 engine');
    RunRegSvr(RegSvr64(), ExpandConstant('{app}\x64\{#SapiDll}'), True);
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then
    RegisterEngines();
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then
  begin
    UnregisterEngines();
    // COM servers are not always released the instant regsvr32 returns; give
    // Windows a moment before the files underneath them are deleted.
    Sleep(1500);
  end;
end;

procedure DeinitializeSetup();
var
  Target: String;
begin
  // Keep a copy of Setup's own transcript next to what it installed, so a
  // bug report does not depend on finding %TEMP%.  Inno flushes the log as
  // it goes, so this copy is complete up to this point.
  if (Length(ExpandConstant('{log}')) > 0) and
     DirExists(ExpandConstant('{app}')) then
  begin
    Target := InstallLogPath();
    if CopyFile(ExpandConstant('{log}'), Target, False) then
      Log('SmoothTalker: copied this log to ' + Target);
  end;
end;
