@echo off
echo ========================================
echo 请按照以下步骤操作：
echo.
echo 1. 在Keil中点击 Download (F8) 重新下载程序
echo 2. 或者按开发板的 RESET 按钮
echo 3. 准备好后，按任意键开始抓取...
echo ========================================
pause
echo.
echo 立即开始抓取 15 秒...
echo.
C:\anaconda\python.exe tools\capture_uart_60s.py --seconds 15 > temp_output.txt 2>&1
type temp_output.txt
echo.
echo ========================================
echo 检查关键信息：
echo.
findstr /C:"DIAG:" temp_output.txt
findstr /C:"p_transfer_tx" temp_output.txt
findstr /C:"SSICR=" temp_output.txt
echo.
findstr /C:"STAT,1," temp_output.txt
echo.
echo ========================================
pause
