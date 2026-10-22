from machine import Pin, SoftI2C, ADC
from ssd1306 import SSD1306_I2C
import dht
import time
import struct
import math


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


# ============================================================
# ADC CONFIGURATION
# ============================================================

ADC_REFERENCE = 3.3


# ============================================================
# VOLTAGE SENSOR  (VERIFIED CALIBRATION)
# ============================================================

VOLTAGE_DIVIDER_RATIO = 5.0
VOLTAGE_CALIBRATION = 1.0


# ============================================================
# ACS712 CURRENT SENSOR  (VERIFIED CALIBRATION)
# ============================================================

ACS_SENSITIVITY = 0.100
CURRENT_ZERO_RAW = 34730
CURRENT_CALIBRATION = 0.004


CURRENT_ZERO_VOLTAGE = (
    CURRENT_ZERO_RAW * ADC_REFERENCE / 65535
)

CURRENT_DEADBAND = 0.01


# ============================================================
# PHASE 2 — BATTERY / THRESHOLD CONFIGURATION
# ============================================================
# NOTE: These are placeholder values not present in your verified
# Phase 1 data. Adjust them to match your actual battery datasheet
# and expected load before treating the status outputs as trustworthy.

# Assuming a single-cell Li-ion pack (nominal 3.7V).
# Change these if your pack is a different chemistry / cell count.
BATTERY_MAX_V = 4.2        # 100%
BATTERY_MIN_V = 3.0        # 0%
BATTERY_LOW_V = 3.3        # LOW BATTERY warning threshold
BATTERY_CRITICAL_V = 3.1   # CRITICAL warning threshold

# Over-current threshold (A) — set to your expected max load current
OVER_CURRENT_THRESHOLD = 1.0

# Temperature thresholds (deg C) — typical Li-ion safe-operating guidance
TEMP_WARNING_C = 45
TEMP_CRITICAL_C = 60

# Motion/vibration threshold, in g, as deviation from the resting 1g vector
MOTION_THRESHOLD_G = 0.15


# ============================================================
# I2C SETUP
# ============================================================

i2c = SoftI2C(
    scl=Pin(SCL_PIN),
    sda=Pin(SDA_PIN),
    freq=100000
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
    # Default ±2g range, 16384 LSB = 1g
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
    return abs(mag - 1.0)  # deviation from resting 1g


def motion_status(ax, ay, az):
    if motion_deviation(ax, ay, az) >= MOTION_THRESHOLD_G:
        return "MOTION"
    return "STILL"


def overall_fault(batt_stat, curr_stat, temp_stat):
    return (
        batt_stat == "CRITICAL"
        or curr_stat == "OVER-I"
        or temp_stat == "CRITICAL"
    )


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

while True:

    try:
        ax, ay, az = read_acceleration()
        temperature, humidity = read_dht()
        voltage_raw, adc_voltage, battery_voltage = read_battery_voltage()
        current_raw, current_sensor_voltage, current = read_current()

        # ---- Phase 2 derived values ----
        batt_pct = battery_percentage(battery_voltage)
        batt_stat = battery_status(battery_voltage)
        curr_stat = current_status(current)
        temp_stat = temperature_status(temperature)
        move_stat = motion_status(ax, ay, az)
        fault = overall_fault(batt_stat, curr_stat, temp_stat)

        # ====================================================
        # SERIAL MONITOR
        # ====================================================

        print("--------------------------------")

        if temperature is not None:
            print("Temperature:", temperature, "C  ->", temp_stat)
            print("Humidity:", humidity, "%")
        else:
            print("DHT ERROR")

        print("Battery Voltage:", round(battery_voltage, 2), "V  ->", batt_stat)
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
        print("OVERALL FAULT:", "YES" if fault else "no")

        # ====================================================
        # OLED
        # ====================================================

        oled.fill(0)
        oled.text("EV BATTERY", 0, 0)

        if temperature is not None:
            oled.text("T:{}C H:{}%".format(temperature, humidity), 0, 10)
        else:
            oled.text("DHT ERROR", 0, 10)

        oled.text(
            "V:{:.2f}V I:{:.3f}A".format(battery_voltage, current),
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

        oled.text(
            "FAULT!" if fault else "Mov:{}".format(move_stat),
            0,
            50
        )

        oled.show()

        time.sleep(2)

    except Exception as e:
        print("--------------------------------")
        print("SYSTEM ERROR:")
        print(e)

        oled.fill(0)
        oled.text("SYSTEM ERROR", 0, 0)
        oled.text(str(e)[:20], 0, 20)
        oled.show()

        time.sleep(2)
