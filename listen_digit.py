import serial
import serial.tools.list_ports
import time
import sys

def find_com_port():
    ports = serial.tools.list_ports.comports()
    for port in ports:
        # Just return the first available port for simplicity if only one is connected
        if "USB" in port.description or "CH340" in port.description or "JLink" in port.description or "CP210" in port.description:
            return port.device
    if ports:
        return ports[0].device
    return None

def main():
    print("==================================================")
    print("🎙️ RA6M5 语音识别监听端")
    print("==================================================")
    
    port_name = find_com_port()
    if not port_name:
        print("❌ 未检测到可用的 COM 端口，请检查 USB 连接！")
        return
        
    print(f"🔌 正在连接端口: {port_name} (115200 波特率)...")
    
    try:
        ser = serial.Serial(port_name, 115200, timeout=1)
        print("✅ 连接成功！请对着麦克风说话...\n")
        
        while True:
            if ser.in_waiting:
                raw_line = ser.readline()
                try:
                    line = raw_line.decode('utf-8', errors='ignore').strip()
                    print(f"RAW: {line}") # Print everything for debugging
                    
                    # Filter what the user sees
                    if "Recognized Digit:" in line:
                        try:
                            parts = line.split("Recognized Digit: ")[1].split(" ")
                            digit = parts[0]
                            conf = parts[1].replace("(Conf:", "").replace(")", "").replace("<", "").strip()
                            
                            print("\n✨✨✨✨✨✨✨✨✨✨✨✨✨✨✨✨✨")
                            print(f"   🤖 识别到数字: 【 {digit} 】  (把握度: {conf})")
                            print("✨✨✨✨✨✨✨✨✨✨✨✨✨✨✨✨✨\n")
                        except:
                            pass
                except Exception as e:
                    print(f"Decode error: {e}")
            
            time.sleep(0.01)
            
    except serial.SerialException as e:
        print(f"❌ 串口打开失败（可能被其他程序占用了）: {e}")
    except KeyboardInterrupt:
        print("\n👋 停止监听。")
        if 'ser' in locals() and ser.is_open:
            ser.close()

if __name__ == "__main__":
    main()
