#include "turn_on_robot/base_node.h"
#include "rclcpp/rclcpp.hpp"
#include "geometry_msgs/msg/twist.hpp"
#include "geometry_msgs/msg/transform_stamped.hpp"
#include "serial/serial.h"
#include "nav_msgs/msg/odometry.hpp"
#include "tf2/LinearMath/Quaternion.h"
#include "tf2_geometry_msgs/tf2_geometry_msgs.h"
#include "sensor_msgs/msg/imu.hpp"
#include "ICM20948.h"

#include <math.h>
#include <chrono>
#include <thread>
#include <future>

#define FRAME_HEADER      0X7B       // Frame header
#define FRAME_TAIL        0X7D       // Frame tail
#define SEND_DATA_SIZE    9          // The length of data sent by ROS to the lower machine 
#define RECEIVE_DATA_SIZE    11          // The length of data sent by the lower machine to ROS

// wheel radius
#define wheel_radius (3.0/100) // unit: m
// half of the distance between front wheels
#define wheel_spacing (19.5/2.0/100) // unit: m
// half of the distance between front wheel and rear wheel
#define axle_spacing (15.1/2.0/100) // unit: m


class BaseNode : public rclcpp::Node
{
  public:
    BaseNode()
    :Node("base_node")
    {
      rclcpp::QoS qos(rclcpp::KeepLast(1));
      cmd_vel_sub = this->create_subscription<geometry_msgs::msg::Twist>("cmd_vel", qos, std::bind(&BaseNode::cmd_vel_callback, this, std::placeholders::_1));
      odom_publisher = this->create_publisher<nav_msgs::msg::Odometry>("odom", rclcpp::SensorDataQoS());
      imu_publisher = this->create_publisher<sensor_msgs::msg::Imu>("imu", rclcpp::SensorDataQoS());
      // set up serial connection with arduino uno
      try{
        arduino_serial.setPort("/dev/ttyACM0");
        arduino_serial.setBaudrate(38400);
        serial::Timeout _time = serial::Timeout::simpleTimeout(2000);
        arduino_serial.setTimeout(_time);
        arduino_serial.open();
        arduino_serial.setDTR();
        arduino_serial.setRTS();
      }
      catch(serial::IOException& e){
        RCLCPP_ERROR(this->get_logger(), e.what());
        RCLCPP_ERROR(this->get_logger(), "An error occurred while establishing serial connection with Arduino uno.");
      }

      // create a polling thread for receiving data from arduino uno and publishing topics 
      if(arduino_serial.isOpen()){
        RCLCPP_INFO(this->get_logger(), "arduino serial port opened");
        future_ = exit_signal_.get_future();
        poll_thread_ = std::thread(&BaseNode::pollThread, this);
      }

      // initialize IMU
      imuInit(&enMotionSensorType);
      if(IMU_EN_SENSOR_TYPE_ICM20948 == enMotionSensorType){
        RCLCPP_INFO(this->get_logger(), "Motion sersor is ICM-20948");
      }
      else{
        RCLCPP_INFO(this->get_logger(), "Motion sersor NULL");
      }
      sleep(3);
      imuDataGet( &stAngles, &stGyroRawData, &stAccelRawData, &stMagnRawData);
      if(fabs(stAccelRawData.fY-(-1))>0.2){
        RCLCPP_WARN(this->get_logger(), "imu data error %f",fabs(stAccelRawData.fY-(-1)));
      }
      arduino_serial.flushInput();
    }
    ~BaseNode()
    {
      exit_signal_.set_value();
      poll_thread_.join();
      arduino_serial.close();
    }
  private:
    void pollThread()
    {
      std::future_status status;
      int recv_count = 0;
      bool start_frame = false;
      uint8_t recv_data[RECEIVE_DATA_SIZE]={0}; //Temporary variable to save the data of the lower machine //临时变量，保存下位机数据
      uint8_t t=0;
      float px=0;
      float py=0;
      float pz=0;
      std::chrono::steady_clock::time_point last_time = std::chrono::steady_clock::now();
      do {
        if(arduino_serial.available()){//RCLCPP_INFO(this->get_logger(), "a");}
          arduino_serial.read(&t, 1);
          //RCLCPP_INFO(this->get_logger(), "t= %d", t);
          if(start_frame){
            recv_data[recv_count++]=t;
          }
          else if(t==FRAME_HEADER)
          {
            start_frame = true;
            recv_data[recv_count++]=t;
          }
          if(recv_count==RECEIVE_DATA_SIZE){
            recv_count = 0;
            start_frame = false;
            //RCLCPP_INFO(this->get_logger(), "end frame");
            if(t==FRAME_TAIL){
              // finish receiving a frame
              if(recv_data[9]==(recv_data[0]^recv_data[1]^recv_data[2]^recv_data[3]^recv_data[4]^recv_data[5]^recv_data[6]^recv_data[7]^recv_data[8])){
                // received data frame correct
                //RCLCPP_INFO(this->get_logger(), "data correct");
                float _1 = (int16_t)((recv_data[1]<<8)|recv_data[2])/1000.0;
                float _2 = (int16_t)((recv_data[3]<<8)|recv_data[4])/1000.0;
                float _3 = (int16_t)((recv_data[5]<<8)|recv_data[6])/1000.0;
                float _4 = (int16_t)((recv_data[7]<<8)|recv_data[8])/1000.0;
                //RCLCPP_INFO(this->get_logger(), "%f < %f < %f < %f",_1,_2,_3,_4);
                float sampling_time = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-last_time).count()/1000000.0;
                last_time = std::chrono::steady_clock::now();
                float x = wheel_radius*((_2+_1+_4+_3)/4.0);
                float y = wheel_radius*((_2-_1+_4-_3)/4.0);
                float z = wheel_radius*((-_2-_1+_4+_3)/4.0/(axle_spacing+wheel_spacing));
                //RCLCPP_INFO(this->get_logger(),"%f %f %f",x,y,z);
                
                //RCLCPP_INFO(this->get_logger(),"%f",sampling_time);
                px+=((x * cos(pz) - y * sin(pz)) * sampling_time); //Calculate the displacement in the X direction, unit: m 
                py+=((x * sin(pz) + y * cos(pz)) * sampling_time); //Calculate the displacement in the Y direction, unit: m 
                pz+=(z * sampling_time); //The angular displacement about the Z axis, in rad 
                //RCLCPP_INFO(this->get_logger(),"p %f %f %f",px,py,pz);

                tf2::Quaternion odom_quat;
                odom_quat.setRPY(0,0,pz);
                nav_msgs::msg::Odometry odom;
                odom.header.stamp = this->now();
                odom.header.frame_id = "odom";
                odom.pose.pose.position.x = px;
                odom.pose.pose.position.y = py;
                odom.pose.pose.position.z = 0.0;
                odom.pose.pose.orientation = tf2::toMsg(odom_quat);
                odom.child_frame_id = "base_link";
                odom.twist.twist.linear.x = x;
                odom.twist.twist.linear.y = y;
                odom.twist.twist.angular.z = z;

                if(x== 0&&y== 0&&z== 0){
                  //如果小車速度是零，說明編碼器的誤差會比較小，認為編碼器數據更可靠
                  memcpy(&odom.pose.covariance, odom_pose_covariance2, sizeof(odom_pose_covariance2)),
                  memcpy(&odom.twist.covariance, odom_twist_covariance2, sizeof(odom_twist_covariance2));
                }
                else{
                  //如果小車速度非零，考慮到運動中編碼器可能帶來的滑動誤差，認為imu的數據更可靠
                  memcpy(&odom.pose.covariance, odom_pose_covariance, sizeof(odom_pose_covariance)),
                  memcpy(&odom.twist.covariance, odom_twist_covariance, sizeof(odom_twist_covariance));       
                }
                odom_publisher->publish(odom);
                //RCLCPP_INFO(this->get_logger(), "odom publised");

                // publish imu data
                imuDataGet( &stAngles, &stGyroRawData, &stAccelRawData, &stMagnRawData);
                sensor_msgs::msg::Imu imu_data;
                imu_data.header.stamp = this->now();
                imu_data.header.frame_id = "imu_link";
                tf2::Quaternion imu_quat;
                imu_data.orientation = tf2::toMsg(imu_quat);
                imu_data.orientation_covariance[0] = -1;
                imu_data.angular_velocity.x = (stGyroRawData.fZ)/57.3;
                imu_data.angular_velocity.y = -(stGyroRawData.fX)/57.3;
                imu_data.angular_velocity.z = -(stGyroRawData.fY-(-0.061))/57.3;
                imu_data.angular_velocity_covariance[0] = 1e-6;
                imu_data.angular_velocity_covariance[4] = 1e-6;
                imu_data.angular_velocity_covariance[8] = 1e-6;
                imu_data.linear_acceleration.x = (stAccelRawData.fZ-(-0.033))*9.81;
                imu_data.linear_acceleration.y = -(stAccelRawData.fX-(0.039))*9.81;
                imu_data.linear_acceleration.z = -(stAccelRawData.fY)*9.81;
                imu_data.linear_acceleration_covariance[0] = 1e-6;
                imu_data.linear_acceleration_covariance[4] = 1e-6;
                imu_data.linear_acceleration_covariance[8] = 1e-6;
                imu_publisher->publish(imu_data);
              }else{
                RCLCPP_INFO(this->get_logger(), "data error");
              }
            }
          }
        }
        status = future_.wait_for(std::chrono::seconds(0));
      } while (status == std::future_status::timeout);
    }
    
    void cmd_vel_callback(const geometry_msgs::msg::Twist::ConstSharedPtr vel)
    {
      //RCLCPP_INFO(this->get_logger(), "cmd_vel msg received");
      int16_t transition;
  
      uint8_t send_data[SEND_DATA_SIZE];
      send_data[0] = FRAME_HEADER;

      transition = vel->linear.x*1000;
      send_data[2] = transition;
      send_data[1] = transition>>8;
      
      transition = vel->linear.y*1000;
      send_data[4] = transition;
      send_data[3] = transition>>8;
      
      transition = vel->angular.z*1000;
      send_data[6] = transition;
      send_data[5] = transition>>8;

      send_data[7] = send_data[0]^send_data[1]^send_data[2]^send_data[3]^send_data[4]^send_data[5]^send_data[6];
      send_data[8] = FRAME_TAIL;

      try{
        arduino_serial.write(send_data, SEND_DATA_SIZE); // Send data to arduino uno via serial port 
      }catch (serial::IOException& e){
        RCLCPP_ERROR(this->get_logger(), e.what());
        RCLCPP_ERROR(this->get_logger(),"Unable to send data through serial port"); // If sending data fails, an error message is printed
      }
    }

    rclcpp::Publisher<nav_msgs::msg::Odometry>::SharedPtr odom_publisher;
    rclcpp::Subscription<geometry_msgs::msg::Twist>::SharedPtr cmd_vel_sub;
    rclcpp::Publisher<sensor_msgs::msg::Imu>::SharedPtr imu_publisher;
    serial::Serial arduino_serial;
    std::thread poll_thread_;
    std::shared_future<void> future_;
    std::promise<void> exit_signal_;
    IMU_EN_SENSOR_TYPE enMotionSensorType;
    IMU_ST_ANGLES_DATA stAngles;
    IMU_ST_SENSOR_DATA stGyroRawData;
    IMU_ST_SENSOR_DATA stAccelRawData;
    IMU_ST_SENSOR_DATA stMagnRawData;
};

int main(int argc, char * argv[])
{
  rclcpp::init(argc, argv);
  rclcpp::spin(std::make_shared<BaseNode>());
  rclcpp::shutdown();
  return 0;
}
