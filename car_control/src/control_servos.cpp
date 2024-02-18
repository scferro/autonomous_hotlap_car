/// \file control_servos.cpp
/// \brief Controls the steering servo and ESC via a PCA 9685 over I2C
///
/// PARAMETERS:
///     rate (double): the publishing rate for wheel speed messages
///     cmd_max (int): the maximum servo command
///     timeout (double): minimum time required between receiving commands
/// SUBSCRIBES:
///     steering_cmd (std_msgs::msg::Int32): the command for the steering servo
///     drive_cmd (std_msgs::msg::Int32): the command for the drive motor ESC
/// SERVERS:
///     enable_drive (std_srvs::srv::SetBool): enables/disables drive motor

#include <chrono>
#include <memory>
#include <string>
#include <random>
#include <vector>
#include <string>
#include <iostream>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>
#include <cstring>
#include <thread>

#include "rclcpp/rclcpp.hpp"
#include "std_msgs/msg/int32.hpp"
#include "std_srvs/srv/set_bool.hpp"

using namespace std::chrono_literals;

// PCA9685 Registers
#define MODE1 0x00
#define PRESCALE 0xFE
#define LED0_ON_L 0x06
#define LED0_ON_H 0x07
#define LED0_OFF_L 0x08
#define LED0_OFF_H 0x09

// I2C Address of the PCA9685 module
#define PCA9685_ADDRESS 0x40

// Function to initialize PCA9685
void initPCA9685(int fd) {
    // Reset the PCA9685
    char settings[2] = {MODE1, 0x00};
    write(fd, settings, 2);

    // Set the PWM frequency to 50 Hz
    float prescale_val = 25000000.0; // 25MHz
    prescale_val /= 4096.0;          // 12-bit
    prescale_val /= 50.0;            // 50Hz
    prescale_val -= 1.0;
    unsigned char prescale = static_cast<unsigned char>(prescale_val + 0.5);

    settings[0] = MODE1;
    settings[1] = 0x10; // Sleep
    write(fd, settings, 2);

    settings[0] = PRESCALE;
    settings[1] = prescale;
    write(fd, settings, 2);

    settings[0] = MODE1;
    settings[1] = 0x80; // Restart
    write(fd, settings, 2);

    settings[1] = 0x20; // Auto-Increment enabled
    write(fd, settings, 2);
}

void delayMicroseconds(int microseconds) {
    std::this_thread::sleep_for(std::chrono::microseconds(microseconds));
}

// Function to set PWM value for a specific channel
void setPWM(int fd, int channel, int onValue, int offValue) {
    char data[5];
    data[0] = LED0_ON_L + 4 * channel;
    data[1] = onValue & 0xFF;
    data[2] = onValue >> 8;
    data[3] = offValue & 0xFF;
    data[4] = offValue >> 8;
    write(fd, data, 5);
}

int convert_microsec(int microsec) {
    double pwm = microsec / 4.88;
    return static_cast<int>(pwm);
}

class Control_Servos : public rclcpp::Node
{
public:
  Control_Servos()
  : Node("control_servos")
  {
    // Parameters and default values
    declare_parameter("rate", 200.);
    declare_parameter("cmd_max", 2000);
    declare_parameter("cmd_min", 1000);
    declare_parameter("timeout", 1.);
    declare_parameter("steer_left_max", 1700);
    declare_parameter("steer_right_max", 1300);
    declare_parameter("enable_drive", true);
    declare_parameter("drive_pin", 0);
    declare_parameter("steer_pin", 1);

    // Define parameter variables
    loop_rate = get_parameter("rate").as_double();
    cmd_max = get_parameter("cmd_max").as_int();
    cmd_min = get_parameter("cmd_min").as_int();
    timeout = get_parameter("timeout").as_double();
    steer_left_max = get_parameter("steer_left_max").as_int();
    steer_right_max = get_parameter("steer_right_max").as_int();
    enable_drive = get_parameter("enable_drive").as_bool();
    drive_pin = get_parameter("drive_pin").as_int();
    steer_pin = get_parameter("steer_pin").as_int();

    // Define other variables
    default_steering_cmd = (steer_left_max + steer_right_max) / 2;
    default_drive_cmd = (cmd_max + cmd_min) / 2;
    steering_cmd = default_steering_cmd;
    drive_cmd = default_drive_cmd;
    now = this->get_clock()->now();
    time_now = now.seconds() + (now.nanoseconds() * 0.000000001);
    time_last_steer = time_now;
    time_last_drive = time_now;
    enable_drive = true;

    // Subscribers
    steering_cmd_sub = create_subscription<std_msgs::msg::Int32>(
      "steering_cmd",
      10, std::bind(&Control_Servos::steering_cmd_callback, this, std::placeholders::_1));
    drive_cmd_sub = create_subscription<std_msgs::msg::Int32>(
      "drive_cmd",
      10, std::bind(&Control_Servos::drive_cmd_callback, this, std::placeholders::_1));

    // Servers
    enable_drive_srv = create_service<std_srvs::srv::SetBool>(
      "enable_drive",
      std::bind(&Control_Servos::enable_drive_callback, this, std::placeholders::_1, std::placeholders::_2));

    // Main timer
    int cycle_time = 1000.0 / loop_rate;
    main_timer = this->create_wall_timer(
      std::chrono::milliseconds(cycle_time),
      std::bind(&Control_Servos::timer_callback, this));

    // Motor startup
    device = "/dev/i2c-7"; // Change to your I2C bus
    fd = open(device, O_RDWR);

    if (fd < 0) {
        RCLCPP_INFO(this->get_logger(), "Failed to open the i2c bus");
    }
    if (ioctl(fd, I2C_SLAVE, PCA9685_ADDRESS) < 0) {
        std::cerr << "Failed to acquire bus access and/or talk to slave." << std::endl;
        close(fd);
    }

    initPCA9685(fd);

    motor_startup();

    // Set motor commands to neutral
    setPWM(fd, 0, 0, convert_microsec(default_drive_cmd)); // 307 corresponds to approximately 1.5ms pulse width at 50Hz
    setPWM(fd, 1, 0, convert_microsec(default_steering_cmd)); // 307 corresponds to approximately 1.5ms pulse width at 50H
  }

private:
  // Initialize parameter variables
  int rate;
  double loop_rate, timeout;
  int cmd_max, cmd_min, steering_cmd, drive_cmd, default_steering_cmd, default_drive_cmd;
  double time_now, time_last_steer, time_last_drive;
  int steer_left_max, steer_right_max;
  rclcpp::Time now;
  const char *device;
  int fd, pwm;
  bool enable_drive;
  int drive_pin, steer_pin;

  // Initialize subscriptions and timer
  rclcpp::Subscription<std_msgs::msg::Int32>::SharedPtr steering_cmd_sub;
  rclcpp::Subscription<std_msgs::msg::Int32>::SharedPtr drive_cmd_sub;
  rclcpp::Service<std_srvs::srv::SetBool>::SharedPtr enable_drive_srv;
  rclcpp::TimerBase::SharedPtr main_timer;

  /// \brief The main timer callback, updates diff_drive state and publishes odom messages
  void timer_callback()
  {
    now = this->get_clock()->now();
    time_now = now.seconds() + (now.nanoseconds() * 0.000000001);

    // Check if steering and drive commands received within before timeout
    // if (((time_now - time_last_steer) > timeout) || ((time_now - time_last_drive) > timeout)) {
    //   RCLCPP_DEBUG(this->get_logger(), "Either no steering or no drive command received within last %f seconds. Timeout.", timeout);
    //   steering_cmd = default_steering_cmd;
    //   drive_cmd = default_drive_cmd;
    // }

    // If in enable_drive is set to false, reset drive command to neutral
    if (enable_drive==false) {
      drive_cmd = default_drive_cmd;
    }

    RCLCPP_INFO(this->get_logger(), "Steering cmd: %i", steering_cmd);
    RCLCPP_INFO(this->get_logger(), "Drive cmd: %i", drive_cmd);

    // Write servo commands
    setPWM(fd, drive_pin, 0, convert_microsec(drive_cmd)); // 307 corresponds to approximately 1.5ms pulse width at 50Hz
    setPWM(fd, steer_pin, 0, convert_microsec(steering_cmd)); // 307 corresponds to approximately 1.5ms pulse width at 50Hz
  }

  /// \brief The steering_cmd callback function, updates the steering command
  void steering_cmd_callback(const std_msgs::msg::Int32 & msg)
  {
    rclcpp::Time time;

    // Constrain command to valid range
    if (msg.data > steer_left_max) {
      steering_cmd = cmd_max;
    } else if (msg.data < steer_right_max) {
      steering_cmd = cmd_min;
    } else {
      steering_cmd = msg.data;
    }

    time = this->get_clock()->now();
    time_last_steer = time.seconds() + (time.nanoseconds() * 0.000000001);
  }

  /// \brief The drive_cmd callback function, updates the drive motor command
  void drive_cmd_callback(const std_msgs::msg::Int32 & msg)
  {
    rclcpp::Time time;

    // Constrain command to valid range
    if (msg.data > cmd_max) {
      drive_cmd = cmd_max;
    } else if (msg.data < cmd_min) {
      drive_cmd = cmd_min;
    } else {
      drive_cmd = msg.data;
    }

    // Get time
    time = this->get_clock()->now();
    time_last_drive = time.seconds() + (time.nanoseconds() * 0.000000001);
  }

  /// \brief ESC startup, sends a pulse of servo commands at startup to initialize ESC
  void motor_startup()
  {
    int step, microsec, microsec_max, microsec_min;
    step = 5;
    microsec_min = 1500;
    microsec_max = 1700;
    microsec = microsec_min;

    while (microsec >= microsec_min) {
        microsec += step;
        if (microsec==microsec_max) {
            step = -step;
        }
        pwm = convert_microsec(microsec);
        setPWM(fd, 0, 0, pwm);
        setPWM(fd, 1, 0, pwm);
        delayMicroseconds(50000);
    }
  }

  /// \brief Callback for the enable drive server, enables/disables motors
  void enable_drive_callback(
    std_srvs::srv::SetBool::Request::SharedPtr request,
    std_srvs::srv::SetBool::Response::SharedPtr)
  {
    if (request->data==true) {
      RCLCPP_INFO(this->get_logger(), "Enabling drive motor.");
      enable_drive = true;
    } else {
      RCLCPP_INFO(this->get_logger(), "Disabling drive motor.");
      enable_drive = false;
    }
  }
};

int main(int argc, char ** argv)
{
  rclcpp::init(argc, argv);
  auto node = std::make_shared<Control_Servos>();
  rclcpp::spin(node);
  rclcpp::shutdown();
  return 0;
}
