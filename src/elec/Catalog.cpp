// The parts catalog. Numbers come from the maker's datasheet or product page (listed in sources).
// Where a maker publishes nothing, the value is a measured or typical figure and its note says so.
#include "elec/Elec.h"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace kst::elec {

namespace {

PinDef pin(const std::string& id, const std::string& label, PinKind k, const std::string& conn) {
    PinDef p;
    p.id = id;
    p.label = label;
    p.kind = k;
    p.connector = conn;
    return p;
}
PinDef pwrOut(const std::string& id, const std::string& label, const std::string& conn, double v, double iMax, double iPeak = 0) {
    PinDef p = pin(id, label, PinKind::PowerOut, conn);
    p.vOut = v;
    p.iMax = iMax;
    p.iPeak = iPeak;
    return p;
}
PinDef pwrIn(const std::string& id, const std::string& label, const std::string& conn, double vMin, double vMax, double draw, double peak) {
    PinDef p = pin(id, label, PinKind::PowerIn, conn);
    p.vMin = vMin;
    p.vMax = vMax;
    p.iDraw = draw;
    p.iDrawPeak = peak;
    return p;
}
PinDef gnd(const std::string& id, const std::string& label, const std::string& conn) {
    PinDef p = pin(id, label, PinKind::Ground, conn);
    p.rail = "GND";
    return p;
}
// A signal: level = the high voltage it drives (outputs), vMax = the most it takes (inputs).
PinDef sig(const std::string& id, const std::string& label, PinKind k, const std::string& conn, double level, double vMax, double iMax = 0) {
    PinDef p = pin(id, label, k, conn);
    p.vOut = level;
    p.vMax = vMax;
    p.iMax = iMax;
    return p;
}

PartDef raspberryPi5() {
    PartDef d;
    d.id = "rpi5";
    d.name = "Raspberry Pi 5";
    d.category = "Board";
    d.maker = "Raspberry Pi";
    d.massG = 46;
    d.specs = {{"Power", "USB-C, 5.1 V 5 A (27 W PD) for full USB current"},
               {"Draw", "about 2.7 W idle, up to about 12 W under full CPU load"},
               {"GPIO", "3.3 V logic, not 5 V tolerant, about 16 mA per pin, keep the total under about 50 mA"},
               {"Camera", "2 x 22 pin 0.5 mm MIPI (CAM/DISP 0 and 1)"},
               {"Size", "85 x 56 mm"}};
    d.notes = {"GPIO pins are 3.3 V only: 5 V on any GPIO pin can kill the Pi.",
               "Power the Pi from one place only: the USB-C port or the 5 V header pins (from a HAT), never both at once.",
               "Camera modules with a 15 pin connector need the 22 to 15 pin \"Standard to Mini\" ribbon for the Pi 5."};
    d.sources = {"https://www.raspberrypi.com/documentation/computers/raspberry-pi.html",
                 "https://www.raspberrypi.com/products/raspberry-pi-5/"};
    auto& P = d.pins;
    PinDef usb = pwrIn("USBC", "USB-C power", "USB-C power port", 4.75, 5.4, 0.6, 2.4);
    usb.rail = "5V";
    P.push_back(usb);
    PinDef v5 = pwrIn("5V", "5V", "40 pin header, pins 2 and 4", 4.75, 5.4, 0, 0);
    v5.rail = "5V";   // the Pi's own draw counts once, on USBC
    v5.note = "The same rail as USB-C: feeds the Pi from a HAT, or feeds small 5 V parts when the Pi runs on USB-C.";
    P.push_back(v5);
    PinDef v33 = pwrOut("3V3", "3V3", "40 pin header, pins 1 and 17", 3.3, 0.3);
    v33.regFrom = "USBC";
    v33.note = "Not a published limit: keep 3.3 V loads under about 300 mA.";
    P.push_back(v33);
    P.push_back(gnd("GND", "GND", "40 pin header, pins 6, 9, 14, 20, 25, 30, 34, 39"));
    struct G { int n, phys; PinKind k; const char* alt; };
    const G gp[] = {{2, 3, PinKind::I2cSda, "I2C SDA"}, {3, 5, PinKind::I2cScl, "I2C SCL"}, {4, 7, PinKind::DigitalIO, ""},
                    {14, 8, PinKind::UartTx, "UART TX"}, {15, 10, PinKind::UartRx, "UART RX"}, {17, 11, PinKind::DigitalIO, ""},
                    {18, 12, PinKind::DigitalIO, "PWM0, I2S clock"}, {27, 13, PinKind::DigitalIO, ""}, {22, 15, PinKind::DigitalIO, ""},
                    {23, 16, PinKind::DigitalIO, ""}, {24, 18, PinKind::DigitalIO, ""}, {10, 19, PinKind::DigitalIO, "SPI MOSI"},
                    {9, 21, PinKind::DigitalIO, "SPI MISO"}, {25, 22, PinKind::DigitalIO, ""}, {11, 23, PinKind::DigitalIO, "SPI SCLK"},
                    {8, 24, PinKind::DigitalIO, "SPI CE0"}, {7, 26, PinKind::DigitalIO, "SPI CE1"}, {0, 27, PinKind::DigitalIO, "ID EEPROM, leave free"},
                    {1, 28, PinKind::DigitalIO, "ID EEPROM, leave free"}, {5, 29, PinKind::DigitalIO, ""}, {6, 31, PinKind::DigitalIO, ""},
                    {12, 32, PinKind::DigitalIO, "PWM0"}, {13, 33, PinKind::DigitalIO, "PWM1"}, {19, 35, PinKind::DigitalIO, "I2S"},
                    {16, 36, PinKind::DigitalIO, ""}, {26, 37, PinKind::DigitalIO, ""}, {20, 38, PinKind::DigitalIO, "I2S"},
                    {21, 40, PinKind::DigitalIO, "I2S"}};
    for (auto& g : gp) {
        std::string id = "GPIO" + std::to_string(g.n);
        PinDef p = sig(id, id, g.k, "40 pin header, pin " + std::to_string(g.phys), 3.3, 3.3, 0.016);
        if (g.alt[0]) p.note = g.alt;
        P.push_back(p);
    }
    PinDef cam0 = pin("CAM0", "CAM/DISP 0", PinKind::Camera, "22 pin 0.5 mm MIPI connector");
    PinDef cam1 = pin("CAM1", "CAM/DISP 1", PinKind::Camera, "22 pin 0.5 mm MIPI connector");
    cam0.note = cam1.note = "22 pin";
    P.push_back(cam0);
    P.push_back(cam1);
    return d;
}

PartDef fusionHat() {
    PartDef d;
    d.id = "fusion-hat-plus";
    d.name = "SunFounder Fusion HAT+";
    d.category = "HAT";
    d.maker = "SunFounder";
    d.massG = 30;
    d.stacksOnPi = true;
    d.specs = {{"Battery in", "6.0 to 8.4 V, XH2.54 3 pin (2S Li-ion), cuts off at 6.0 V"},
               {"Output", "5 V 5 A (shared by the Pi and the servo ports), 3.3 V 1 A"},
               {"Motor ports", "4, 1.8 A each, straight from the battery (6 to 8.4 V)"},
               {"PWM", "12 channels: P0 to P3 free for servos, P4 to P11 drive the motor ports"},
               {"ADC", "A0 to A3, 12 bit, 3.3 V. A4 reads the battery"},
               {"Digital", "4 pins wired straight to Pi GPIO17, GPIO4, GPIO27, GPIO22 (3.3 V)"},
               {"Charging", "USB-C 5 V 3 A in, about 2 hours, 1.5 A"},
               {"MCU", "GD32E203C8T6 on I2C"}};
    d.notes = {"Motor ports give 1.8 A each. A motor that stalls above that can burn the port: use a separate motor driver for big motors.",
               "The 5 V 5 A is shared by the Pi and every servo. Over 5 A total needs an external BEC for the servos.",
               "The digital pins D0 to D3 are bare Pi GPIO: 3.3 V only.",
               "The battery plug is XH2.54 3 pin, the same as a 2S LiPo balance lead, but a balance lead is too thin to run the Pi and servos: wire the main leads to it instead.",
               "Its charger is for 2S lithium (8.4 V). It does not balance cells: charge a LiPo with a balance charger."};
    d.sources = {"https://docs.sunfounder.com/projects/fusion-hat/en/latest/hardware/features.html",
                 "https://docs.sunfounder.com/projects/fusion-hat/en/latest/hardware/interfaces.html"};
    auto& P = d.pins;
    PinDef bat = pwrIn("BAT+", "Battery +", "XH2.54 3 pin battery plug", 6.0, 8.4, 0.03, 0.03);
    P.push_back(bat);
    P.push_back(pin("BAT.M", "Battery middle", PinKind::Other, "XH2.54 3 pin battery plug (between the cells)"));
    P.push_back(gnd("BAT-", "Battery -", "XH2.54 3 pin battery plug"));
    PinDef usb = pwrIn("USBC", "USB-C charge", "USB-C port (charging only)", 4.25, 8.4, 0, 0);
    usb.note = "Charges the battery, about 2 hours.";
    P.push_back(usb);
    PinDef v5 = pwrOut("5V", "5V", "Pi header 5 V (feeds the Pi)", 5.285, 5.0);
    v5.header = "5V";
    v5.rail = "5V";
    v5.regFrom = "BAT+";
    P.push_back(v5);
    PinDef g = gnd("GND", "GND", "Pi header GND and every GND pin");
    g.header = "GND";
    P.push_back(g);
    PinDef v33 = pwrOut("3V3", "3V3", "3.3 V pins (I2C, ADC, digital ports)", 3.3, 1.0);
    v33.regFrom = "BAT+";
    P.push_back(v33);
    for (int i = 0; i < 4; ++i) {
        std::string n = "P" + std::to_string(i), c = "Servo port " + n + " (3 pin)";
        P.push_back(sig(n + ".S", n + " signal", PinKind::Pwm, c, 3.3, 3.3, 0.008));
        PinDef p = pwrOut(n + ".+", n + " +5V", c, 5.285, 5.0);
        p.rail = "5V";
        p.regFrom = "BAT+";
        P.push_back(p);
        P.push_back(gnd(n + ".-", n + " GND", c));
    }
    for (int i = 0; i < 4; ++i) {
        std::string n = "M" + std::to_string(i), c = "Motor port " + n + " (XH2.54 2 pin)";
        for (const char* s : {".A", ".B"}) {
            PinDef p = pin(n + s, n + s, PinKind::MotorOut, c);
            p.iMax = 1.8;
            p.iPeak = 1.8;
            p.regFrom = "BAT+";
            P.push_back(p);
        }
    }
    const char* dgp[] = {"GPIO17", "GPIO4", "GPIO27", "GPIO22"};
    for (int i = 0; i < 4; ++i) {
        std::string n = "D" + std::to_string(i), c = "Digital port " + n + " (3 pin)";
        PinDef s = sig(n + ".S", n + " (" + dgp[i] + ")", PinKind::DigitalIO, c, 3.3, 3.3, 0.016);
        s.header = dgp[i];
        P.push_back(s);
        PinDef p = pwrOut(n + ".V", n + " power", c, 3.3, 1.0);
        p.regFrom = "BAT+";
        p.note = "Check the silkscreen: these ports are 3.3 V on SunFounder HATs.";
        P.push_back(p);
        P.push_back(gnd(n + ".-", n + " GND", c));
    }
    for (int i = 0; i < 4; ++i) {
        std::string n = "A" + std::to_string(i);
        P.push_back(sig(n, n, PinKind::Analog, "ADC port " + n + " (3 pin)", 0, 3.3));
    }
    PinDef sda = sig("SDA", "I2C SDA", PinKind::I2cSda, "I2C header / QWIIC", 3.3, 3.3);
    sda.header = "GPIO2";
    PinDef scl = sig("SCL", "I2C SCL", PinKind::I2cScl, "I2C header / QWIIC", 3.3, 3.3);
    scl.header = "GPIO3";
    P.push_back(sda);
    P.push_back(scl);
    PinDef tx = sig("TX", "UART TX", PinKind::UartTx, "UART header", 3.3, 3.3);
    tx.header = "GPIO14";
    PinDef rx = sig("RX", "UART RX", PinKind::UartRx, "UART header", 3.3, 3.3);
    rx.header = "GPIO15";
    P.push_back(tx);
    P.push_back(rx);
    // The 7 pin SPI header: plain Pi GPIO when SPI is not used (MOSI is shared with the WS2812 port).
    const std::pair<const char*, const char*> spi[] = {{"BSY", "GPIO6"}, {"CE0", "GPIO8"}, {"SCK", "GPIO11"}, {"MISO", "GPIO9"}, {"MOSI", "GPIO10"}};
    for (auto& [n, g] : spi) {
        PinDef s = sig(n, std::string(n) + " (" + g + ")", PinKind::DigitalIO, "SPI header (7 pin)", 3.3, 3.3, 0.016);
        s.header = g;
        P.push_back(s);
    }
    return d;
}

PartDef battery2S(const std::string& id, const std::string& name, double ah, double maxA, int par, double massG,
                  const std::string& plug, std::vector<std::string> notes, std::vector<std::string> sources, bool lipo) {
    PartDef d;
    d.id = id;
    d.name = name;
    d.category = "Battery";
    d.massG = massG;
    d.hasBattery = true;
    d.battery.cellsSeries = 2;
    d.battery.cellsParallel = par;
    d.battery.capacityAh = ah;
    d.battery.maxA = maxA;
    d.battery.cellEmptyV = lipo ? 3.3 : 3.0;
    char b[96];
    std::snprintf(b, sizeof b, "%.1f Ah, %.0f A max", ah, maxA);
    d.specs = {{"Voltage", "7.4 V nominal, 8.4 V full, about " + std::string(lipo ? "6.6" : "6.0") + " V empty"}, {"Capacity", b}, {"Plug", plug}};
    d.notes = std::move(notes);
    d.sources = std::move(sources);
    PinDef p = pwrOut("+", "+", plug + " (red)", 8.4, maxA);
    p.vMin = 2 * d.battery.cellEmptyV;
    d.pins.push_back(p);
    d.pins.push_back(gnd("-", "-", plug + " (black)"));
    if (lipo) {
        PinDef bal = pwrOut("BAL+", "Balance +", "JST-XH 3 pin balance lead", 8.4, 1.0);
        bal.vMin = 6.6;
        bal.note = "For charging and voltage checks: about 1 A at most.";
        d.pins.push_back(bal);
        d.pins.push_back(pin("BAL.M", "Balance middle", PinKind::Other, "JST-XH 3 pin balance lead"));
        d.pins.push_back(gnd("BAL-", "Balance -", "JST-XH 3 pin balance lead"));
    }
    return d;
}

PartDef encoderMotor(const std::string& id, const std::string& name, double massG, MotorSpec m, double encMin, double encMax,
                     std::vector<std::pair<std::string, std::string>> specs, std::vector<std::string> notes, std::vector<std::string> sources) {
    PartDef d;
    d.id = id;
    d.name = name;
    d.category = "Motor";
    d.massG = massG;
    d.hasMotor = true;
    d.motor = m;
    d.specs = std::move(specs);
    d.notes = std::move(notes);
    d.sources = std::move(sources);
    d.pins.push_back(pin("M+", "Motor +", PinKind::MotorIn, "motor lead (usually red)"));
    d.pins.push_back(pin("M-", "Motor -", PinKind::MotorIn, "motor lead (usually black or white)"));
    if (m.encoderPpr > 0) {
        d.pins.push_back(pwrIn("VCC", "Encoder VCC", "encoder lead", encMin, encMax, 0.01, 0.015));
        d.pins.push_back(gnd("GND", "Encoder GND", "encoder lead"));
        PinDef a = sig("A", "Encoder A", PinKind::DigitalOut, "encoder lead", -1, 0);
        PinDef b = sig("B", "Encoder B", PinKind::DigitalOut, "encoder lead", -1, 0);
        a.note = b.note = "Swings to the encoder's VCC: power it from 3.3 V when it goes to a Pi pin.";
        d.pins.push_back(a);
        d.pins.push_back(b);
    }
    return d;
}

PartDef hobbyServo(const std::string& id, const std::string& name, double massG, ServoSpec s, std::vector<std::pair<std::string, std::string>> specs,
                   std::vector<std::string> notes, std::vector<std::string> sources, double idle) {
    PartDef d;
    d.id = id;
    d.name = name;
    d.category = "Servo";
    d.massG = massG;
    d.hasServo = true;
    d.servo = s;
    d.specs = std::move(specs);
    d.notes = std::move(notes);
    d.sources = std::move(sources);
    d.pins.push_back(sig("S", "Signal", PinKind::DigitalIn, "orange or yellow wire", 0, 6.0));
    d.pins.push_back(pwrIn("+", "+", "red wire", s.minV, s.maxV, idle, s.stallA));
    d.pins.push_back(gnd("-", "-", "brown or black wire"));
    return d;
}

std::vector<PartDef> build() {
    std::vector<PartDef> c;
    c.push_back(raspberryPi5());
    c.push_back(fusionHat());

    // ---- power
    c.push_back(battery2S("zeee-2s-5200-50c", "Zeee 2S LiPo 5200 mAh 50C (hard case)", 5.2, 260, 1, 270, "T plug (Deans)",
                          {"Main leads (T plug) carry the real current: motors and the HAT go on these, not on the balance lead.",
                           "Charge with a LiPo balance charger. Do not run it below about 3.3 V per cell (6.6 V).",
                           "50C on 5.2 Ah is about 260 A on paper: far more than this robot needs, so voltage sag stays small."},
                          {"https://www.amazon.com/s?k=Zeee+2S+5200mAh+50C+hard+case"}, true));
    c.push_back(battery2S("li-ion-2s1p-18650", "2S Li-ion 18650 pack (2 cells, Fusion HAT kit)", 2.0, 8, 1, 95, "XH2.54 3 pin",
                          {"Capacity depends on the cells: 2.0 Ah is a typical kit cell. Set capacity_mAh to yours."},
                          {"https://docs.sunfounder.com/projects/fusion-hat/en/latest/hardware/features.html"}, false));
    c.push_back(battery2S("li-ion-2s4p-18650", "2S4P Li-ion 18650 pack (8 cells, home built)", 12.0, 40, 4, 390, "XT60 or T plug",
                          {"Use 8 matched cells and a 2S BMS rated for the current. 12 Ah assumes 3.0 Ah cells: set capacity_mAh to yours."},
                          {}, false));
    {
        PartDef d;
        d.id = "canakit-45w-pd";
        d.name = "CanaKit 45 W USB-C PD supply (Pi 5)";
        d.category = "Power";
        d.maker = "CanaKit";
        d.massG = 90;
        d.specs = {{"Output", "5.1 V 5 A for the Pi 5 (27 W), other PD voltages up to 45 W"}};
        d.notes = {"For the bench: running the Pi from the wall while the robot is parked or charging the Fusion HAT through its USB-C.",
                   "Do not plug it into the Pi while the HAT is also feeding the Pi from the battery."};
        d.sources = {"https://www.canakit.com/"};
        d.pins.push_back(pwrOut("OUT", "USB-C out", "USB-C cable", 5.1, 5.0));
        d.pins.push_back(gnd("GND", "GND", "USB-C cable"));
        c.push_back(d);
    }
    {
        PartDef d;
        d.id = "ubec-5v-5a";
        d.name = "5 V 5 A UBEC (servo supply)";
        d.category = "Power";
        d.massG = 20;
        d.specs = {{"Input", "6 to 24 V (2S to 6S)"}, {"Output", "5.1 V 5 A"}};
        d.notes = {"Feeds servos from the battery so they do not load the Pi's 5 V."};
        d.pins.push_back(pwrIn("IN+", "In +", "input leads", 6.0, 24.0, 0.01, 0.01));
        d.pins.push_back(gnd("IN-", "In -", "input leads"));
        PinDef o = pwrOut("OUT+", "Out +", "servo style output", 5.1, 5.0);
        o.regFrom = "IN+";
        d.pins.push_back(o);
        d.pins.push_back(gnd("OUT-", "Out -", "servo style output"));
        c.push_back(d);
    }

    // ---- motors
    {
        MotorSpec m;
        m.ratedV = 12;
        m.noLoadRpm = 40;
        m.noLoadA = 0.1;
        m.stallA = 1.5;
        m.encoderPpr = 11;
        c.push_back(encoderMotor("worm-12v-40rpm-enc", "12 V worm gear motor with encoder, 40 RPM (JGY-370 type)", 190, m, 3.3, 5.0,
                                 {{"Rated", "12 V, 40 RPM at the shaft"}, {"Gearbox", "worm, self locking"},
                                  {"Encoder", "hall, 2 channels, VCC 3.3 to 5 V (11 pulses per motor turn is typical: count yours)"},
                                  {"Stall current", "about 1.5 A at 12 V (typical for this size, not published: measure it)"}},
                                 {"On a 2S pack (6.6 to 8.4 V) it runs at about 55 to 70% of the 12 V speed: roughly 22 to 28 RPM, with less torque.",
                                  "Self locking: the tracks will not roll when pushed and the robot stops dead when power is cut.",
                                  "Power the encoder from 3.3 V so its A and B outputs are safe for Pi pins."},
                                 {"https://www.amazon.com/s?k=12V+worm+gear+motor+encoder+40RPM"}));
    }
    {
        MotorSpec m;
        m.ratedV = 6;
        m.noLoadRpm = 120;
        m.noLoadA = 0.1;
        m.stallA = 1.0;
        m.gearRatio = 90;
        m.encoderPpr = 12;
        c.push_back(encoderMotor("tt-metal-90-enc", "TT motor, all metal 1:90 gearbox, 12 PPR hall encoder", 45, m, 3.3, 5.0,
                                 {{"Measured (Adafruit)", "6 V: 120 RPM, 0.1 A no load, 1.0 A stalled"}, {"Gearbox", "1:90, right angle"},
                                  {"Encoder", "hall, 12 pulses per motor turn"}},
                                 {"Black \"pro strength\" versions are rated 3 to 12 V and can stall well above 1 A at 7.4 V: measure yours."},
                                 {"https://www.adafruit.com/products/3802"}));
    }

    // ---- motor drivers
    {
        PartDef d;
        d.id = "drv8876-board";
        d.name = "DRV8876 single motor driver board";
        d.category = "Motor driver";
        d.maker = "Texas Instruments chip";
        d.massG = 4;
        d.specs = {{"Motor supply", "4.5 to 37 V"}, {"Current", "about 1.3 A continuous on a small board, 3.5 A peak (under a second)"},
                   {"Logic", "1.8 to 5.5 V inputs"}, {"Modes", "PWM (IN/IN) or phase and enable"}, {"Current sense", "about 2.5 V per amp on IPROPI/CS"}};
        d.notes = {"nSLEEP must be pulled high or the driver stays off.",
                   "It gets hot at 1 to 2 A: give it airflow, and stalls longer than a second trip its thermal cutoff.",
                   "One board drives one motor: a tank needs two."};
        d.sources = {"https://shop.pimoroni.com/en-us/products/drv8876-single-brushed-dc-motor-driver-carrier",
                     "https://www.ti.com/lit/gpn/DRV8876"};
        d.pins.push_back(pwrIn("VM", "VM", "motor power terminal", 4.5, 37, 0.003, 0.003));
        d.pins.push_back(gnd("GND", "GND", "power terminal and logic header"));
        for (const char* o : {"OUT1", "OUT2"}) {
            PinDef p = pin(o, o, PinKind::MotorOut, "motor terminal");
            p.iMax = 1.3;
            p.iPeak = 3.5;
            p.regFrom = "VM";
            d.pins.push_back(p);
        }
        d.pins.push_back(sig("IN1", "EN/IN1", PinKind::DigitalIn, "logic header", 0, 5.5));
        d.pins.push_back(sig("IN2", "PH/IN2", PinKind::DigitalIn, "logic header", 0, 5.5));
        PinDef sl = sig("nSLEEP", "nSLEEP", PinKind::DigitalIn, "logic header", 0, 5.5);
        sl.must = "pull it high (to 3.3 V or a GPIO set high) or the driver never wakes";
        d.pins.push_back(sl);
        d.pins.push_back(sig("PMODE", "PMODE", PinKind::DigitalIn, "logic header", 0, 5.5));
        d.pins.push_back(sig("nFAULT", "nFAULT", PinKind::Other, "logic header (open drain, needs a pull-up)", 0, 5.5));
        d.pins.push_back(sig("CS", "IPROPI / CS", PinKind::Other, "logic header (about 2.5 V per amp)", 0, 0));
        c.push_back(d);
    }
    {
        PartDef d;
        d.id = "tb6612fng";
        d.name = "TB6612FNG dual motor driver";
        d.category = "Motor driver";
        d.maker = "Toshiba chip";
        d.massG = 3;
        d.specs = {{"Motor supply", "4.5 to 13.5 V"}, {"Logic", "2.7 to 5.5 V"}, {"Current", "1.2 A per channel, 3.2 A peak"}};
        d.notes = {"STBY must be high for the outputs to work."};
        d.sources = {"https://www.pololu.com/product/713"};
        d.pins.push_back(pwrIn("VM", "VM", "motor power", 4.5, 13.5, 0.002, 0.002));
        d.pins.push_back(pwrIn("VCC", "VCC", "logic power", 2.7, 5.5, 0.002, 0.002));
        d.pins.push_back(gnd("GND", "GND", "header"));
        for (const char* o : {"AO1", "AO2", "BO1", "BO2"}) {
            PinDef p = pin(o, o, PinKind::MotorOut, "motor outputs");
            p.iMax = 1.2;
            p.iPeak = 3.2;
            p.regFrom = "VM";
            d.pins.push_back(p);
        }
        for (const char* s : {"AIN1", "AIN2", "PWMA", "BIN1", "BIN2", "PWMB"}) d.pins.push_back(sig(s, s, PinKind::DigitalIn, "header", 0, 5.5));
        PinDef st = sig("STBY", "STBY", PinKind::DigitalIn, "header", 0, 5.5);
        st.must = "tie it high or the outputs stay off";
        d.pins.push_back(st);
        c.push_back(d);
    }

    // ---- sensors and camera
    {
        PartDef d;
        d.id = "camera-module-3";
        d.name = "Raspberry Pi Camera Module 3 (IMX708)";
        d.category = "Camera";
        d.maker = "Raspberry Pi";
        d.massG = 4;
        d.specs = {{"Sensor", "Sony IMX708, 12 MP, autofocus"}, {"Connector", "15 pin 1 mm ribbon"}, {"Size", "25 x 24 x 11.5 mm"}};
        d.notes = {"On a Pi 5 use the 22 to 15 pin ribbon (\"Standard to Mini\"), blue side to the right way round on both ends."};
        d.sources = {"https://www.raspberrypi.com/documentation/accessories/camera.html"};
        PinDef p = pin("CSI", "Camera ribbon", PinKind::Camera, "15 pin 1 mm ribbon connector");
        p.note = "15 pin";
        d.pins.push_back(p);
        c.push_back(d);
    }
    {
        PartDef d;
        d.id = "bno055-board";
        d.name = "BNO055 9 axis IMU board (Adafruit style)";
        d.category = "Sensor";
        d.maker = "Bosch chip";
        d.massG = 3;
        d.i2cAddr = {0x28};
        d.specs = {{"Power", "VIN 3.3 to 5 V (3.3 V regulator on board), about 12 mA"}, {"Bus", "I2C, address 0x28 (0x29 with ADR high)"},
                   {"Output", "fused orientation, gyro, accel, mag"}};
        d.notes = {"The Pi's I2C slows clock stretching: if readings fail, lower the I2C speed (dtparam=i2c_arm_baudrate=10000)."};
        d.sources = {"https://learn.adafruit.com/adafruit-bno055-absolute-orientation-sensor"};
        d.pins.push_back(pwrIn("VIN", "VIN", "header", 3.3, 5.0, 0.0123, 0.015));
        d.pins.push_back(gnd("GND", "GND", "header"));
        d.pins.push_back(sig("SDA", "SDA", PinKind::I2cSda, "header", 3.3, 5.0));
        d.pins.push_back(sig("SCL", "SCL", PinKind::I2cScl, "header", 3.3, 5.0));
        d.pins.push_back(sig("ADR", "ADR", PinKind::DigitalIn, "header (high = 0x29)", 0, 3.6));
        c.push_back(d);
    }
    {
        PartDef d;
        d.id = "bme280-board";
        d.name = "BME280 temperature, humidity, pressure board (3.3/5 V)";
        d.category = "Sensor";
        d.maker = "Bosch chip";
        d.massG = 2;
        d.i2cAddr = {0x76};
        d.specs = {{"Power", "3.3 or 5 V (board with regulator), under 1 mA"}, {"Bus", "I2C 0x76 (SDO to GND) or 0x77, or SPI"}};
        d.notes = {"For I2C leave CSB high or open. SDO picks the address: GND = 0x76, 3.3 V = 0x77.",
                   "Mount it away from the Pi and motor drivers or it reads their heat, not the room."};
        d.sources = {"https://www.bosch-sensortec.com/products/environmental-sensors/humidity-sensors-bme280/"};
        d.pins.push_back(pwrIn("VCC", "VCC", "6 pin header", 3.3, 5.0, 0.001, 0.001));
        d.pins.push_back(gnd("GND", "GND", "6 pin header"));
        d.pins.push_back(sig("SCL", "SCL", PinKind::I2cScl, "6 pin header", 3.3, 5.0));
        d.pins.push_back(sig("SDA", "SDA", PinKind::I2cSda, "6 pin header", 3.3, 5.0));
        d.pins.push_back(sig("CSB", "CSB", PinKind::DigitalIn, "6 pin header (leave open for I2C)", 0, 3.6));
        d.pins.push_back(sig("SDO", "SDO", PinKind::DigitalIn, "6 pin header (address select)", 0, 3.6));
        c.push_back(d);
    }
    {
        PartDef d;
        d.id = "hc-sr04";
        d.name = "Ultrasonic distance sensor (HC-SR04 type)";
        d.category = "Sensor";
        d.massG = 9;
        d.specs = {{"Power", "5 V, about 15 mA"}, {"Range", "2 to 400 cm"}, {"Echo", "a pulse at the supply voltage (5 V)"}};
        d.notes = {"Echo is 5 V: put a divider (1 k and 2 k) before a Pi pin, or use a 3.3 V version of the module."};
        d.sources = {"https://cdn.sparkfun.com/datasheets/Sensors/Proximity/HCSR04.pdf"};
        d.pins.push_back(pwrIn("VCC", "VCC", "4 pin header", 4.5, 5.5, 0.015, 0.015));
        d.pins.push_back(sig("TRIG", "Trig", PinKind::DigitalIn, "4 pin header", 0, 5.5));
        d.pins.push_back(sig("ECHO", "Echo", PinKind::DigitalOut, "4 pin header", -1, 0));
        d.pins.push_back(gnd("GND", "GND", "4 pin header"));
        c.push_back(d);
    }
    {
        PartDef d;
        d.id = "rgb-led-module";
        d.name = "RGB LED module (common cathode, DIYables)";
        d.category = "Light";
        d.maker = "DIYables";
        d.massG = 2;
        d.specs = {{"Pins", "R, G, B, - (common cathode)"}, {"Drive", "3.3 or 5 V logic, a few mA per color"}};
        d.notes = {"Check the module has a resistor on each color: without them an LED pulls too much from a Pi pin.",
                   "Use PWM pins for color mixing."};
        d.sources = {"https://www.amazon.com/s?k=DIYables+RGB+LED+module"};
        for (const char* s : {"R", "G", "B"}) {
            PinDef p = sig(s, s, PinKind::DigitalIn, "4 pin header", 0, 5.5);
            p.iDraw = 0.008;
            d.pins.push_back(p);
        }
        d.pins.push_back(gnd("-", "- (common)", "4 pin header"));
        c.push_back(d);
    }

    // ---- servos
    {
        ServoSpec s;
        s.minV = 4.8;
        s.maxV = 6.0;
        s.torqueNm = 0.176;
        s.atV = 4.8;
        s.speedSecPer60 = 0.1;
        s.stallA = 0.65;
        c.push_back(hobbyServo("sg90", "SG90 9 g micro servo", 9, s, {{"Torque", "1.8 kg cm at 4.8 V"}, {"Speed", "0.1 s per 60 degrees"}, {"Stall", "about 0.65 A (measured, not in the datasheet)"}},
                               {"Plastic gears: a pan/tilt head is fine, a load that slams into a stop strips them."},
                               {"http://www.ee.ic.ac.uk/pcheung/teaching/DE1_EE/stores/sg90_datasheet.pdf"}, 0.01));
    }
    {
        ServoSpec s;
        s.minV = 4.8;
        s.maxV = 6.0;
        s.torqueNm = 0.216;
        s.atV = 4.8;
        s.speedSecPer60 = 0.1;
        s.stallA = 0.7;
        c.push_back(hobbyServo("mg90s", "MG90S metal gear micro servo", 13.4, s, {{"Torque", "1.8 kg cm at 4.8 V, 2.2 at 6 V"}, {"Stall", "about 0.7 A (measured)"}}, {},
                               {"https://www.towerpro.com.tw/product/mg90s-3/"}, 0.01));
    }
    {
        ServoSpec s;
        s.minV = 4.8;
        s.maxV = 7.2;
        s.torqueNm = 1.08;
        s.atV = 6.0;
        s.speedSecPer60 = 0.14;
        s.stallA = 2.5;
        c.push_back(hobbyServo("mg996r", "MG996R metal gear servo", 55, s, {{"Torque", "9.4 kg cm at 4.8 V, 11 at 6 V"}, {"Stall", "about 2.5 A"}},
                               {"Two of these stalling can pull 5 A: give them their own BEC."}, {"https://www.towerpro.com.tw/product/mg996r/"}, 0.01));
    }
    for (auto& p : c)
        for (auto& pn : p.pins)
            if (pn.kind == PinKind::Ground && pn.rail.empty()) pn.rail = "GND";
    return c;
}

std::string lower(std::string s) {
    for (auto& ch : s) ch = (char)std::tolower((unsigned char)ch);
    return s;
}

} // namespace

const std::vector<PartDef>& catalog() {
    static const std::vector<PartDef> c = build();
    return c;
}

const PartDef* findDef(const std::string& id) {
    for (auto& d : catalog())
        if (d.id == id) return &d;
    std::string l = lower(id);
    for (auto& d : catalog())
        if (lower(d.name) == l) return &d;
    return nullptr;
}

std::vector<const PartDef*> search(const std::string& query) {
    std::vector<std::string> words;
    std::istringstream is(lower(query));
    for (std::string w; is >> w;) words.push_back(w);
    std::vector<const PartDef*> out;
    for (auto& d : catalog()) {
        std::string hay = lower(d.id + " " + d.name + " " + d.maker + " " + d.category);
        bool all = true;
        for (auto& w : words)
            if (hay.find(w) == std::string::npos) all = false;
        if (all) out.push_back(&d);
    }
    return out;
}

} // namespace kst::elec
