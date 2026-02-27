$ErrorActionPreference = 'Continue'
$root   = 'C:\Users\cesco\Documents\smt32config\DaisySeed'
$dfu    = 'C:\Espressif\tools\dfu-util\0.11\dfu-util-0.11-win64\dfu-util.exe'
$boot   = 'C:\Users\cesco\Documents\smt32config\DaisySeed\libdaisy\core\dsy_bootloader_v6_4-intdfu-2000ms.bin'
$fw     = 'C:\Users\cesco\Documents\smt32config\DaisySeed\build\DrumMachine.bin'
$gccBin = 'C:\ST\STM32CubeIDE_2.0.0\STM32CubeIDE\plugins\com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.13.3.rel1.win32_1.0.100.202509120712\tools\bin'
$makeBin= 'C:\ST\STM32CubeIDE_2.0.0\STM32CubeIDE\plugins\com.st.stm32cube.ide.mcu.externaltools.make.win32_2.2.0.202409170845\tools\bin'
$log    = 'C:\Users\cesco\Documents\smt32config\flash_daisy_script_log.txt'

$env:PATH = "$gccBin;$makeBin;$env:PATH"
Set-Location $root

"=== FLASH DAISY $(Get-Date -Format s) ===" | Out-File -FilePath $log -Encoding utf8
Write-Host 'PASO 1/2: Presiona BOOT + RESET en la Daisy (ROM DFU)...' -ForegroundColor Yellow

$found = $false
for($i = 0; $i -lt 240; $i++) {
    $list = & $dfu -l 2>&1 | Out-String
    if($list -match 'df11') {
        $found = $true
        "DFU detectado en t=$($i/2)s" | Tee-Object -FilePath $log -Append
        break
    }
    if($i % 20 -eq 0) { Write-Host "[$($i/2)s] esperando DFU..." -ForegroundColor DarkGray }
    Start-Sleep -Milliseconds 500
}

if(-not $found) {
    'RESULT=TIMEOUT_DFU' | Tee-Object -FilePath $log -Append
    Write-Host 'TIMEOUT: no se detecto DFU' -ForegroundColor Red
    exit 1
}

Write-Host 'Flasheando bootloader interno...' -ForegroundColor Cyan
$resBoot = & $dfu -a 0 -s 0x08000000:leave -D $boot -d ",0483:df11" 2>&1 | Tee-Object -FilePath $log -Append | Out-String
if($resBoot -notmatch 'Download done') {
    'RESULT=BOOT_FLASH_FAIL' | Tee-Object -FilePath $log -Append
    Write-Host 'Fallo al flashear bootloader interno' -ForegroundColor Red
    exit 2
}

Write-Host 'PASO 2/2: Desconecta y reconecta USB SIN botones (bootloader DFU)...' -ForegroundColor Yellow
$found2 = $false
for($i = 0; $i -lt 600; $i++) {
    $list = & $dfu -l 2>&1 | Out-String
    if($list -match 'df11') {
        $found2 = $true
        "DFU bootloader detectado en t=$($i/2)s" | Tee-Object -FilePath $log -Append
        break
    }
    if($i % 20 -eq 0) { Write-Host "[$($i/2)s] esperando reconexion..." -ForegroundColor DarkGray }
    Start-Sleep -Milliseconds 500
}

if(-not $found2) {
    'RESULT=TIMEOUT_DFU_BOOTLOADER' | Tee-Object -FilePath $log -Append
    Write-Host 'TIMEOUT: no aparecio DFU del bootloader' -ForegroundColor Red
    exit 3
}

Write-Host 'Flasheando firmware app (QSPI)...' -ForegroundColor Cyan
$resApp = & $dfu -a 0 -s 0x90040000:leave -D $fw -d ",0483:df11" 2>&1 | Tee-Object -FilePath $log -Append | Out-String

if($resApp -match 'Download done') {
    'RESULT=FLASH_OK' | Tee-Object -FilePath $log -Append
    Write-Host 'FLASH_OK' -ForegroundColor Green
    exit 0
} else {
    'RESULT=FLASH_FAIL' | Tee-Object -FilePath $log -Append
    Write-Host 'FLASH_FAIL' -ForegroundColor Red
    exit 4
}
