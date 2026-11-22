"""Add the persistent task-history layout to the FormaView Inno template."""


def configure_task_history(template: str) -> str:
    def insert(before: str, text: str) -> None:
        nonlocal template
        if template.count(before) != 1:
            raise ValueError(f"Expected one FormaView installer template marker: {before}")
        template = template.replace(before, text + before, 1)

    insert("[Code]\n", """[Registry]
; User task history survives uninstall. The first successful setup selects a
; layout; later upgrades and reinstalls retain it.
Root: HKCU64; Subkey: "Software\\FormaView\\Installer"; ValueType: string; ValueName: "TaskHistoryLayout"; ValueData: "{code:InitialTaskHistoryLayout}"; Flags: createvalueifdoesntexist

""")
    insert("function GetFileAttributesW(", """var
  LegacyTaskHistoryUpgrade: Boolean;

function InitialTaskHistoryLayout(Param: string): string;
begin
  if LegacyTaskHistoryUpgrade then Result := 'legacy'
  else Result := 'installed';
end;

""")
    insert("  if HasContents(Root) and not Existing then begin",
           "  LegacyTaskHistoryUpgrade := Existing and (PreviousPath <> '');\n")
    return template
