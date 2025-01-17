/// \file steering_test.cpp
/// \brief Commands the car to drive in a circle
///
/// PARAMETERS:
///     rate (int): the publishing frequency (Hz)
///     angular_velocity (double): the angular velocity of the robot (m)
/// PUBLISHES:
///     cmd_vel (geometry_msgs::msg::Twist): velocity commands for the robot

#include <chrono>

#include "rclcpp/rclcpp.hpp"
#include "std_msgs/msg/u_int64.hpp"
#include "geometry_msgs/msg/twist.hpp"
#include "std_msgs/msg/int32.hpp"

using namespace std::chrono_literals;

class Steering_Test : public rclcpp::Node
{
public:
  Steering_Test()
  : Node("steering_test")
  {
    // Parameters and default values
    declare_parameter("angular_velocity", 2.0);
    declare_parameter("rate", 100);
    declare_parameter("drive_cmd", 1600);

    // Define parameter variables
    angular_velocity = get_parameter("angular_velocity").as_double();
    loop_rate = get_parameter("rate").as_int();
    drive_cmd = get_parameter("drive_cmd").as_int();

    // Publishers
    cmd_vel_pub = create_publisher<geometry_msgs::msg::Twist>("cmd_vel", 10);
    drive_cmd_pub = create_publisher<std_msgs::msg::Int32>("drive_cmd", 10);

    // Main timer
    int cycle_time = 1000.0 / loop_rate;
    main_timer = this->create_wall_timer(
      std::chrono::milliseconds(cycle_time),
      std::bind(&Steering_Test::timer_callback, this));
  }

private:
  // Initialize parameter variables
  int rate;
  double angular_velocity;
  int loop_rate, drive_cmd;
  int state = 1;

  rclcpp::Publisher<geometry_msgs::msg::Twist>::SharedPtr cmd_vel_pub;
  rclcpp::Publisher<std_msgs::msg::Int32>::SharedPtr drive_cmd_pub;
  rclcpp::TimerBase::SharedPtr main_timer;

  /// \brief The main timer callback, publishes velocity commands
  void timer_callback()
  {
    geometry_msgs::msg::Twist cmd_vel_msg;
    std_msgs::msg::Int32 drive_cmd_msg;

    // cmd_vel and servo commands
    cmd_vel_msg.angular.z = angular_velocity;
    drive_cmd_msg.data = drive_cmd;
    
    // Publish commands
    cmd_vel_pub->publish(cmd_vel_msg);
    drive_cmd_pub->publish(drive_cmd_msg);
  }
};

int main(int argc, char ** argv)
{
  rclcpp::init(argc, argv);
  auto node = std::make_shared<Steering_Test>();
  rclcpp::spin(node);
  rclcpp::shutdown();
  return 0;
}
