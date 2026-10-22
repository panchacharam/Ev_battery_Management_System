from machine import Pin, SoftI2C, ADC, UART
from ssd1306 import SSD1306_I2C
import dht
import time
import struct
import math
import sys

# ============================================================
# PIN CONFIGURATION  (VERIFIED — DO NOT CHANGE WITHOUT REASON)
# ============================================================

# I2C
SDA_PIN = 0
SCL_PIN = 1

# DHT11
DHT_PIN = 15

# Analog sensors
VOLTAGE_PIN = 26
CURRENT_PIN = 27

# CLR / reset button (clears a latched fault)
RESET_BTN_PIN = 16

# UART to ESP32 (Phase 3 — verified working at 115200 baud)
UART_TX_PIN = 4
UART_RX_PIN = 5
UART_BAUDRATE = 115200


# ============================================================
# CALIBRATION MODE
# ============================================================
# Set this True temporarily whenever you rewire or move sensors.
# It skips all Phase 2/3 logic and the OLED, and just prints raw
# averaged ADC counts for both sensors every second — use this to
# capture a fresh CURRENT_ZERO_RAW and to sanity-check the voltage
# reading before trusting any of the derived values below.

CALIBRATION_MODE = False


# ============================================================
# ADC CONFIGURATION
# ============================================================

ADC_REFERENCE = 3.3


# ============================================================
# VOLTAGE SENSOR
# ============================================================

VOLTAGE_DIVIDER_RATIO = 4.86
VOLTAGE_CALIBRATION = 1.0


# ============================================================
# ACS712 CURRENT SENSOR
# ============================================================

ACS_SENSITIVITY = 0.100     # V/A, ACS712-20A nominal

# Zero-offset point under the corrected wiring (battery -> ACS712 -> VSYS)
CURRENT_ZERO_RAW = 51232

# Updated per further hardware testing (was 1.0) — keeping as given.
CURRENT_CALIBRATION = 0.05

CURRENT_ZERO_VOLTAGE = (
    CURRENT_ZERO_RAW * ADC_REFERENCE / 65535
)

CURRENT_DEADBAND = 0.01


# ============================================================
# PHASE 2 — BATTERY / THRESHOLD CONFIGURATION
# ============================================================

BATTERY_MAX_V = 4.2
BATTERY_MIN_V = 3.0
BATTERY_LOW_V = 3.3
BATTERY_CRITICAL_V = 3.1

BATTERY_DISCONNECTED_THRESHOLD_V = 0.5

OVER_CURRENT_THRESHOLD = 1.0     # A — set to your expected max draw
TEMP_WARNING_C = 45
TEMP_CRITICAL_C = 60
MOTION_THRESHOLD_G = 0.15


# ============================================================
# I2C SETUP
# ============================================================

i2c = SoftI2C(
    scl=Pin(SCL_PIN),
    sda=Pin(SDA_PIN),
    freq=10000
)

print("I2C devices:", i2c.scan())


# ============================================================
# OLED SETUP
# ============================================================

oled = SSD1306_I2C(
    128,
    64,
    i2c,
    addr=0x3C
)

print("OLED initialized")


# ============================================================
# MPU6050 SETUP
# ============================================================

MPU_ADDR = 0x68

i2c.writeto_mem(
    MPU_ADDR,
    0x6B,
    b'\x00'
)

time.sleep_ms(100)

print("MPU6050 initialized")


# ============================================================
# DHT11 SETUP
# ============================================================

dht_sensor = dht.DHT11(
    Pin(DHT_PIN)
)

print("DHT11 initialized")


# ============================================================
# ADC SETUP
# ============================================================

voltage_adc = ADC(
    Pin(VOLTAGE_PIN)
)

current_adc = ADC(
    Pin(CURRENT_PIN)
)


# ============================================================
# RESET BUTTON SETUP
# ============================================================

reset_btn = Pin(RESET_BTN_PIN, Pin.IN, Pin.PULL_UP)


# ============================================================
# UART SETUP (Phase 3 — Pico to ESP32)
# ============================================================

uart = UART(
    1,
    baudrate=UART_BAUDRATE,
    tx=Pin(UART_TX_PIN),
    rx=Pin(UART_RX_PIN)
)

print("UART initialized")


# ============================================================
# ADC AVERAGING
# ============================================================

def read_adc_average(adc, samples=20):
    total = 0
    for i in range(samples):
        total += adc.read_u16()
        time.sleep_ms(2)
    return total // samples


# ============================================================
# BATTERY VOLTAGE
# ============================================================

def read_battery_voltage():
    raw = read_adc_average(voltage_adc, 20)
    adc_voltage = raw * ADC_REFERENCE / 65535
    battery_voltage = adc_voltage * VOLTAGE_DIVIDER_RATIO
    battery_voltage *= VOLTAGE_CALIBRATION
    return raw, adc_voltage, battery_voltage


# ============================================================
# ACS712 CURRENT
# ============================================================

def read_current():
    raw = read_adc_average(current_adc, 30)
    sensor_voltage = raw * ADC_REFERENCE / 65535
    voltage_difference = sensor_voltage - CURRENT_ZERO_VOLTAGE
    current = voltage_difference / ACS_SENSITIVITY
    current *= CURRENT_CALIBRATION
    if abs(current) < CURRENT_DEADBAND:
        current = 0.0
    return raw, sensor_voltage, current


# ============================================================
# MPU6050
# ============================================================

def read_acceleration():
    data = i2c.readfrom_mem(MPU_ADDR, 0x3B, 6)
    ax_raw, ay_raw, az_raw = struct.unpack(">hhh", data)
    ax = ax_raw / 16384.0
    ay = ay_raw / 16384.0
    az = az_raw / 16384.0
    return ax, ay, az


# ============================================================
# DHT11
# ============================================================

def read_dht():
    try:
        dht_sensor.measure()
        temperature = dht_sensor.temperature()
        humidity = dht_sensor.humidity()
        return temperature, humidity
    except Exception as e:
        print("DHT ERROR:", e)
        return None, None


# ============================================================
# PHASE 2 — BATTERY PERCENTAGE
# ============================================================

def battery_percentage(voltage):
    pct = (voltage - BATTERY_MIN_V) / (BATTERY_MAX_V - BATTERY_MIN_V) * 100
    if pct < 0:
        pct = 0
    if pct > 100:
        pct = 100
    return pct


# ============================================================
# PHASE 2 — STATUS FUNCTIONS
# ============================================================

def battery_status(voltage):
    if voltage <= BATTERY_CRITICAL_V:
        return "CRITICAL"
    elif voltage <= BATTERY_LOW_V:
        return "LOW"
    return "NORMAL"


def current_status(current):
    if abs(current) >= OVER_CURRENT_THRESHOLD:
        return "OVER-I"
    return "NORMAL"


def temperature_status(temperature):
    if temperature is None:
        return "UNKNOWN"
    if temperature >= TEMP_CRITICAL_C:
        return "CRITICAL"
    elif temperature >= TEMP_WARNING_C:
        return "WARNING"
    return "NORMAL"


def motion_deviation(ax, ay, az):
    mag = math.sqrt(ax * ax + ay * ay + az * az)
    return abs(mag - 1.0)


def motion_status(ax, ay, az):
    if motion_deviation(ax, ay, az) >= MOTION_THRESHOLD_G:
        return "MOTION"
    return "STILL"


def overall_fault(batt_stat, curr_stat, temp_stat, battery_disconnected):
    if battery_disconnected:
        return (
            curr_stat == "OVER-I"
            or temp_stat == "CRITICAL"
        )
    return (
        batt_stat == "CRITICAL"
        or curr_stat == "OVER-I"
        or temp_stat == "CRITICAL"
    )


# ============================================================
# CALIBRATION MODE LOOP
# ============================================================

if CALIBRATION_MODE:
    print("=== CALIBRATION MODE ===")
    print("Printing raw averaged ADC values every second.")

    while True:
        v_raw, v_adc, v_batt = read_battery_voltage()
        c_raw, c_v, c_a = read_current()

        print("--------------------------------")
        print("Voltage raw:", v_raw, " ADC V:", round(v_adc, 4),
              " -> battery (old ratio):", round(v_batt, 2), "V")
        print("Current raw:", c_raw, " sensor V:", round(c_v, 4))

        time.sleep(1)


# ============================================================
# STARTUP SCREEN
# ============================================================

oled.fill(0)
oled.text("EV BATTERY", 0, 0)
oled.text("MONITORING", 0, 12)
oled.text("SYSTEM START", 0, 28)
oled.show()

time.sleep(2)


# ============================================================
# MAIN LOOP
# ============================================================

fault_latched = False

while True:

    try:
        ax, ay, az = read_acceleration()
        temperature, humidity = read_dht()
        voltage_raw, adc_voltage, battery_voltage = read_battery_voltage()
        current_raw, current_sensor_voltage, current = read_current()

        # ---- Battery disconnected check ----
        battery_disconnected = battery_voltage < BATTERY_DISCONNECTED_THRESHOLD_V

        if battery_disconnected:
            battery_voltage_display = 0.0
            batt_pct = 0
            batt_stat = "DISCONNECTED"
        else:
            battery_voltage_display = battery_voltage
            batt_pct = battery_percentage(battery_voltage)
            batt_stat = battery_status(battery_voltage)

        # ---- Phase 2 derived values ----
        curr_stat = current_status(current)
        temp_stat = temperature_status(temperature)
        move_stat = motion_status(ax, ay, az)
        new_fault = overall_fault(batt_stat, curr_stat, temp_stat, battery_disconnected)

        # Latch: once a fault occurs, it stays until the button clears it
        if new_fault:
            fault_latched = True

        # Reset button is active-low (pull-up): 0 means pressed
        if reset_btn.value() == 0:
            fault_latched = False

        # ====================================================
        # SERIAL MONITOR
        # ====================================================

        print("--------------------------------")

        if temperature is not None:
            print("Temperature:", temperature, "C  ->", temp_stat)
            print("Humidity:", humidity, "%")
        else:
            print("DHT ERROR")

        if battery_disconnected:
            print("BATTERY: DISCONNECTED")
        else:
            print("Battery Voltage:", round(battery_voltage_display, 2), "V  ->", batt_stat)
            print("Battery Percentage:", round(batt_pct, 1), "%")
        print("Current:", round(current, 3), "A  ->", curr_stat)
        print(
            "AX:", round(ax, 2),
            "AY:", round(ay, 2),
            "AZ:", round(az, 2),
            " ->", move_stat
        )
        print("Voltage ADC:", voltage_raw)
        print("Current ADC:", current_raw)
        print("LATCHED FAULT:", "YES" if fault_latched else "no")

        # ====================================================
        # PHASE 3 — UART TRANSMIT TO ESP32
        # ====================================================
        # Uses the exact same values just computed/printed above —
        # nothing is recalculated or hard-coded for the packet.

        # A DHT read failure (temperature is None) still needs a
        # numeric field for the ESP32 parser, so send a clear -1
        # sentinel in that case only; the Pico's own display/serial
        # output above is unaffected and still shows "DHT ERROR".
        if temperature is not None:
            uart_temp = temperature
            uart_hum = humidity
        else:
            uart_temp = -1
            uart_hum = -1

        overall_fault_flag = 1 if fault_latched else 0

        packet = "DATA,{:.2f},{:.1f},{:.3f},{},{},{:.2f},{:.2f},{:.2f},{},{},{}\n".format(
            battery_voltage_display,
            batt_pct,
            current,
            uart_temp,
            uart_hum,
            ax,
            ay,
            az,
            move_stat,
            batt_stat,
            overall_fault_flag
        )

        uart.write(packet)

        print("UART SENT:")
        print(packet)

        # ====================================================
        # OLED
        # ====================================================

        oled.fill(0)
        oled.text("EV BATTERY", 0, 0)

        if temperature is not None:
            oled.text("T:{}C H:{}%".format(temperature, humidity), 0, 10)
        else:
            oled.text("DHT ERROR", 0, 10)

        if battery_disconnected:
            oled.text("BATTERY:", 0, 20)
            oled.text("DISCONNECTED", 0, 30)
        else:
            oled.text(
                "V:{:.2f}V I:{:.3f}A".format(battery_voltage_display, current),
                0,
                20
            )

            oled.text(
                "Batt:{:.0f}% {}".format(batt_pct, batt_stat),
                0,
                30
            )

        oled.text(
            "I:{} T:{}".format(curr_stat, temp_stat),
            0,
            40
        )

        if fault_latched:
            oled.text("FAULT! PRESS CLR", 0, 50)
        else:
            oled.text("Mov:{}".format(move_stat), 0, 50)

        oled.show()

        time.sleep(2)

    except Exception as e:
        print("--------------------------------")
        print("SYSTEM ERROR:")
        sys.print_exception(e)
        print("--------------------------------")
        time.sleep(2)
