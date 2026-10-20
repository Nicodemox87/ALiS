#define Version "0.1.0-alpha.5.8"
#define PythonURL "https://www.python.org/ftp/python/3.12.10/python-3.12.10-amd64.exe"
#define PythonSHA "67b5635e80ea51072b87941312d00ec8927c4db9ba18938f7ad2d27b328b95fb"
[Setup]
AppId=ALiS-Complete-Setup-r1
AppName=ALiS Complete Online Setup
AppVersion={#Version}
AppVerName=ALiS {#Version} — Complete Setup r1
AppPublisher=Nicodemo Abate — CNR-ISPC
PrivilegesRequired=lowest
ArchitecturesAllowed=x64os
ArchitecturesInstallIn64BitMode=x64os
MinVersion=10.0
CreateAppDir=no
Uninstallable=no
DisableProgramGroupPage=yes
DisableWelcomePage=no
WizardStyle=modern
SetupLogging=yes
Compression=lzma2
SolidCompression=yes
LicenseFile=..\..\LICENSE
InfoBeforeFile=INSTALL.txt
SetupIconFile=..\assets\ALiS.ico
OutputDir=..\..\output\complete-bootstrap
OutputBaseFilename=ALiS-{#Version}-Windows-x64-Complete-Setup-r1

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "italian"; MessagesFile: "compiler:Languages\Italian.isl"

[Files]
Source: "bootstrap.py"; Flags: dontcopy
Source: "verify_runtime.py"; Flags: dontcopy
Source: "dependencies.json"; Flags: dontcopy

[Code]
var
  BackendPage: TInputOptionWizardPage;
  ProgressPage: TOutputProgressWizardPage;
  DownloadPage: TDownloadWizardPage;
  PythonExe, LogDirectory: String;
  LastOutput: String;

function UsablePython(const P: String): Boolean;
var Code: Integer;
begin
  Result := False;
  if not FileExists(P) then exit;
  if Exec(P, '-I -c "import sys,struct,venv,ensurepip; assert sys.version_info[:2]==(3,12) and struct.calcsize(''P'')==8"', '', SW_HIDE, ewWaitUntilTerminated, Code) then
    Result := Code = 0;
end;

function FindPython: String;
var P: String;
begin
  Result := ExpandConstant('{param:BASEPYTHON|}');
  if (Result <> '') and UsablePython(Result) then exit;
  P := ExpandConstant('{localappdata}\ALiS\Python312\python.exe');
  if UsablePython(P) then begin Result := P; exit; end;
  if RegQueryStringValue(HKCU, 'Software\Python\PythonCore\3.12\InstallPath', 'ExecutablePath', P) then
    if UsablePython(P) then begin Result := P; exit; end;
  if RegQueryStringValue(HKLM64, 'Software\Python\PythonCore\3.12\InstallPath', 'ExecutablePath', P) then
    if UsablePython(P) then begin Result := P; exit; end;
  P := ExpandConstant('{localappdata}\Programs\Python\Python312\python.exe');
  if UsablePython(P) then begin Result := P; exit; end;
  Result := '';
end;

procedure ReceiveOutput(const S: String; const Error, FirstLine: Boolean);
var N: Integer;
begin
  Log(S);
  LastOutput := S;
  ProgressPage.SetText('Installing and checking ALiS components', Copy(S, 1, 230));
  if Pos('STEP ', S) = 1 then begin
    N := StrToIntDef(Copy(S, 6, 1), 0);
    ProgressPage.SetProgress(N, 6);
  end;
end;

procedure InitializeWizard;
begin
  BackendPage := CreateInputOptionPage(wpInfoBefore, 'ML and deep learning',
    'Choose the computation backend',
    'CPU includes all ML and deep-learning libraries. CUDA needs a supported NVIDIA GPU/driver and a larger download. Existing incompatible CC folders are preserved.', True, False);
  BackendPage.Add('CPU — complete ML + deep learning (recommended)');
  BackendPage.Add('NVIDIA CUDA 12.8 — verify GPU before activation');
  BackendPage.SelectedValueIndex := 0;
  if CompareText(ExpandConstant('{param:BACKEND|cpu}'), 'cuda') = 0 then BackendPage.SelectedValueIndex := 1;
  ProgressPage := CreateOutputProgressPage('Complete ALiS setup', 'CloudCompare, plugin, Python and ML/DL');
  DownloadPage := CreateDownloadPage('Python prerequisite', 'Downloading the verified official Python installer', nil);
  LogDirectory := ExpandConstant('{localappdata}\ALiS\InstallerLogs\') + GetDateTimeString('yyyymmdd-hhnnss', '-', ':');
  ForceDirectories(LogDirectory);
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var Code: Integer; Backend, Params, PythonDir, RequestedHost: String;
begin
  Result := '';
  if IsAdmin and (ExpandConstant('{param:ALLOWADMIN|0}') <> '1') then begin
    Result := 'Run this wizard normally, not as administrator. It will request elevation for native installers only; Python belongs to your Windows user.';
    exit;
  end;
  try
    ExtractTemporaryFile('bootstrap.py');
    ExtractTemporaryFile('verify_runtime.py');
    ExtractTemporaryFile('dependencies.json');
    PythonExe := FindPython;
    if PythonExe = '' then begin
      DownloadPage.Clear;
      DownloadPage.Add('{#PythonURL}', 'python-3.12.10-amd64.exe', '{#PythonSHA}');
      DownloadPage.Show;
      try DownloadPage.Download; finally DownloadPage.Hide; end;
      PythonDir := ExpandConstant('{localappdata}\ALiS\Python312');
      ProgressPage.SetText('Installing Python 3.12 x64 for this Windows user', 'No global packages, PATH or security-policy changes.');
      ProgressPage.Show;
      try
        if not Exec(ExpandConstant('{tmp}\python-3.12.10-amd64.exe'),
          '/passive /norestart InstallAllUsers=0 Include_launcher=0 Include_pip=1 Include_test=0 Include_doc=0 Include_tcltk=0 Shortcuts=0 AssociateFiles=0 PrependPath=0 TargetDir="' + PythonDir + '" /log "' + LogDirectory + '\python.log"',
          '', SW_SHOWNORMAL, ewWaitUntilTerminated, Code) then RaiseException('Could not start Python installer.');
        if Code = 3010 then begin NeedsRestart := True; RaiseException('Restart Windows and run this installer again.'); end;
        if Code <> 0 then RaiseException('Python installation failed. See ' + LogDirectory);
        PythonExe := PythonDir + '\python.exe';
        if not UsablePython(PythonExe) then RaiseException('Python installation verification failed.');
      finally ProgressPage.Hide; end;
    end;
    Backend := 'cpu';
    if BackendPage.SelectedValueIndex = 1 then Backend := 'cuda';
    Params := '-I -X utf8 -u "' + ExpandConstant('{tmp}\bootstrap.py') + '" --backend ' + Backend +
      ' --cache "' + ExpandConstant('{localappdata}\ALiS\InstallerCache') + '" --logs "' + LogDirectory + '"';
    RequestedHost := ExpandConstant('{param:HOSTDIR|}');
    if RequestedHost <> '' then Params := Params + ' --host "' + RequestedHost + '"';
    ProgressPage.SetProgress(0, 6);
    ProgressPage.Show;
    try
      if not ExecAndLogOutput(PythonExe, Params, '', SW_SHOWNORMAL, ewWaitUntilTerminated, Code, @ReceiveOutput) then
        RaiseException('Could not start ALiS component setup.');
      if Code <> 0 then RaiseException('Setup did not complete. ' + LastOutput + #13#10 + 'Logs: ' + LogDirectory);
      ProgressPage.SetProgress(6, 6);
    finally ProgressPage.Hide; end;
  except
    Result := GetExceptionMessage;
  end;
end;

procedure CurPageChanged(CurPageID: Integer);
begin
  if CurPageID = wpFinished then
    WizardForm.FinishedLabel.Caption := 'ALiS, CloudCompare and the verified ML/deep-learning runtime are ready.' + #13#10 +
      'Open CloudCompare from its ALiS shortcut. Restart any existing terminal before launching it.' + #13#10 +
      'Logs: ' + LogDirectory;
end;
