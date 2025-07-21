# ESP-IDF 环境设置脚本
# 这个脚本会设置所有必要的环境变量来使用 ESP-IDF

Write-Host "设置 ESP-IDF 环境..." -ForegroundColor Green

# 设置执行策略
Set-ExecutionPolicy -ExecutionPolicy RemoteSigned -Scope CurrentUser -Force

# 设置 ESP-IDF 路径
$env:IDF_PATH = "C:\Espressif\frameworks\esp-idf-v5.4.1"
$env:IDF_PYTHON_ENV_PATH = "C:\Espressif\python_env\idf5.4_py3.11_env"
$env:ESP_IDF_VERSION = "5.4.1"

# 添加 Python 到 PATH
$env:PATH = "C:\Espressif\python_env\idf5.4_py3.11_env\Scripts;$env:PATH"

# 添加 CMake 到 PATH
$env:PATH = "C:\Espressif\tools\cmake\3.30.2\bin;$env:PATH"

# 添加 Ninja 到 PATH
$env:PATH = "C:\Espressif\tools\ninja\1.12.1;$env:PATH"

# 添加 ESP32 工具链到 PATH
$env:PATH = "C:\Espressif\tools\xtensa-esp-elf\esp-14.2.0_20241119\xtensa-esp-elf\bin;$env:PATH"

# 添加 ESP-IDF 工具到 PATH
$env:PATH = "C:\Espressif\frameworks\esp-idf-v5.4.1\tools;$env:PATH"

# 创建 idf.py 函数
function global:idf.py { 
    python "C:\Espressif\frameworks\esp-idf-v5.4.1\tools\idf.py" @args 
}

Write-Host "ESP-IDF 环境设置完成!" -ForegroundColor Green
Write-Host "可以使用以下命令:" -ForegroundColor Yellow
Write-Host "  idf.py --version" -ForegroundColor Cyan
Write-Host "  idf.py build" -ForegroundColor Cyan
Write-Host "  idf.py flash" -ForegroundColor Cyan
Write-Host "  idf.py monitor" -ForegroundColor Cyan

# 验证环境
Write-Host "`n验证环境:" -ForegroundColor Yellow
Write-Host "Python: " -NoNewline -ForegroundColor White
python --version
Write-Host "CMake: " -NoNewline -ForegroundColor White
cmake --version | Select-Object -First 1
Write-Host "ESP-IDF: " -NoNewline -ForegroundColor White
idf.py --version
