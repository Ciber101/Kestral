#pragma once
// Electronics in a design: real parts (boards, HATs, sensors, motors, servos, drivers, batteries)
// from a catalog with their datasheet numbers, placed in the design and joined by wires. The
// wiring is checked like the real thing (voltages, logic levels, current, shorts, I2C addresses)
// and becomes a step by step wiring guide, and later the netlist for a circuit board.
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace kst {

enum class PinKind : uint8_t {
    PowerOut,   // supplies a rail (regulator output, battery +, a header 5 V that feeds others)
    PowerIn,    // takes a rail (VIN, VCC, a servo's +)
    Ground,
    DigitalIO,  // GPIO: input or output at the logic level
    DigitalIn,  // only reads (a trigger input, a driver's logic input)
    DigitalOut, // only drives (an echo, an encoder channel, a sensor output)
    Analog,     // ADC input
    Pwm,        // PWM output (servo signal, motor speed)
    I2cSda,
    I2cScl,
    UartTx,
    UartRx,
    SpiMosi,
    SpiMiso,
    SpiSck,
    SpiCs,
    MotorOut,   // H bridge output
    MotorIn,    // a motor terminal
    Camera,     // CSI ribbon
    Other,
};

struct PinDef {
    std::string id;          // unique in the part: "GPIO17", "P0.S", "VIN"
    std::string label;       // what is printed on the board or the datasheet name
    PinKind kind = PinKind::Other;
    std::string connector;   // where it is: "40 pin header, pin 11", "Servo port P0", "XH2.54 battery"
    std::string header;      // HATs: the Raspberry Pi header pin this one sits on ("5V", "GND", "GPIO17")
    std::string rail;        // pins of one part with the same rail are joined inside it (every GND, USB-C and 5 V)
    std::string regFrom;     // PowerOut / MotorOut: the part's input pin that powers it (its draw lands there)
    std::string must;        // must be connected, and why ("pull it high or the driver never wakes")
    double vOut = 0;         // PowerOut: its voltage. Outputs: logic high. -1 = the voltage on the part's supply
    double vMin = 0, vMax = 0;   // PowerIn: accepted range. Inputs: the highest voltage it survives (vMax)
    double iMax = 0;         // PowerOut: amps it can give. MotorOut: amps per channel (continuous)
    double iPeak = 0;        // MotorOut / PowerOut: short peak amps (0 = same as iMax)
    double iDraw = 0;        // PowerIn: typical amps the part takes here
    double iDrawPeak = 0;    // PowerIn: peak amps (motors starting, Wi-Fi bursts)
    std::string note;
};

struct MotorSpec {
    double ratedV = 0;       // the voltage the numbers below were measured at
    double noLoadRpm = 0;    // at the output shaft
    double noLoadA = 0;
    double stallA = 0;
    double stallTorqueNm = 0;    // at the output shaft (0 = not published)
    double gearRatio = 0;
    int encoderPpr = 0;      // pulses per motor turn per channel (0 = none)
};

struct ServoSpec {
    double minV = 0, maxV = 0;
    double torqueNm = 0, atV = 0;     // stall torque and the voltage it is rated at
    double speedSecPer60 = 0;         // no load, seconds per 60 degrees
    double stallA = 0;
    double rangeDeg = 180;
};

struct BatterySpec {
    int cellsSeries = 2, cellsParallel = 1;
    double cellNominalV = 3.7, cellFullV = 4.2, cellEmptyV = 3.0;
    double capacityAh = 3.0;          // whole pack
    double maxA = 10;                 // continuous discharge
};

struct PartDef {
    std::string id;          // "rpi5", "fusion-hat-plus"
    std::string name;        // "Raspberry Pi 5"
    std::string category;    // Board, HAT, Sensor, Camera, Motor, Servo, Motor driver, Battery, Power
    std::string maker;
    double massG = 0;
    std::vector<std::pair<std::string, std::string>> specs;   // shown as a table
    std::vector<std::string> notes;   // the things that bite people (shown in the guide)
    std::vector<std::string> sources; // datasheet and product pages the numbers come from
    std::vector<int> i2cAddr;         // default I2C addresses
    bool stacksOnPi = false;          // a HAT: sits on a Raspberry Pi 40 pin header
    bool hasMotor = false, hasServo = false, hasBattery = false;
    MotorSpec motor;
    ServoSpec servo;
    BatterySpec battery;
    std::vector<PinDef> pins;
    const PinDef* pin(const std::string& pid) const {
        for (auto& p : pins) if (p.id == pid) return &p;
        return nullptr;
    }
};

// One real part in the design.
struct ElecPart {
    uint32_t id = 0;
    std::string def;             // PartDef id
    std::string label;           // "Left drive motor"
    std::string attach;          // component path or body name it is (or sits on); blank = not placed
    std::map<std::string, double> vals;   // overrides: "capacity_mAh", "cells_parallel"
};

struct Wire {
    uint32_t id = 0;
    uint32_t a = 0, b = 0;       // ElecPart ids
    std::string aPin, bPin;      // pin ids; "HEADER" on both = a HAT pressed onto the Pi's header
    std::string color;           // "red", "black", "yellow"...
    int awg = 26;
    std::string note;
};

struct ElecDoc {
    std::vector<ElecPart> parts;
    std::vector<Wire> wires;
    uint32_t nextId = 1;
    bool empty() const { return parts.empty() && wires.empty(); }
    ElecPart* part(uint32_t id) {
        for (auto& p : parts) if (p.id == id) return &p;
        return nullptr;
    }
    const ElecPart* part(uint32_t id) const {
        for (auto& p : parts) if (p.id == id) return &p;
        return nullptr;
    }
};

} // namespace kst
