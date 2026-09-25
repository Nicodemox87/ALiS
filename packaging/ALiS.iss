#define ProductName "ALiS - Archaeological LiDAR Studio"
#define ProductVersion "0.1.0-alpha.5.6"
#define ProductPublisher "Dott. Nicodemo Abate — CNR-ISPC"

[Setup]
AppId={{C46C8898-7B82-4BDE-AC15-B4A2EB318A31}
AppName={#ProductName}
AppVersion={#ProductVersion}
AppVerName={#ProductName} {#ProductVersion}
AppPublisher={#ProductPublisher}
DefaultDirName={autopf}\CloudCompare
DefaultGroupName=ALiS
DisableProgramGroupPage=yes
PrivilegesRequired=admin
PrivilegesRequiredOverridesAllowed=dialog commandline
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
DisableWelcomePage=no
WizardImageFile=stage\ALiS-wizard.bmp
WizardImageStretch=no
SetupIconFile=stage\ALiS.ico
UninstallDisplayIcon={app}\ALiS.ico
OutputDir=..\output\release
OutputBaseFilename=ALiS-{#ProductVersion}-CloudCompare-2.13.2-Windows-x64-Setup
LicenseFile=stage\LICENSE.txt
InfoBeforeFile=stage\INSTALL.txt

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "italian"; MessagesFile: "compiler:Languages\Italian.isl"

[Tasks]
Name: "desktopicon"; Description: "Create an ALiS desktop shortcut"; GroupDescription: "Shortcuts:"; Flags: checkedonce

[Files]
Source: "stage\ALiS.ico"; DestDir: "{app}"; Flags: ignoreversion
Source: "stage\plugins\ALIS_PLUGIN.dll"; DestDir: "{app}\plugins"; Flags: ignoreversion
Source: "stage\worker\ALiS\*"; DestDir: "{app}\worker\ALiS"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "stage\doc\ALiS\*"; DestDir: "{app}\doc\ALiS"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "stage\ALiS Launcher.cmd"; DestDir: "{app}"; Flags: ignoreversion
Source: "stage\TESTING_NOTICE.txt"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\ALiS - Archaeological LiDAR Studio"; Filename: "{app}\ALiS Launcher.cmd"; WorkingDir: "{app}"; IconFilename: "{app}\ALiS.ico"
Name: "{autodesktop}\ALiS - Archaeological LiDAR Studio"; Filename: "{app}\ALiS Launcher.cmd"; WorkingDir: "{app}"; IconFilename: "{app}\ALiS.ico"; Tasks: desktopicon
Name: "{autoprograms}\ALiS Getting Started"; Filename: "{app}\doc\ALiS\GETTING_STARTED.txt"

[Run]
Filename: "{app}\ALiS Launcher.cmd"; Description: "Launch ALiS in CloudCompare"; Flags: nowait postinstall skipifsilent

[Code]
function NextButtonClick(CurPageID: Integer): Boolean;
var
  TargetDir: String;
  ChangeLogLines: TArrayOfString;
  I: Integer;
  StableReleaseFound: Boolean;
begin
  Result := True;
  if CurPageID = wpSelectDir then
  begin
    TargetDir := WizardDirValue;
    if not FileExists(AddBackslash(TargetDir) + 'CloudCompare.exe') then
    begin
      MsgBox('Select the root directory of an existing CloudCompare 2.13.2 x64 installation (the directory that contains CloudCompare.exe).', mbError, MB_OK);
      Result := False;
      exit;
    end;
    if (not FileExists(AddBackslash(TargetDir) + 'Qt5Core.dll')) and
       (not FileExists(AddBackslash(TargetDir) + 'Qt5Core_conda.dll')) then
    begin
      MsgBox('This package targets the stable Qt 5 build of CloudCompare 2.13.2. Qt5Core.dll was not found in the selected directory.', mbError, MB_OK);
      Result := False;
      exit;
    end;
    StableReleaseFound := False;
    if LoadStringsFromFile(AddBackslash(TargetDir) + 'CHANGELOG.md', ChangeLogLines) then
      for I := 0 to GetArrayLength(ChangeLogLines) - 1 do
      begin
        if (I <= 8) and (Pos('v2.13.2', ChangeLogLines[I]) = 1) then
          StableReleaseFound := True;
      end;
    if not StableReleaseFound then
    begin
      MsgBox('This ALiS build requires CloudCompare 2.13.2 stable. The selected folder appears to contain a different release (for example 2.14 alpha), which can trigger Qt/ABI plugin alerts. Select the 2.13.2 stable folder.', mbError, MB_OK);
      Result := False;
      exit;
    end;
  end;
end;
