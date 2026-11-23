@echo off
setlocal
REM ===== Rust 试点构建（M0/M1，与 build_dll.bat 并列，不接 gradle）=====
REM 2026-11-23 修：原脚本 %ROOT% 从未赋值 ⇒ 最后两条 copy 实际写到盘根 "\src\..."，
REM 必然失败（|| exit /b 1），所以 orion_farfield.dll 永远部署不到 resources，
REM 仓库里留下的是一份旧 dll。这里改用 %~dp0 推导仓库根，并把硬编码的 D 盘
REM 工具链路径改成"工作区内 .toolchains 优先、PATH 兜底"，换机器也能跑。
REM
REM 本机 C 盘近满（<1GB），rustup 可安装在：<repo>\.toolchains\rustup|cargo
REM   如需全局可用：setx RUSTUP_HOME <repo>\.toolchains\rustup
REM                setx CARGO_HOME  <repo>\.toolchains\cargo

set "ROOT=%~dp0.."
set "TOOLCHAINS=%ROOT%\.toolchains"

if exist "%TOOLCHAINS%\cargo\bin\cargo.exe" (
    set "RUSTUP_HOME=%TOOLCHAINS%\rustup"
    set "CARGO_HOME=%TOOLCHAINS%\cargo"
    set "PATH=%CARGO_HOME%\bin;%PATH%"
)

where cargo >nul 2>nul || (echo [ERROR] cargo 不可用：安装 rustup，或把 rustup 装在 "%TOOLCHAINS%" 下 & exit /b 1)

cd /d "%~dp0orion-farfield" || exit /b 1
cargo build --release --target x86_64-pc-windows-msvc || exit /b 1
if not exist dist mkdir dist
copy /y "target\x86_64-pc-windows-msvc\release\orion_farfield.dll" "dist\" >nul || exit /b 1

REM 部署到两处：src（打包资源）+ build（dev 运行时实际读取，漏了这里会导致 dev 侧加载旧 DLL）
copy /y "dist\orion_farfield.dll" "%ROOT%\src\main\resources\natives\windows\orion_farfield.dll" >nul || exit /b 1
if exist "%ROOT%\build\resources\main\natives\windows" (
    copy /y "dist\orion_farfield.dll" "%ROOT%\build\resources\main\natives\windows\orion_farfield.dll" >nul
)

echo OK: orion_farfield.dll 已部署 src/main/resources + build/resources
echo 验证: java --enable-native-access=ALL-UNNAMED "%~dp0orion-farfield\bench\OrionFarFieldBench.java" "%~dp0orion-farfield\dist\orion_farfield.dll"
