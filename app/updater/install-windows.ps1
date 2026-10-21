# Instala uma versão nova do app no Windows (chamado pelo AppUpdater, ver
# appupdater.h): espera o app fechar, extrai o .zip baixado, copia os
# arquivos por cima da pasta do app e abre o app de novo.
#
# Antes de sobrescrever, cada arquivo antigo é guardado em <WorkDir>\anterior;
# se qualquer cópia falhar, os antigos voltam e o app abre na versão de antes
# com --update-failed (ele avisa e oferece o download manual). Arquivos que
# só existem na versão antiga ficam na pasta: o app pode estar numa pasta
# com outras coisas (Área de Trabalho, Downloads) e nada além do que veio no
# .zip é apagado. O andamento fica em <WorkDir>\atualizacao.log.
#
# Até a 1.4.0 o app se chamava Abelhas Petterson: o .zip traz também uma
# cópia AbelhasPetterson.exe para os atualizadores dessas versões. Ela não é
# instalada, e a da pasta do app é apagada depois de uma atualização que deu
# certo (o app passa a abrir só por BeeDuino.exe).
#
# Caminhos são usados com as funções do .NET, que não tratam [ ] como curinga.

param(
    [Parameter(Mandatory = $true)] [int] $ProcessId,
    [Parameter(Mandatory = $true)] [string] $Package,
    [Parameter(Mandatory = $true)] [string] $AppDir,
    [Parameter(Mandatory = $true)] [string] $WorkDir
)

$ErrorActionPreference = 'Stop'

$log = [System.IO.Path]::Combine($WorkDir, 'atualizacao.log')
$newDir = [System.IO.Path]::Combine($WorkDir, 'novo')
$backupDir = [System.IO.Path]::Combine($WorkDir, 'anterior')
$exe = [System.IO.Path]::Combine($AppDir, 'BeeDuino.exe')
$legacyExe = 'AbelhasPetterson.exe'

function Write-Log([string] $text) {
    $line = '{0:yyyy-MM-dd HH:mm:ss} {1}' -f (Get-Date), $text
    [System.IO.File]::AppendAllText($log, $line + "`r`n", [System.Text.Encoding]::UTF8)
}

# Um antivírus pode segurar um arquivo recém-extraído por alguns segundos.
function Copy-WithRetry([string] $from, [string] $to) {
    for ($attempt = 1; ; $attempt++) {
        try {
            [System.IO.Directory]::CreateDirectory([System.IO.Path]::GetDirectoryName($to)) | Out-Null
            [System.IO.File]::Copy($from, $to, $true)
            return
        } catch {
            if ($attempt -ge 10) { throw }
            Start-Sleep -Milliseconds 500
        }
    }
}

function Remove-Folder([string] $path) {
    if ([System.IO.Directory]::Exists($path)) {
        [System.IO.Directory]::Delete($path, $true)
    }
}

# ── Esperar o app fechar ────────────────────────────────────────────────────
Write-Log "Atualizando $AppDir com $Package"
Wait-Process -Id $ProcessId -Timeout 60 -ErrorAction SilentlyContinue
$failed = $false
$restoreFailed = $false
$copied = New-Object System.Collections.Generic.List[string]

try {
    if (Get-Process -Id $ProcessId -ErrorAction SilentlyContinue) {
        throw 'O app não fechou em 60 s.'
    }

    # ── Extrair ─────────────────────────────────────────────────────────────
    Remove-Folder $newDir
    Remove-Folder $backupDir
    Expand-Archive -LiteralPath $Package -DestinationPath $newDir -Force
    # O .zip traz uma pasta BeeDuino-<versão>-windows\ com o app dentro.
    $newExe = Get-ChildItem -LiteralPath $newDir -Recurse -File -Filter 'BeeDuino.exe' |
        Select-Object -First 1
    if (-not $newExe) {
        throw 'BeeDuino.exe não está no pacote.'
    }
    $source = $newExe.DirectoryName

    # ── Copiar, guardando os antigos ───────────────────────────────────────
    $files = Get-ChildItem -LiteralPath $source -Recurse -File
    foreach ($file in $files) {
        $relative = $file.FullName.Substring($source.Length).TrimStart('\')
        if ($relative -ieq $legacyExe) { continue }   # só para os atualizadores antigos
        $target = [System.IO.Path]::Combine($AppDir, $relative)
        if ([System.IO.File]::Exists($target)) {
            Copy-WithRetry $target ([System.IO.Path]::Combine($backupDir, $relative))
        }
        # Anotado antes de copiar: um arquivo copiado pela metade também volta.
        $copied.Add($relative)
        Copy-WithRetry $file.FullName $target
    }
    Write-Log "Pronto: $($copied.Count) arquivos copiados."

    # O nome antigo sai da pasta: dali em diante o app é aberto por BeeDuino.exe.
    $oldExe = [System.IO.Path]::Combine($AppDir, $legacyExe)
    if ([System.IO.File]::Exists($oldExe)) {
        try {
            [System.IO.File]::Delete($oldExe)
            Write-Log "$legacyExe (nome antigo) apagado."
        } catch {
            Write-Log "Não foi possível apagar ${legacyExe}: $($_.Exception.Message)"
        }
    }
} catch {
    $failed = $true
    Write-Log "Falhou: $($_.Exception.Message)"

    # ── Voltar a versão antiga ─────────────────────────────────────────────
    foreach ($relative in $copied) {
        $target = [System.IO.Path]::Combine($AppDir, $relative)
        $backup = [System.IO.Path]::Combine($backupDir, $relative)
        try {
            if ([System.IO.File]::Exists($backup)) {
                Copy-WithRetry $backup $target
            } elseif ([System.IO.File]::Exists($target)) {
                [System.IO.File]::Delete($target)   # arquivo novo, não existia antes
            }
        } catch {
            $restoreFailed = $true
            Write-Log "Não foi possível restaurar ${relative}: $($_.Exception.Message)"
        }
    }
    if ($copied.Count -gt 0) {
        Write-Log "Versão antiga restaurada ($($copied.Count) arquivos)."
    }
}

# ── Limpar e abrir o app ────────────────────────────────────────────────────
try {
    Remove-Folder $newDir
    # Os antigos ficam guardados só se não deu para pôr todos de volta.
    if (-not $restoreFailed) { Remove-Folder $backupDir }
    [System.IO.File]::Delete($Package)
} catch {
    Write-Log "Limpeza incompleta: $($_.Exception.Message)"
}

$start = New-Object System.Diagnostics.ProcessStartInfo
$start.FileName = $exe
$start.WorkingDirectory = $AppDir
$start.UseShellExecute = $true
if ($failed) { $start.Arguments = '--update-failed' }
try {
    [System.Diagnostics.Process]::Start($start) | Out-Null
} catch {
    Write-Log "Não foi possível abrir o app: $($_.Exception.Message)"
}
