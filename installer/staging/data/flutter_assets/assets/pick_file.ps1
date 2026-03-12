Add-Type -AssemblyName System.Windows.Forms

$dlg = New-Object System.Windows.Forms.OpenFileDialog
$dlg.Filter = 'Config/Text Files (*.txt;*.sub;*.list;*.conf;*.json)|*.txt;*.sub;*.list;*.conf;*.json|Text Files (*.txt)|*.txt|All Files (*.*)|*.*'
$dlg.Multiselect = $false
$dlg.CheckFileExists = $true
$dlg.CheckPathExists = $true
$dlg.Title = 'Select config file to import'

if ($args.Count -gt 0 -and $args[0].Length -gt 0 -and (Test-Path $args[0])) {
    $dlg.InitialDirectory = $args[0]
}

if ($dlg.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) {
    [Console]::OutputEncoding = [System.Text.Encoding]::UTF8
    Write-Output $dlg.FileName
}
