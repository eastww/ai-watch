# AI Watch ESP-IDF 环境设置脚本
# 请以管理员身份运行 PowerShell

Write-Host "Setting up ESP-IDF environment..." -ForegroundColor Green

# 1. 设置ESP-IDF路径
$env:IDF_PATH = "D:\esp\esp-idf-v5.5.3"
Write-Host "IDF_PATH set to: $env:IDF_PATH"

# 2. 添加Python到PATH（如果还没有）
$pythonPath = "C:\Python311"
if (Test-Path $pythonPath) {
    $env:PATH = "$pythonPath;$pythonPath\Scripts;$env:PATH"
    Write-Host "Added Python to PATH: $pythonPath"
} else {
    Write-Warning "Python 3.11 not found in $pythonPath"
}

# 3. 添加ESP-IDF工具到PATH
$toolsPath = "$env:IDF_PATH\tools"
$env:PATH = "$toolsPath;$env:PATH"
Write-Host "Added ESP-IDF tools to PATH"

# 4. 添加GCC工具链到PATH
$gccPath = "$env:IDF_PATH\esp-idf-gcc"
if (Test-Path $gccPath) {
    $env:PATH = "$gccPath\bin;$env:PATH"
    Write-Host "Added GCC toolchain to PATH"
}

# 5. 设置其他环境变量
$env:PYTHON = "python.exe"
$env:IDF_PYTHON_ENV_PATH = "$env:IDF_PATH\.export"

Write-Host ""
Write-Host "Environment setup complete!" -ForegroundColor Green
Write-Host "Now try running:" -ForegroundColor Yellow
Write-Host "  idf.py --version" -ForegroundColor Cyan

# 6. 测试
Write-Host ""
Write-Host "Testing idf.py..." -ForegroundColor Yellow
try {
    & idf.py --version
    Write-Host "Success!" -ForegroundColor Green
}
catch {
    Write-Host "Failed to run idf.py" -ForegroundColor Red
    Write-Host "Please check if all tools are installed" -ForegroundColor Red
}