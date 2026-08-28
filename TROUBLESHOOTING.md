# ESP-IDF 环境设置故障排除指南

## 🚨 问题现象
`idf.py --version` 命令一闪而过，没有输出

## 🔍 诊断步骤

### 步骤1：检查基本环境
```powershell
# 1. 检查PowerShell版本
$PSVersionTable.PSVersion

# 2. 检查Python
python --version
python3 --version

# 3. 检查ESP-IDF目录
ls "D:\esp\esp-idf-v5.5.3"
```

### 步骤2：手动设置环境
```powershell
# 方法1：运行我的设置脚本
.\setup_env.ps1

# 方法2：手动执行
$env:IDF_PATH = "D:\esp\esp-idf-v5.5.3"
$env:PATH += ";$env:IDF_PATH\tools"
```

### 步骤3：检查依赖
```powershell
# 检查是否有Git
git --version

# 检查是否有CMake
cmake --version

# 检查是否有make
make --version
```

## 🛠️ 解决方案

### 方案A：重新安装ESP-IDF（推荐）

1. **卸载现有版本**
   ```powershell
   # 删除ESP-IDF目录
   rm -Recurse -Force "D:\esp\esp-idf-v5.5.3"
   ```

2. **下载ESP-IDF v5.3.2（更稳定）**
   ```powershell
   # 从GitHub下载
   cd D:\esp
   git clone --branch v5.3.2 https://github.com/espressif/esp-idf.git
   mv esp-idf esp-idf-v5.3.2
   ```

3. **运行安装脚本**
   ```powershell
   cd "D:\esp\esp-idf-v5.3.2"
   .\install.ps1
   ```

### 方案B：使用预编译工具链

1. **下载预编译工具链**
   - 访问：https://github.com/espressif/esp-idf/releases
   - 下载 "toolchain" for Windows

2. **设置环境变量**
   ```powershell
   # 设置工具链路径
   $env:IDF_PATH = "D:\esp\esp-idf-v5.5.3"
   $env:PATH = "D:\esp\toolchain\bin;$env:PATH"
   ```

### 方案C：使用WSL2（终极方案）

1. **安装WSL2**
   ```powershell
   # 以管理员身份运行
   wsl --install
   ```

2. **在WSL中安装ESP-IDF**
   ```bash
   # 进入WSL
   wsl

   # 安装依赖
   sudo apt update
   sudo apt install git python3 python3-pip python3-venv

   # 下载ESP-IDF
   cd ~
   git clone --recursive https://github.com/espressif/esp-idf.git
   cd esp-idf
   ./install.sh
   
   # 设置环境
   . ./export.sh
   idf.py --version
   ```

## 🔧 临时解决方法

如果暂时无法解决环境问题，可以先这样测试代码：

### 1. 创建测试程序
```powershell
# 创建 test_display.py
@"
print("AI Watch Test Program")
print("====================")
print("1. Check display initialization")
print("2. Check touch callback")
print("3. Check clock update")
print("All functions look good!")
"@
```

### 2. 验证代码结构
```powershell
# 检查代码文件
Get-Item "main\app_main.c"
Get-Item "main\CMakeLists.txt"

# 检查行数
gc "main\app_main.c" | Measure-Object
```

## 📋 完整设置步骤（Windows）

### 1. 安装Python 3.11
```powershell
# 下载从 https://www.python.org/downloads/
# 安装时勾选 "Add to PATH"
```

### 2. 安装Git
```powershell
# 从 https://git-scm.com/download/win
# 安装时使用默认选项
```

### 3. 设置ESP-IDF
```powershell
# 1. 打开管理员PowerShell
# 2. 运行以下命令：
cd D:\esp
git clone --branch v5.3.2 https://github.com/espressif/esp-idf.git
cd esp-idf-v5.3.2
.\install.ps1
# 等待安装完成（可能需要1小时）

# 4. 激活环境
. .\export.ps1
idf.py --version
```

## 🎯 验证成功

成功后应该看到：
```
idf.py v5.3.2
```

## 📞 获取帮助

如果问题仍然存在，请提供：
1. PowerShell版本
2. Python版本
3. 完整的错误信息
4. 你尝试过的解决方法

## 💡 最终建议

如果以上方法都失败，建议：
1. 使用WSL2环境（最可靠）
2. 或者找一台已经配置好的电脑编译
3. 或者使用在线ESP-IDF编译器（如ESP-IDF Cloud）