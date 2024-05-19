#include "sdbusplus/bus.hpp"
#include "sdbusplus/bus/match.hpp"
#include "sdbusplus/message/native_types.hpp"

#include <sdbusplus/async.hpp>

#include <chrono>
#include <iostream>
#include <list>
#include <utility>

namespace rules = sdbusplus::bus::match::rules;

enum class PowerState
{
    Awake,
    Suspended,
    Hibernating
};

enum class BatteryState
{
    Charging,
    Discharging,
    Idle,
};

enum class Stat : uint32_t
{
    energy = 1,
    rate = 2,
    averageRate = 4,
    relEnergy = 8,
};

struct StatFlags
{
    StatFlags(Stat s) : value(std::to_underlying(s)) {}
    StatFlags(uint32_t v = 0) : value(v) {}
    uint32_t value;
};

StatFlags operator|(Stat a, Stat b)
{
    return StatFlags{std::to_underlying(a) | std::to_underlying(b)};
}

bool operator&(StatFlags flags, Stat a)
{
    return (flags.value & std::to_underlying(a)) != 0;
}

StatFlags operator|(StatFlags f, Stat s)
{
    f.value |= std::to_underlying(s);
    return f;
}

template <typename T>
std::string formatRelTime(T duration)
{
    const auto relTime =
        std::chrono::duration_cast<std::chrono::seconds>(duration);
    if (relTime.count() == 0)
    {
        return {};
    }

    std::string output;
    const auto hours = std::chrono::duration_cast<std::chrono::hours>(relTime);
    if (hours.count() > 0)
    {
        output += std::format("{}h", hours.count());
    }
    const auto mins =
        std::chrono::duration_cast<std::chrono::minutes>(relTime) - hours;
    if (mins.count() > 0)
    {
        output += std::format("{}m", mins.count());
    }
    const auto secs = relTime - hours - mins;
    if (secs.count() > 0)
    {
        output += std::format("{}s", secs.count());
    }
    return output;
}

class BatteryMonitor
{
    using Clock = std::chrono::system_clock;
    using Time = Clock::time_point;
    using RelClock = std::chrono::steady_clock;
    using RelTime = RelClock::time_point;
    struct Reading
    {
        Time time;
        RelTime relTime;
        double energy;
    };

  public:
    BatteryMonitor() = default;

    void setPowerState(PowerState powerState)
    {
        switch (powerState)
        {
            case PowerState::Suspended:
                enterSuspendTime = Clock::now();
                print("Going to sleep");
                break;
            case PowerState::Awake:
            {
                if (!enterSuspendTime)
                {
                    break;
                }
                const auto suspendTime =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        Clock::now() - *enterSuspendTime);

                enterSuspendTime.reset();
                printSuspendStats = true;
                print(std::format("Resumed from {} sleep",
                                  formatRelTime(suspendTime)));
                break;
            }
            default:
                break;
        }
    }

    bool isSuspended() const
    {
        return enterSuspendTime.has_value();
    }

    void setBatteryState(BatteryState batteryState)
    {
        if (batteryState == BatteryState::Idle)
        {
            print("Battery idle");
            // Don't clear stats when going idle
            return;
        }

        firstReading.reset();
        readings.clear();
        totalSuspendEnergy = 0;

        switch (batteryState)
        {
            case BatteryState::Charging:
                print("Battery charging");
                break;
            case BatteryState::Discharging:
                print("Battery discharging");
                break;
            default:
                break;
        }
    }

    void setBatteryLimits(double empty, double full)
    {
        energyEmpty = empty;
        energyFull = full;
    }

    void updateEnergy(double energy)
    {
        if (isSuspended())
        {
            // Drop any readings that come in between enter/exit suspend events.
            // It seems tricky to tell whether a reading in this period happens
            // before or after the actual hardware suspend. The former case
            // would be fine to process, but in the latter interval (between HW
            // waking and resume D-Bus event) processing the reading would mess
            // up our stats (more significantly with longer sleep time).
            return;
        }

        Reading r{
            .time = Clock::now(), .relTime = RelClock::now(), .energy = energy};

        if (!firstReading)
        {
            firstReading = r;
        }

        readings.push_back(r);
        if (readings.size() > 2)
        {
            readings.pop_front();
        }

        if (printSuspendStats)
        {
            if (readings.size() > 1)
            {
                totalSuspendEnergy +=
                    (energy - std::prev(readings.end(), 2)->energy);
            }
            print("Sleep energy use", Stat::relEnergy | Stat::rate);
            printSuspendStats = false;
        }
        else
        {
            print("", Stat::energy | Stat::rate | Stat::averageRate);
        }
    }

  private:
    void print(std::string_view msg, StatFlags flags = StatFlags())
    {
        const std::chrono::zoned_time curTime{
            std::chrono::current_zone(),
            std::chrono::time_point_cast<std::chrono::seconds>(
                std::chrono::system_clock::now())};
        std::cout << curTime;

        if (firstReading)
        {
            const std::string runTimeStr =
                formatRelTime(RelClock::now() - firstReading->relTime);
            if (!runTimeStr.empty())
            {
                std::cout << std::format(" (+{})", runTimeStr);
            }
        }

        if (!msg.empty())
        {
            std::cout << " - " << msg;
        }

        if (readings.empty())
        {
            std::cout << std::endl;
            return;
        }

        const Reading* const curReading = &readings.back();
        const Reading* const prevReading =
            readings.size() > 1 ? &*std::prev(readings.end(), 2) : nullptr;

        if (flags & Stat::energy)
        {
            std::cout << std::format(" - {:.2f} Wh", curReading->energy);
            if (energyEmpty && energyFull)
            {
                double percent = 100 * (curReading->energy - *energyEmpty) /
                                 (*energyFull - *energyEmpty);
                std::cout << std::format(" ({:.2f}%)", percent);
            }
        }

        if ((flags & Stat::relEnergy) && prevReading != nullptr)
        {
            double energyDiff = curReading->energy - prevReading->energy;
            std::cout << std::format(" - {:+.2f} Wh", energyDiff);
            if (energyEmpty && energyFull)
            {
                double percent = 100 * energyDiff /
                                 (*energyFull - *energyEmpty);
                std::cout << std::format(" ({:.2f}%)", percent);
            }
        }

        if ((flags & Stat::rate) && prevReading != nullptr)
        {
            std::cout << " / Rate ";
            printRate(curReading->energy - prevReading->energy,
                      std::chrono::duration_cast<std::chrono::milliseconds>(
                          curReading->time - prevReading->time));
        }

        if ((flags & Stat::averageRate) && firstReading && readings.size() > 1)
        {
            std::cout << " / Avg ";
            double awakeEnergy = curReading->energy - firstReading->energy -
                                 totalSuspendEnergy;
            auto awakeTime = curReading->relTime - firstReading->relTime;
            printRate(awakeEnergy,
                      std::chrono::duration_cast<std::chrono::milliseconds>(
                          awakeTime));
        }
        std::cout << std::endl;
    }

    void printRate(double energyDiff, std::chrono::milliseconds timeDiff)
    {
        const double msPerHour = 1000 * 60 * 60;
        const double hours = timeDiff.count() / msPerHour;
        const double watts = energyDiff / hours;

        std::cout << std::format("{:.2f} W", watts);

        if (energyEmpty && energyFull)
        {
            const double percentPerHour =
                (100 * energyDiff / (*energyFull - *energyEmpty)) / hours;
            if (std::abs(percentPerHour) >= 1.0)
            {
                std::cout << std::format(" ({:.1f}%/hr)", percentPerHour);
            }
            else
            {
                const double percentPerDay = percentPerHour * 24;
                std::cout << std::format(" ({:.1f}%/day)", percentPerDay);
            }
        }
    }

  private:
    std::optional<double> energyEmpty;
    std::optional<double> energyFull;
    std::optional<Reading> firstReading;
    std::list<Reading> readings;

    bool printSuspendStats = false;
    std::optional<Time> enterSuspendTime;
    double totalSuspendEnergy = 0;
};

auto sleepEventMonitor(sdbusplus::async::context& ctx, BatteryMonitor& batmon)
    -> sdbusplus::async::task<>
{
    auto match = sdbusplus::async::match(
        ctx, rules::type::signal() + rules::path("/BatteryStats") +
                 rules::interface("BatteryStats.Sleep") +
                 rules::member("SystemdSleepEvent"));

    while (true)
    {
        auto [stage, operation, extraAction] =
            co_await match.next<std::string, std::string, std::string>();

        if (operation == "suspend")
        {
            if (stage == "pre")
            {
                batmon.setPowerState(PowerState::Suspended);
            }
            else if (stage == "post")
            {
                batmon.setPowerState(PowerState::Awake);
            }
        }
    }
}

using UPowerDeviceProperty = std::variant<std::string, uint64_t, uint32_t, bool,
                                          double, int32_t, int64_t>;
using UPowerDeviceProperties =
    std::unordered_map<std::string, UPowerDeviceProperty>;

void processBatteryProperties(BatteryMonitor& batmon,
                              const UPowerDeviceProperties& properties)
{
    auto propIt = properties.find("State");
    if (propIt != properties.end())
    {
        auto state = std::get<uint32_t>(propIt->second);
        switch (state)
        {
            case 1:
                batmon.setBatteryState(BatteryState::Charging);
                break;
            case 2:
                batmon.setBatteryState(BatteryState::Discharging);
                break;
            case 4:
            case 5:
                batmon.setBatteryState(BatteryState::Idle);
                break;
        }
    }

    propIt = properties.find("EnergyEmpty");
    if (propIt != properties.end())
    {
        double energyEmpty = std::get<double>(propIt->second);

        propIt = properties.find("EnergyFull");
        if (propIt != properties.end())
        {
            double energyFull = std::get<double>(propIt->second);
            batmon.setBatteryLimits(energyEmpty, energyFull);
        }
    }

    propIt = properties.find("Energy");
    if (propIt != properties.end())
    {
        batmon.updateEnergy(std::get<double>(propIt->second));
    }
}

auto powerEventMonitor(sdbusplus::async::context& ctx, BatteryMonitor& batmon)
    -> sdbusplus::async::task<>
{
    // Find the battery object
    constexpr auto upower =
        sdbusplus::async::proxy().service("org.freedesktop.UPower");
    constexpr auto upowerMain = upower.path("/org/freedesktop/UPower")
                                    .interface("org.freedesktop.UPower");
    constexpr auto upowerDevice =
        upower.interface("org.freedesktop.UPower.Device");

    const auto devices =
        co_await upowerMain.call<std::vector<sdbusplus::message::object_path>>(
            ctx, "EnumerateDevices");
    std::optional<sdbusplus::message::object_path> batteryPath;
    for (const auto& device : devices)
    {
        const auto deviceObject = upowerDevice.path(device.str);

        const auto type = co_await deviceObject.get_property<uint32_t>(ctx,
                                                                       "Type");
        if (type == 2)
        {
            std::cout << "Found battery at " << device.str << '\n';
            if (batteryPath)
            {
                std::cout << "Multiple batteries not supported yet\n";
                co_return;
            }
            batteryPath = device;
        }
    }

    if (!batteryPath)
    {
        std::cout << "No battery found\n";
        co_return;
    }

    // Get all current properties
    const auto batteryObject = upowerDevice.path(batteryPath->str);

    processBatteryProperties(
        batmon,
        co_await batteryObject.get_all_properties<UPowerDeviceProperty>(ctx));

    // Watch for future property updates
    auto batteryChangeMatch = sdbusplus::async::match(
        ctx, rules::propertiesChanged(batteryPath->str,
                                      "org.freedesktop.UPower.Device"));
    while (true)
    {
        auto [interfaceName, changedProps,
              invalProps] = co_await batteryChangeMatch
                                .next<std::string, UPowerDeviceProperties,
                                      std::vector<std::string>>();

        processBatteryProperties(batmon, changedProps);
    }
}

int main(int /* argc */, char** /* argv */)
{
    BatteryMonitor batmon;
    sdbusplus::async::context ctx(sdbusplus::bus::new_default_system());
    ctx.spawn(sleepEventMonitor(ctx, batmon));
    ctx.spawn(powerEventMonitor(ctx, batmon));
    ctx.run();

    return 0;
}
