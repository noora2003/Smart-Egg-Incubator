#define BLYNK_TEMPLATE_ID "TMPL64r9C3c-L"
#define BLYNK_TEMPLATE_NAME "Smart Egg Incubator"
#define BLYNK_AUTH_TOKEN "YcTJKd02cZTlxLcF0754C7UQahqXvcf4"

// ======================================================
// LIBRARIES
// ======================================================

#include <Arduino.h>
#include <WiFi.h>
#include <BlynkSimpleEsp32.h>

#include <DHT.h>

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ======================================================
// WIFI
// ======================================================

char ssid[] = "Wael";
char pass[] = "Ww123456.";

// ======================================================
// PIN MAP
// ======================================================

#define DHT_PIN 4
#define DHT_TYPE DHT11

#define HEATER_RELAY_PIN 16
#define HUMID_RELAY_PIN 17

#define STEPPER_IN1 18
#define STEPPER_IN2 27
#define STEPPER_IN3 25
#define STEPPER_IN4 26

#define BUZZER_PIN 19

#define SDA_PIN 21
#define SCL_PIN 22

// ======================================================
// RELAYS (ACTIVE LOW)
// ======================================================

#define RELAY_ON LOW
#define RELAY_OFF HIGH

// ======================================================
// TEST / REAL MODE
// ======================================================

#define BENCH_TEST_MODE true

#define TEST_TURN_INTERVAL_MS 30000UL

#define REAL_TURN_INTERVAL_MS \
    (6UL * 60UL * 60UL * 1000UL)

#define DAY_DURATION_MS \
    (24UL * 60UL * 60UL * 1000UL)

#define TURN_STOP_DAY 18
#define LOCKDOWN_DAY 18

// ======================================================
// CONTROL LIMITS
// ======================================================

#define TEMP_LOW 36.8
#define TEMP_HIGH 38.0

#define HUM_LOW_NORMAL 50.0
#define HUM_HIGH_NORMAL 55.0

#define HUM_LOW_LOCKDOWN 65.0
#define HUM_HIGH_LOCKDOWN 70.0

// ======================================================
// SAFETY LIMITS
// ======================================================

#define SAFETY_TEMP_LOW 35.5
#define SAFETY_TEMP_HIGH 39.5

#define SAFETY_HUM_LOW 20.0
#define SAFETY_HUM_HIGH 90.0

// ======================================================
// OBJECTS
// ======================================================

DHT dht(DHT_PIN, DHT_TYPE);

Adafruit_SSD1306 display(128, 64, &Wire, -1);

// ======================================================
// SENSOR DATA
// ======================================================

struct SensorData
{
    float temperature;
    float humidity;
    bool valid;
    unsigned long timestamp;
};

QueueHandle_t sensorQueue;
SemaphoreHandle_t stateMutex;

// ======================================================
// SHARED SYSTEM STATE
// ======================================================

bool heaterState = false;
bool humidityState = false;

volatile bool systemFault = false;

// Motor state (published to Blynk — FR6)
volatile bool motorActive = false;

bool turnDirection = true;

int incubationDay = 1;
unsigned long dayStartTime = 0;

// ======================================================
// MANUAL / TEST MODE
// ======================================================

bool manualMode = false;

bool manualHeaterCommand = false;
bool manualHumidifierCommand = false;

bool turnNowRequest = false;

// ======================================================
// PENDING BLYNK NOTIFICATIONS
// (SafetyTask/EggTurnTask set these; BlynkTask sends them)
// ======================================================

bool pendingFaultAlert = false;
bool pendingFaultCleared = false;
bool pendingTurnButtonReset = false;

// ======================================================
// STEPPER CONFIGURATION
// ======================================================

const int motorPins[4] = {
    STEPPER_IN1,
    STEPPER_IN2,
    STEPPER_IN3,
    STEPPER_IN4};

const int stepSequence[8][4] = {
    {1, 0, 0, 0},
    {1, 1, 0, 0},
    {0, 1, 0, 0},
    {0, 1, 1, 0},
    {0, 0, 1, 0},
    {0, 0, 1, 1},
    {0, 0, 0, 1},
    {1, 0, 0, 1}};

// ======================================================
// STEPPER FUNCTIONS
// ======================================================

void setStep(int stepNumber)
{
    for (int i = 0; i < 4; i++)
    {
        digitalWrite(motorPins[i], stepSequence[stepNumber][i]);
    }
}

void motorOff()
{
    for (int i = 0; i < 4; i++)
    {
        digitalWrite(motorPins[i], LOW);
    }
}

bool moveStepper(int steps, bool clockwise)
{
    for (int i = 0; i < steps; i++)
    {
        if (systemFault)
        {
            motorOff();
            Serial.println("[TURN] ABORTED BY SAFETY");
            return false;
        }

        int stepIndex;

        if (clockwise)
        {
            stepIndex = i % 8;
        }
        else
        {
            stepIndex = 7 - (i % 8);
        }

        setStep(stepIndex);
        delayMicroseconds(2000);
    }

    motorOff();
    return true;
}

// ======================================================
// BLYNK CONTROL CALLBACKS
// ======================================================

// V6 = MANUAL MODE
BLYNK_WRITE(V6)
{
    bool value = param.asInt();

    xSemaphoreTake(stateMutex, portMAX_DELAY);

    manualMode = value;

    manualHeaterCommand = false;
    manualHumidifierCommand = false;

    xSemaphoreGive(stateMutex);

    Blynk.virtualWrite(V7, 0);
    Blynk.virtualWrite(V8, 0);

    Serial.print("[MODE] ");
    Serial.println(manualMode ? "MANUAL / TEST" : "AUTOMATIC");
}

// V7 = HEATER TEST
BLYNK_WRITE(V7)
{
    bool value = param.asInt();

    xSemaphoreTake(stateMutex, portMAX_DELAY);

    if (manualMode)
    {
        manualHeaterCommand = value;
    }
    else
    {
        manualHeaterCommand = false;
    }

    bool accepted = manualMode;

    xSemaphoreGive(stateMutex);

    if (!accepted)
    {
        Blynk.virtualWrite(V7, 0);
        Serial.println("[MANUAL HEATER] Ignored - Manual Mode OFF");
        return;
    }

    Serial.print("[MANUAL HEATER] ");
    Serial.println(value ? "ON" : "OFF");
}

// V8 = HUMIDIFIER TEST
BLYNK_WRITE(V8)
{
    bool value = param.asInt();

    xSemaphoreTake(stateMutex, portMAX_DELAY);

    if (manualMode)
    {
        manualHumidifierCommand = value;
    }
    else
    {
        manualHumidifierCommand = false;
    }

    bool accepted = manualMode;

    xSemaphoreGive(stateMutex);

    if (!accepted)
    {
        Blynk.virtualWrite(V8, 0);
        Serial.println("[MANUAL HUMIDIFIER] Ignored - Manual Mode OFF");
        return;
    }

    Serial.print("[MANUAL HUMIDIFIER] ");
    Serial.println(value ? "ON" : "OFF");
}

// V9 = TURN EGGS NOW
BLYNK_WRITE(V9)
{
    if (param.asInt() == 1)
    {
        xSemaphoreTake(stateMutex, portMAX_DELAY);

        if (manualMode)
        {
            turnNowRequest = true;
            Serial.println("[MANUAL TURN] Requested");
        }
        else
        {
            Serial.println("[MANUAL TURN] Ignored - Manual Mode OFF");
        }

        xSemaphoreGive(stateMutex);
    }
}

// ======================================================
// WHEN BLYNK CONNECTS
// ======================================================

BLYNK_CONNECTED()
{
    xSemaphoreTake(stateMutex, portMAX_DELAY);

    manualMode = false;
    manualHeaterCommand = false;
    manualHumidifierCommand = false;
    turnNowRequest = false;

    xSemaphoreGive(stateMutex);

    Blynk.virtualWrite(V6, 0);
    Blynk.virtualWrite(V7, 0);
    Blynk.virtualWrite(V8, 0);
    Blynk.virtualWrite(V9, 0);

    Serial.println("[BLYNK] Controls reset to safe state");
}

// ======================================================
// SENSOR TASK  (Priority 3, 1 Hz)
// ======================================================

void SensorTask(void *parameter)
{
    SensorData data;

    vTaskDelay(pdMS_TO_TICKS(1000));

    while (true)
    {
        float temperature = dht.readTemperature();
        float humidity = dht.readHumidity();

        bool valid =
            !isnan(temperature) &&
            !isnan(humidity) &&
            temperature >= 5.0 &&
            temperature <= 60.0 &&
            humidity >= 1.0 &&
            humidity <= 100.0;

        data.temperature = temperature;
        data.humidity = humidity;
        data.valid = valid;
        data.timestamp = millis();

        xQueueOverwrite(sensorQueue, &data);

        if (valid)
        {
            Serial.print("[SENSOR] Temp: ");
            Serial.print(temperature, 1);
            Serial.print(" C | Humidity: ");
            Serial.print(humidity, 1);
            Serial.println(" %");
        }
        else
        {
            Serial.println("[SENSOR] INVALID");
        }

        // DHT11 supports 1 Hz (FR1 / NFR1)
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// ======================================================
// SAFETY WATCHDOG  (Highest Priority = 4)
// ======================================================

void SafetyTask(void *parameter)
{
    SensorData data;

    unsigned long outOfRangeStart = 0;

    while (true)
    {
        bool fault = false;
        bool outOfRange = false;

        bool hasData =
            xQueuePeek(sensorQueue, &data, 0) == pdTRUE;

        if (hasData)
        {
            // Invalid sensor
            if (!data.valid)
            {
                fault = true;
            }

            // Sensor stale (>3s)
            if (millis() - data.timestamp > 3000)
            {
                fault = true;
            }

            // Extreme limits only in real operation
            if (!BENCH_TEST_MODE && data.valid)
            {
                if (data.temperature < SAFETY_TEMP_LOW ||
                    data.temperature > SAFETY_TEMP_HIGH ||
                    data.humidity < SAFETY_HUM_LOW ||
                    data.humidity > SAFETY_HUM_HIGH)
                {
                    outOfRange = true;
                }
            }
        }

        // Grace period (2 minutes)
        if (outOfRange)
        {
            if (outOfRangeStart == 0)
            {
                outOfRangeStart = millis();
                Serial.println("[SAFETY] Out-of-range detected, starting grace period...");
            }

            if (millis() - outOfRangeStart >= 120000UL)
            {
                fault = true;
            }
        }
        else
        {
            if (outOfRangeStart != 0)
            {
                Serial.println("[SAFETY] Values back to normal, grace period reset");
                outOfRangeStart = 0;
            }
        }

        // Sensor never responded
        if (!hasData && millis() > 4000)
        {
            fault = true;
        }

        // Update shared state
        xSemaphoreTake(stateMutex, portMAX_DELAY);

        bool oldFault = systemFault;
        systemFault = fault;

        if (systemFault)
        {
            heaterState = false;
            humidityState = false;

            digitalWrite(HEATER_RELAY_PIN, RELAY_OFF);
            digitalWrite(HUMID_RELAY_PIN, RELAY_OFF);
            motorOff();
        }

        // Set flags for BlynkTask (Blynk runs in ONE task only)
        if (fault && !oldFault)
        {
            pendingFaultAlert = true;
        }

        if (!fault && oldFault)
        {
            pendingFaultCleared = true;
        }

        xSemaphoreGive(stateMutex);

        if (fault && !oldFault)
        {
            Serial.println("[SAFETY] FAULT DETECTED");
        }

        if (!fault && oldFault)
        {
            Serial.println("[SAFETY] Fault cleared");
        }

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

// ======================================================
// HEATER TASK  (Priority 2)
// ======================================================

void HeaterTask(void *parameter)
{
    SensorData data;

    while (true)
    {
        if (xQueuePeek(sensorQueue, &data, 0) == pdTRUE)
        {
            xSemaphoreTake(stateMutex, portMAX_DELAY);

            bool newState = heaterState;

            if (systemFault ||
                !data.valid ||
                millis() - data.timestamp > 3000)
            {
                newState = false;
            }
            else if (manualMode)
            {
                newState = manualHeaterCommand;
            }
            else
            {
                if (data.temperature < TEMP_LOW)
                {
                    newState = true;
                }
                else if (data.temperature > TEMP_HIGH)
                {
                    newState = false;
                }
            }

            if (newState != heaterState)
            {
                heaterState = newState;

                digitalWrite(HEATER_RELAY_PIN,
                             heaterState ? RELAY_ON : RELAY_OFF);

                Serial.print("[HEATER] ");
                Serial.println(heaterState ? "ON" : "OFF");
            }

            xSemaphoreGive(stateMutex);
        }

        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

// ======================================================
// HUMIDITY TASK  (Priority 2)
// ======================================================

void HumidityTask(void *parameter)
{
    SensorData data;

    while (true)
    {
        if (xQueuePeek(sensorQueue, &data, 0) == pdTRUE)
        {
            xSemaphoreTake(stateMutex, portMAX_DELAY);

            bool newState = humidityState;

            if (systemFault ||
                !data.valid ||
                millis() - data.timestamp > 3000)
            {
                newState = false;
            }
            else if (manualMode)
            {
                newState = manualHumidifierCommand;
            }
            else
            {
                float humLow;
                float humHigh;

                if (incubationDay >= LOCKDOWN_DAY)
                {
                    humLow = HUM_LOW_LOCKDOWN;
                    humHigh = HUM_HIGH_LOCKDOWN;
                }
                else
                {
                    humLow = HUM_LOW_NORMAL;
                    humHigh = HUM_HIGH_NORMAL;
                }

                if (data.humidity < humLow)
                {
                    newState = true;
                }
                else if (data.humidity > humHigh)
                {
                    newState = false;
                }
            }

            if (newState != humidityState)
            {
                humidityState = newState;

                digitalWrite(HUMID_RELAY_PIN,
                             humidityState ? RELAY_ON : RELAY_OFF);

                Serial.print("[HUMIDIFIER] ");
                Serial.println(humidityState ? "ON" : "OFF");
            }

            xSemaphoreGive(stateMutex);
        }

        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

// ======================================================
// EGG TURN TASK  (Priority 1)
// ======================================================

void EggTurnTask(void *parameter)
{
    unsigned long lastTurn = millis();

    while (true)
    {
        unsigned long interval;

        if (BENCH_TEST_MODE)
        {
            interval = TEST_TURN_INTERVAL_MS;
        }
        else
        {
            interval = REAL_TURN_INTERVAL_MS;
        }

        bool fault;
        bool manual;
        bool request;
        int day;

        xSemaphoreTake(stateMutex, portMAX_DELAY);

        fault = systemFault;
        manual = manualMode;
        day = incubationDay;
        request = turnNowRequest;

        if (request)
        {
            turnNowRequest = false;
        }

        xSemaphoreGive(stateMutex);

        // MANUAL TURN
        if (request && manual && !fault)
        {
            if (BENCH_TEST_MODE || day < TURN_STOP_DAY)
            {
                Serial.print("[MANUAL TURN] Moving ");
                Serial.println(turnDirection ? "RIGHT" : "LEFT");

                motorActive = true;
                bool completed = moveStepper(512, turnDirection);
                motorActive = false;

                if (completed)
                {
                    turnDirection = !turnDirection;
                    Serial.println("[MANUAL TURN] Complete");
                }
            }
            else
            {
                Serial.println("[MANUAL TURN] Blocked - Day 18+");
            }

            // Set flag for BlynkTask (do NOT call Blynk here)
            xSemaphoreTake(stateMutex, portMAX_DELAY);
            pendingTurnButtonReset = true;
            xSemaphoreGive(stateMutex);
        }

        // AUTOMATIC TURNING
        if (!manual &&
            !fault &&
            day < TURN_STOP_DAY &&
            millis() - lastTurn >= interval)
        {
            Serial.print("[TURN] Day ");
            Serial.print(day);
            Serial.print(" - Moving ");
            Serial.println(turnDirection ? "RIGHT" : "LEFT");

            motorActive = true;
            bool completed = moveStepper(512, turnDirection);
            motorActive = false;

            if (completed)
            {
                turnDirection = !turnDirection;
                lastTurn = millis();
                Serial.println("[TURN] Complete");
            }
        }

        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

// ======================================================
// DAY COUNTER TASK
// NOTE: uses millis() — if power is lost, day resets.
// For robust day tracking, use DS3231 RTC.
// ======================================================

void DayTask(void *parameter)
{
    dayStartTime = millis();

    while (true)
    {
        if (millis() - dayStartTime >= DAY_DURATION_MS)
        {
            xSemaphoreTake(stateMutex, portMAX_DELAY);

            if (incubationDay < 21)
            {
                incubationDay++;

                Serial.print("[DAY] Incubation Day = ");
                Serial.println(incubationDay);
            }

            xSemaphoreGive(stateMutex);

            dayStartTime = millis();
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// ======================================================
// ACTIVE BUZZER ALARM TASK
// ======================================================

void AlarmTask(void *parameter)
{
    while (true)
    {
        bool fault;

        xSemaphoreTake(stateMutex, portMAX_DELAY);
        fault = systemFault;
        xSemaphoreGive(stateMutex);

        if (fault)
        {
            digitalWrite(BUZZER_PIN, HIGH);
            vTaskDelay(pdMS_TO_TICKS(200));

            digitalWrite(BUZZER_PIN, LOW);
            vTaskDelay(pdMS_TO_TICKS(200));
        }
        else
        {
            digitalWrite(BUZZER_PIN, LOW);
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}

// ======================================================
// OLED DISPLAY TASK
// ======================================================

void DisplayTask(void *parameter)
{
    SensorData data;

    while (true)
    {
        if (xQueuePeek(sensorQueue, &data, 0) == pdTRUE)
        {
            bool heater;
            bool humidifier;
            bool fault;
            bool manual;
            int day;

            xSemaphoreTake(stateMutex, portMAX_DELAY);

            heater = heaterState;
            humidifier = humidityState;
            fault = systemFault;
            manual = manualMode;
            day = incubationDay;

            xSemaphoreGive(stateMutex);

            display.clearDisplay();
            display.setTextSize(1);
            display.setTextColor(SSD1306_WHITE);

            // Header
            display.setCursor(0, 0);
            display.print("Incubator ");
            display.println(manual ? "[MAN]" : "[AUTO]");

            // Day
            display.setCursor(0, 12);
            display.print("Day: ");
            display.println(day);

            // Sensor
            display.setCursor(0, 24);
            if (data.valid)
            {
                display.print("T:");
                display.print(data.temperature, 1);
                display.print("C H:");
                display.print(data.humidity, 0);
                display.println("%");
            }
            else
            {
                display.println("SENSOR ERROR");
            }

            // Outputs
            display.setCursor(0, 36);
            display.print("Heat:");
            display.print(heater ? "ON " : "OFF");
            display.print(" Hum:");
            display.println(humidifier ? "ON" : "OFF");

            // Status
            display.setCursor(0, 48);
            if (fault)
            {
                display.println("FAULT");
            }
            else
            {
                display.print("OK Turn:");
                display.println(day < TURN_STOP_DAY ? "ON" : "STOP");
            }

            display.display();
        }

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

// ======================================================
// BLYNK + WIFI TASK
// ======================================================

void BlynkTask(void *parameter)
{
    unsigned long lastSend = 0;
    unsigned long lastWiFiAttempt = 0;
    unsigned long lastBlynkAttempt = 0;

    SensorData data;

    while (true)
    {
        // WIFI
        if (WiFi.status() != WL_CONNECTED)
        {
            if (millis() - lastWiFiAttempt >= 10000)
            {
                Serial.println("[WIFI] Trying reconnect...");
                WiFi.disconnect();
                WiFi.begin(ssid, pass);
                lastWiFiAttempt = millis();
            }

            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        // BLYNK
        if (!Blynk.connected())
        {
            if (millis() - lastBlynkAttempt >= 5000)
            {
                Serial.println("[BLYNK] Trying reconnect...");

                if (Blynk.connect(1000))
                {
                    Serial.println("[BLYNK] Connected");
                }
                else
                {
                    Serial.println("[BLYNK] Connection failed");
                }

                lastBlynkAttempt = millis();
            }
        }
        else
        {
            Blynk.run();

            // ========================================
            // Handle pending Blynk notifications
            // (Blynk must only be called from ONE task)
            // ========================================

            xSemaphoreTake(stateMutex, portMAX_DELAY);

            bool sendFaultAlert = pendingFaultAlert;
            bool sendFaultCleared = pendingFaultCleared;
            bool resetTurnButton = pendingTurnButtonReset;

            pendingFaultAlert = false;
            pendingFaultCleared = false;
            pendingTurnButtonReset = false;

            xSemaphoreGive(stateMutex);

            if (sendFaultAlert)
            {
                Blynk.logEvent("fault_alert",
                               "Incubator fault detected!");
                Serial.println("[BLYNK] fault_alert sent");
            }

            if (sendFaultCleared)
            {
                Blynk.logEvent("fault_cleared",
                               "Incubator back to normal");
                Serial.println("[BLYNK] fault_cleared sent");
            }

            if (resetTurnButton)
            {
                Blynk.virtualWrite(V9, 0);
                Serial.println("[BLYNK] Turn button reset");
            }

            // ======================================
            // Telemetry every 2 seconds
            // ======================================

            if (millis() - lastSend >= 2000)
            {
                if (xQueuePeek(sensorQueue, &data, 0) == pdTRUE)
                {
                    bool heater;
                    bool humidifier;
                    bool fault;
                    bool manual;
                    bool manualHeat;
                    bool manualHum;
                    int day;

                    xSemaphoreTake(stateMutex, portMAX_DELAY);

                    heater = heaterState;
                    humidifier = humidityState;
                    fault = systemFault;
                    manual = manualMode;
                    manualHeat = manualHeaterCommand;
                    manualHum = manualHumidifierCommand;
                    day = incubationDay;

                    xSemaphoreGive(stateMutex);

                    if (data.valid)
                    {
                        Blynk.virtualWrite(V0, data.temperature);
                        Blynk.virtualWrite(V1, data.humidity);
                    }

                    Blynk.virtualWrite(V2, heater ? 1 : 0);
                    Blynk.virtualWrite(V3, humidifier ? 1 : 0);
                    Blynk.virtualWrite(V4, day);
                    Blynk.virtualWrite(V5, fault ? 1 : 0);
                    Blynk.virtualWrite(V10, motorActive ? 1 : 0);

                    Blynk.virtualWrite(V6, manual ? 1 : 0);
                    Blynk.virtualWrite(V7, manualHeat ? 1 : 0);
                    Blynk.virtualWrite(V8, manualHum ? 1 : 0);

                    Serial.println("[BLYNK] Data sent");
                }

                lastSend = millis();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

// ======================================================
// SETUP
// ======================================================

void setup()
{
    Serial.begin(115200);
    delay(500);

    Serial.println();
    Serial.println("================================");
    Serial.println("Smart Incubator Final");
    Serial.println("FreeRTOS + Blynk + Manual Mode");
    Serial.println("================================");

    // RELAYS
    pinMode(HEATER_RELAY_PIN, OUTPUT);
    pinMode(HUMID_RELAY_PIN, OUTPUT);

    digitalWrite(HEATER_RELAY_PIN, RELAY_OFF);
    digitalWrite(HUMID_RELAY_PIN, RELAY_OFF);

    // BUZZER
    pinMode(BUZZER_PIN, OUTPUT);
    digitalWrite(BUZZER_PIN, LOW);

    // STEPPER
    pinMode(STEPPER_IN1, OUTPUT);
    pinMode(STEPPER_IN2, OUTPUT);
    pinMode(STEPPER_IN3, OUTPUT);
    pinMode(STEPPER_IN4, OUTPUT);
    motorOff();

    // DHT
    dht.begin();

    // OLED
    Wire.begin(SDA_PIN, SCL_PIN);

    if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C))
    {
        Serial.println("OLED FAILED");
    }
    else
    {
        Serial.println("OLED OK");

        display.clearDisplay();
        display.setTextSize(1);
        display.setTextColor(SSD1306_WHITE);
        display.setCursor(0, 20);
        display.println("Starting system...");
        display.display();
    }

    // FREERTOS OBJECTS
    sensorQueue = xQueueCreate(1, sizeof(SensorData));
    stateMutex = xSemaphoreCreateMutex();

    if (sensorQueue == NULL || stateMutex == NULL)
    {
        Serial.println("FreeRTOS init FAILED");
        while (true)
        {
            delay(1000);
        }
    }

    // WIFI + BLYNK
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, pass);
    Blynk.config(BLYNK_AUTH_TOKEN);

    Serial.println("WiFi/Blynk initialized");

    // CREATE TASKS
    xTaskCreate(SafetyTask, "Safety", 3072, NULL, 4, NULL);
    xTaskCreate(SensorTask, "Sensor", 4096, NULL, 3, NULL);
    xTaskCreate(HeaterTask, "Heater", 2048, NULL, 2, NULL);
    xTaskCreate(HumidityTask, "Humidity", 2048, NULL, 2, NULL);
    xTaskCreate(AlarmTask, "Alarm", 2048, NULL, 2, NULL);
    xTaskCreate(EggTurnTask, "EggTurn", 3072, NULL, 1, NULL);
    xTaskCreate(DayTask, "DayCounter", 2048, NULL, 1, NULL);
    xTaskCreate(DisplayTask, "Display", 4096, NULL, 1, NULL);
    xTaskCreate(BlynkTask, "Blynk", 6144, NULL, 1, NULL);

    Serial.println("All FreeRTOS tasks started");
}

// ======================================================
// LOOP
// ======================================================

void loop()
{
    vTaskDelay(pdMS_TO_TICKS(1000));
}