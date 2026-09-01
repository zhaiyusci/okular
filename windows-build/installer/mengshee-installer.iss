#define AppName "Mengshee"
#define AppIconFile AddBackslash(SourcePath) + "..\..\icons\mengshee.ico"
#define MengsheeVersionFile AddBackslash(SourcePath) + "..\..\VERSION.txt"
#define MengsheeVersionHandle FileOpen(MengsheeVersionFile)
#define MengsheeVersionFromFile Trim(FileRead(MengsheeVersionHandle))
#expr FileClose(MengsheeVersionHandle)
#define AppVersion GetEnv("MENGSHEE_VERSION")
#if AppVersion == ""
#define AppVersion MengsheeVersionFromFile
#endif
#define FileVersion GetEnv("MENGSHEE_FILE_VERSION")
#if FileVersion == ""
#define FileVersion MengsheeVersionFromFile
#endif
#define SourceDir GetEnv("MENGSHEE_STAGE")
#if SourceDir == ""
#define SourceDir "..\..\..\dist\mengshee-pdf\app"
#endif
#define StemTeXSupportDir GetEnv("MENGSHEE_STEMTEX_SUPPORT_STAGE")
#if StemTeXSupportDir == ""
#define StemTeXSupportDir "..\..\..\dist\mengshee-pdf\optional\stemtex-support"
#endif
#define OutputDir GetEnv("MENGSHEE_OUTPUT")
#if OutputDir == ""
#define OutputDir "..\..\..\dist"
#endif

[Setup]
AppId={{06A28C09-9BB5-47D0-8F43-24BC9019C8E4}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher=Yu Zhai
VersionInfoVersion={#FileVersion}
VersionInfoProductVersion={#FileVersion}
VersionInfoProductName={#AppName}
VersionInfoDescription={#AppName} Setup
VersionInfoOriginalFileName=Mengshee-{#AppVersion}-Setup.exe
DefaultDirName={autopf}\Mengshee
DefaultGroupName=Mengshee
DisableDirPage=no
DisableProgramGroupPage=yes
OutputDir={#OutputDir}
OutputBaseFilename=Mengshee-{#AppVersion}-Setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
SetupIconFile={#AppIconFile}
ArchitecturesAllowed=x64
ArchitecturesInstallIn64BitMode=x64
UninstallDisplayIcon={app}\bin\mengshee.exe
ChangesAssociations=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Types]
Name: "full"; Description: "Recommended (with bundled StemTeX TeX tree)"
Name: "compact"; Description: "Compact (use an external TeX tree)"
Name: "custom"; Description: "Custom installation"; Flags: iscustom

[Components]
Name: "core"; Description: "Mengshee application"; Types: full compact custom; Flags: fixed
Name: "stemtexsupport"; Description: "Bundled StemTeX TeX tree"; Types: full

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked
Name: "associatepdf"; Description: "Associate .pdf files with Mengshee"; GroupDescription: "File associations:"

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Components: core; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#StemTeXSupportDir}\*"; DestDir: "{app}"; Components: stemtexsupport; Flags: ignoreversion recursesubdirs createallsubdirs

[InstallDelete]
Type: filesandordirs; Name: "{app}\StemTeX\runtime\texmf-dist"

[Icons]
Name: "{group}\Mengshee"; Filename: "{app}\bin\mengshee.exe"; IconFilename: "{app}\bin\mengshee.ico"
Name: "{autodesktop}\Mengshee"; Filename: "{app}\bin\mengshee.exe"; IconFilename: "{app}\bin\mengshee.ico"; Tasks: desktopicon

[Registry]
Root: HKCR; Subkey: ".pdf"; ValueType: string; ValueName: ""; ValueData: "Mengshee.Document"; Flags: uninsdeletevalue; Tasks: associatepdf
Root: HKCR; Subkey: "Mengshee.Document"; ValueType: string; ValueName: ""; ValueData: "PDF Document"; Flags: uninsdeletekey; Tasks: associatepdf
Root: HKCR; Subkey: "Mengshee.Document\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\bin\application-pdf.ico"; Tasks: associatepdf
Root: HKCR; Subkey: "Mengshee.Document\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\bin\mengshee.exe"" ""%1"""; Tasks: associatepdf

[Run]
Filename: "{tmp}\vc_redist.x64.exe"; Parameters: "/install /quiet /norestart"; StatusMsg: "Installing Microsoft Visual C++ Runtime..."; Flags: waituntilterminated runhidden; Check: NeedsMsvcRuntime
Filename: "{app}\bin\mengshee.exe"; Description: "Launch Mengshee"; Flags: nowait postinstall skipifsilent

[Code]
var
  DownloadPage: TDownloadWizardPage;

function NeedsMsvcRuntime: Boolean;
var
  Installed: Cardinal;
begin
  Result := not (RegQueryDWordValue(HKLM64, 'SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64', 'Installed', Installed) and (Installed = 1));
end;

procedure InitializeWizard;
begin
  DownloadPage := CreateDownloadPage(SetupMessage(msgWizardPreparing), SetupMessage(msgPreparingDesc), nil);
  DownloadPage.ShowBaseNameInsteadOfUrl := True;
end;

function NextButtonClick(CurPageID: Integer): Boolean;
var
  Error: String;
begin
  Result := True;
  if (CurPageID = wpReady) and NeedsMsvcRuntime then begin
    DownloadPage.Clear;
    DownloadPage.Add('https://aka.ms/vs/17/release/vc_redist.x64.exe', 'vc_redist.x64.exe', '');
    DownloadPage.Show;
    try
      try
        DownloadPage.Download;
      except
        if DownloadPage.AbortedByUser then
          Log('Microsoft Visual C++ Runtime download was aborted by user.')
        else begin
          Error := Format('%s: %s', [DownloadPage.LastBaseNameOrUrl, GetExceptionMessage]);
          SuppressibleMsgBox(AddPeriod(Error), mbCriticalError, MB_OK, IDOK);
        end;
        Result := False;
      end;
    finally
      DownloadPage.Hide;
    end;
  end;
end;
